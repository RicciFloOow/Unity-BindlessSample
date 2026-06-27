// ============================================================================
// Unity Native Plugin: D3D12 RootSignature & Descriptor Heap Injection
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "includes/IUnityInterface.h"
#include "includes/IUnityGraphics.h"
#include "includes/IUnityGraphicsD3D12.h"

// ---------------------------------------------------------------------------
// Global state & VTable Bookkeeping
// ---------------------------------------------------------------------------
static IUnityInterfaces* g_UnityInterfaces = nullptr;
static ID3D12Device* g_D3D12Device = nullptr;

using CreateRootSignature_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
static CreateRootSignature_t g_OriginalCreateRootSignature = nullptr;
static constexpr size_t kCreateRootSignatureVTableIndex = 16;

using CreateDescriptorHeap_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
static CreateDescriptorHeap_t g_OriginalCreateDescriptorHeap = nullptr;
static constexpr size_t kCreateDescriptorHeapVTableIndex = 14;

static ID3D12DescriptorHeap* g_pCBVSRVUAVHeap = nullptr;
static UINT                  g_HeapIncrementSize = 0;
static SIZE_T                g_HeapStartCPUHandle = 0;
static UINT                  g_OriginalHeapSize = 0;

static ID3D12DescriptorHeap* g_StagingHeap = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE g_StagingHeapHandle = { 0 };

static void** g_OriginalVTable = nullptr;
static void** g_CopiedVTable = nullptr;
static void*** g_pVTablePtr = nullptr;
static bool    g_HookInstalled = false;
static constexpr size_t kDeviceVTableSize = 256;

static constexpr UINT BINDLESS_CAPACITY = 262144;//虽然正常用不上这么大

#if !defined(ROOTSIG_HOOK_NO_LOG)
#define RS_LOG(fmt, ...) do { char _buf[512]; sprintf_s(_buf, "[BindlessHook] " fmt "\n", ##__VA_ARGS__); OutputDebugStringA(_buf); } while(0)
#else
#define RS_LOG(fmt, ...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookedCreateRootSignature(
    ID3D12Device* pDevice, UINT nodeMask, const void* pBlobWithRootSignature,
    SIZE_T blobLengthInBytes, REFIID riid, void** ppvRootSignature)
{
    Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> pVersionedDeserializer;
    HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer(
        pBlobWithRootSignature, blobLengthInBytes, IID_PPV_ARGS(&pVersionedDeserializer));

    if (FAILED(hr)) return g_OriginalCreateRootSignature(pDevice, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);

    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pVersionedDesc = pVersionedDeserializer->GetUnconvertedRootSignatureDesc();
    if (!pVersionedDesc) return g_OriginalCreateRootSignature(pDevice, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);

    D3D12_ROOT_SIGNATURE_FLAGS originalFlags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (pVersionedDesc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) originalFlags = pVersionedDesc->Desc_1_0.Flags;
    else if (pVersionedDesc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) originalFlags = pVersionedDesc->Desc_1_1.Flags;
    else return g_OriginalCreateRootSignature(pDevice, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);

    if (originalFlags & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)
    {
        return g_OriginalCreateRootSignature(pDevice, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
    }

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC modifiedVersioned = *pVersionedDesc;
    const D3D12_ROOT_SIGNATURE_FLAGS newFlags = originalFlags | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    if (modifiedVersioned.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) modifiedVersioned.Desc_1_0.Flags = newFlags;
    else modifiedVersioned.Desc_1_1.Flags = newFlags;

    Microsoft::WRL::ComPtr<ID3DBlob> pNewBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> pErrorBlob;
    hr = D3D12SerializeVersionedRootSignature(&modifiedVersioned, &pNewBlob, &pErrorBlob);

    if (FAILED(hr)) return g_OriginalCreateRootSignature(pDevice, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
    return g_OriginalCreateRootSignature(pDevice, nodeMask, pNewBlob->GetBufferPointer(), pNewBlob->GetBufferSize(), riid, ppvRootSignature);
}

static HRESULT STDMETHODCALLTYPE HookedCreateDescriptorHeap(
    ID3D12Device* pDevice, const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
    REFIID riid, void** ppvHeap)
{
    D3D12_DESCRIPTOR_HEAP_DESC modifiedDesc = *pDescriptorHeapDesc;

    bool isShaderVisibleHeap = (modifiedDesc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) && (modifiedDesc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

    if (isShaderVisibleHeap)
    {
        g_OriginalHeapSize = modifiedDesc.NumDescriptors;
        modifiedDesc.NumDescriptors += BINDLESS_CAPACITY;
    }

    HRESULT hr = g_OriginalCreateDescriptorHeap(pDevice, &modifiedDesc, riid, ppvHeap);

    if (SUCCEEDED(hr) && isShaderVisibleHeap)
    {
        g_pCBVSRVUAVHeap = static_cast<ID3D12DescriptorHeap*>(*ppvHeap);
        g_HeapIncrementSize = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_HeapStartCPUHandle = g_pCBVSRVUAVHeap->GetCPUDescriptorHandleForHeapStart().ptr;

        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
        nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrvDesc.Texture2D.MipLevels = 1;
        nullSrvDesc.Texture2D.MostDetailedMip = 0;
        nullSrvDesc.Texture2D.PlaneSlice = 0;
        nullSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        for (UINT i = 0; i < BINDLESS_CAPACITY; ++i)
        {
            SIZE_T ptrOffset = static_cast<SIZE_T>(g_OriginalHeapSize + i) * g_HeapIncrementSize;
            D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle = { g_HeapStartCPUHandle + ptrOffset };

            pDevice->CreateShaderResourceView(nullptr, &nullSrvDesc, descriptorHandle);
        }

        RS_LOG("Intercepted and Expanded CBV/SRV/UAV Heap! Old Size: %d, New Size: %d", g_OriginalHeapSize, modifiedDesc.NumDescriptors);
    }
    return hr;
}

// ---------------------------------------------------------------------------
// VTable Management
// ---------------------------------------------------------------------------
static bool InstallVTableHook(ID3D12Device* pDevice)
{
    if (!pDevice) return false;
    g_pVTablePtr = reinterpret_cast<void***>(pDevice);
    g_OriginalVTable = *g_pVTablePtr;

    g_OriginalCreateRootSignature = reinterpret_cast<CreateRootSignature_t>(g_OriginalVTable[kCreateRootSignatureVTableIndex]);
    g_OriginalCreateDescriptorHeap = reinterpret_cast<CreateDescriptorHeap_t>(g_OriginalVTable[kCreateDescriptorHeapVTableIndex]);

    DWORD oldProtect = 0;

    if (VirtualProtect(&g_OriginalVTable[kCreateRootSignatureVTableIndex], sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        g_OriginalVTable[kCreateRootSignatureVTableIndex] = reinterpret_cast<void*>(&HookedCreateRootSignature);
        VirtualProtect(&g_OriginalVTable[kCreateRootSignatureVTableIndex], sizeof(void*), oldProtect, &oldProtect);
    }

    if (VirtualProtect(&g_OriginalVTable[kCreateDescriptorHeapVTableIndex], sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        g_OriginalVTable[kCreateDescriptorHeapVTableIndex] = reinterpret_cast<void*>(&HookedCreateDescriptorHeap);
        VirtualProtect(&g_OriginalVTable[kCreateDescriptorHeapVTableIndex], sizeof(void*), oldProtect, &oldProtect);
    }

    RS_LOG("VTable hook installed safely via slot override");
    return true;
}

static void UninstallVTableHook()
{
    if (!g_OriginalVTable) return;
    DWORD oldProtect = 0;

    if (VirtualProtect(&g_OriginalVTable[kCreateRootSignatureVTableIndex], sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        g_OriginalVTable[kCreateRootSignatureVTableIndex] = reinterpret_cast<void*>(g_OriginalCreateRootSignature);
        VirtualProtect(&g_OriginalVTable[kCreateRootSignatureVTableIndex], sizeof(void*), oldProtect, &oldProtect);
    }

    if (VirtualProtect(&g_OriginalVTable[kCreateDescriptorHeapVTableIndex], sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        g_OriginalVTable[kCreateDescriptorHeapVTableIndex] = reinterpret_cast<void*>(g_OriginalCreateDescriptorHeap);
        VirtualProtect(&g_OriginalVTable[kCreateDescriptorHeapVTableIndex], sizeof(void*), oldProtect, &oldProtect);
    }

    g_HookInstalled = false;
    g_pCBVSRVUAVHeap = nullptr;
}

// ---------------------------------------------------------------------------
// Bindless Event Execution (Render Thread)
// ---------------------------------------------------------------------------
struct BindlessEventData
{
    ID3D12Resource* pTexture;
    uint32_t        index;

    uint32_t        isBuffer;          // 0: Texture, 1: Buffer
    uint32_t        bufferNumElements; // Buffer 元素总数
    uint32_t        bufferStride;      // 结构体步长 (字节)
    uint32_t        isRawBuffer;       // 1: ByteAddressBuffer
};

// Event IDs
constexpr int kEventRegisterSRV = 1;
constexpr int kEventRegisterUAV = 2;
constexpr int kEventUnregister = 3; // Generic unregister (Null Descriptor)
constexpr int kEventForceDependencyTracking = 4;

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API OnRenderEventAndData(int eventID, void* data)
{
    if (!g_pCBVSRVUAVHeap || !g_D3D12Device || !g_UnityInterfaces || !data) return;

    IUnityGraphicsD3D12v8* pD3D12 = g_UnityInterfaces->Get<IUnityGraphicsD3D12v8>();
    if (!pD3D12) return;

    BindlessEventData* pEventData = static_cast<BindlessEventData*>(data);
    if (pEventData->index >= g_pCBVSRVUAVHeap->GetDesc().NumDescriptors) return; // Out of bounds safety

    const SIZE_T ptrOffset = static_cast<SIZE_T>(pEventData->index) * g_HeapIncrementSize;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle = { g_HeapStartCPUHandle + ptrOffset };

    if (eventID == kEventRegisterSRV && pEventData->pTexture)
    {
        D3D12_RESOURCE_STATES targetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        pD3D12->RequestResourceState(pEventData->pTexture, targetState);
        pD3D12->NotifyResourceState(pEventData->pTexture, targetState, false);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        if (pEventData->isBuffer == 1)
        {
            //Buffer
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = pEventData->bufferNumElements;
            srvDesc.Buffer.StructureByteStride = pEventData->bufferStride;

            if (pEventData->isRawBuffer == 1)
            {
                //ByteAddressBuffer
                srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            }
            else
            {
                //StructuredBuffer
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            }
        }
        else
        {
            srvDesc.Format = pEventData->pTexture->GetDesc().Format;
            //@TODO: SM6.10+时理论上没必要有这个操作了, 可以提供一个额外的EventRegister
            switch (srvDesc.Format)
            {
            case DXGI_FORMAT_D16_UNORM: srvDesc.Format = DXGI_FORMAT_R16_UNORM; break;
            case DXGI_FORMAT_D24_UNORM_S8_UINT: srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
            case DXGI_FORMAT_D32_FLOAT: srvDesc.Format = DXGI_FORMAT_R32_FLOAT; break;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
            default: break;
            }
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = -1;
        }

        g_D3D12Device->CreateShaderResourceView(pEventData->pTexture, &srvDesc, g_StagingHeapHandle);
        g_D3D12Device->CopyDescriptorsSimple(1, descriptorHandle, g_StagingHeapHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    else if (eventID == kEventRegisterUAV && pEventData->pTexture)
    {
        D3D12_RESOURCE_STATES targetState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        pD3D12->RequestResourceState(pEventData->pTexture, targetState);
        pD3D12->NotifyResourceState(pEventData->pTexture, targetState, true);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = pEventData->pTexture->GetDesc().Format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        g_D3D12Device->CreateUnorderedAccessView(pEventData->pTexture, nullptr, &uavDesc, g_StagingHeapHandle);
        g_D3D12Device->CopyDescriptorsSimple(1, descriptorHandle, g_StagingHeapHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    else if (eventID == kEventUnregister)
    {
        // Null Descriptor to prevent GPU reading dangling pointers (TDR protection)
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
        nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // Must be valid format for Null Descriptor
        nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        nullSrvDesc.Texture2D.MipLevels = 1;
        nullSrvDesc.Texture2D.MostDetailedMip = 0;
        nullSrvDesc.Texture2D.PlaneSlice = 0;
        nullSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        g_D3D12Device->CreateShaderResourceView(nullptr, &nullSrvDesc, g_StagingHeapHandle);
        g_D3D12Device->CopyDescriptorsSimple(1, descriptorHandle, g_StagingHeapHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    else if (eventID == kEventForceDependencyTracking && pEventData->pTexture)
    {
        D3D12_RESOURCE_STATES targetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        pD3D12->RequestResourceState(pEventData->pTexture, targetState);
    }
}

// ---------------------------------------------------------------------------
// Exported C# APIs
// ---------------------------------------------------------------------------
extern "C" uint32_t UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetSRVDescriptorHeapCount()
{
    return g_pCBVSRVUAVHeap ? g_pCBVSRVUAVHeap->GetDesc().NumDescriptors : 0;
}

extern "C" UnityRenderingEventAndData UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetRenderEventAndDataFunc()
{
    return OnRenderEventAndData;
}

extern "C" uint32_t UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetBindlessHeapOffset()
{
    return g_OriginalHeapSize;
}

extern "C" bool UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API IsGraphicsDebuggerAttached()
{
    if (GetModuleHandleW(L"ngfx-injection.dll") != nullptr) return true;

    if (GetModuleHandleW(L"renderdoc.dll") != nullptr) return true;

    if (GetModuleHandleW(L"WinPixGpuCapturer.dll") != nullptr) return true;

    if (GetModuleHandleW(L"gpa-capture64.dll") != nullptr ||
        GetModuleHandleW(L"gpa-injector64.dll") != nullptr)
    {
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Unity Plugin Lifecycle
// ---------------------------------------------------------------------------
static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
    if (eventType == kUnityGfxDeviceEventInitialize && g_UnityInterfaces)
    {
        IUnityGraphics* pGraphics = g_UnityInterfaces->Get<IUnityGraphics>();
        if (pGraphics && pGraphics->GetRenderer() == kUnityGfxRendererD3D12)
        {
            IUnityGraphicsD3D12v8* pD3D12 = g_UnityInterfaces->Get<IUnityGraphicsD3D12v8>();
            if (pD3D12 && !g_HookInstalled)
            {
                g_D3D12Device = pD3D12->GetDevice();

                D3D12_DESCRIPTOR_HEAP_DESC stagingDesc = {};
                stagingDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                stagingDesc.NumDescriptors = 1;
                stagingDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                g_D3D12Device->CreateDescriptorHeap(&stagingDesc, IID_PPV_ARGS(&g_StagingHeap));
                g_StagingHeapHandle = g_StagingHeap->GetCPUDescriptorHandleForHeapStart();

                g_HookInstalled = InstallVTableHook(g_D3D12Device);

                // Configure Events to allow Command Buffer modifications without explicit flushes
                UnityD3D12PluginEventConfig eventConfig = {};
                eventConfig.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
                eventConfig.flags = kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
                eventConfig.ensureActiveRenderTextureIsBound = false;

                pD3D12->ConfigureEvent(kEventRegisterSRV, &eventConfig);
                pD3D12->ConfigureEvent(kEventRegisterUAV, &eventConfig);
                pD3D12->ConfigureEvent(kEventUnregister, &eventConfig);
            }
        }
    }
    else if (eventType == kUnityGfxDeviceEventShutdown)
    {
        if (g_StagingHeap) { g_StagingHeap->Release(); g_StagingHeap = nullptr; }
        if (g_HookInstalled) UninstallVTableHook();
        g_D3D12Device = nullptr;
    }
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
    g_UnityInterfaces = unityInterfaces;
    IUnityGraphics* pGraphics = unityInterfaces->Get<IUnityGraphics>();
    if (pGraphics) pGraphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);

    // Catch manual load scenario if initialized before plugin load
    OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
    if (g_UnityInterfaces)
    {
        IUnityGraphics* pGraphics = g_UnityInterfaces->Get<IUnityGraphics>();
        if (pGraphics) pGraphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
    }
    if (g_HookInstalled) UninstallVTableHook();
    g_UnityInterfaces = nullptr;
}

// ---------------------------------------------------------------------------
// Emergency Cleanup API (For App Shutdown & Editor Play Mode Exit)
// ---------------------------------------------------------------------------
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API EmergencyClearBindlessHeap()
{
    if (!g_pCBVSRVUAVHeap || !g_D3D12Device)
    {
        RS_LOG("EmergencyClearBindlessHeap skipped: Heap or Device not initialized.");
        return;
    }

    uint32_t maxCount = g_pCBVSRVUAVHeap->GetDesc().NumDescriptors;

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
    nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrvDesc.Texture2D.MipLevels = 1;
    nullSrvDesc.Texture2D.MostDetailedMip = 0;
    nullSrvDesc.Texture2D.PlaneSlice = 0;
    nullSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    for (uint32_t i = 0; i < BINDLESS_CAPACITY; ++i)
    {
        uint32_t actualIndex = g_OriginalHeapSize + i;
        const SIZE_T ptrOffset = static_cast<SIZE_T>(actualIndex) * g_HeapIncrementSize;
        D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle = { g_HeapStartCPUHandle + ptrOffset };
        g_D3D12Device->CreateShaderResourceView(nullptr, &nullSrvDesc, descriptorHandle);
    }

    RS_LOG("Emergency cleanup executed: %u descriptors cleared to Null.", maxCount);
}