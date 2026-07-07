using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Unity.Collections;
using Unity.Collections.LowLevel.Unsafe;
using UnityEngine;
using UnityEngine.Rendering;

namespace UniBindlessSample
{
    public class BindlessResourceManager : IDisposable
    {
        [DllImport("GfxPluginUniBindlessNativePlugin")]
        private static extern IntPtr GetRenderEventAndDataFunc();

        [DllImport("GfxPluginUniBindlessNativePlugin")]
        private static extern void EmergencyClearBindlessHeap();

        [DllImport("GfxPluginUniBindlessNativePlugin")]
        private static extern uint GetBindlessHeapOffset();

        [DllImport("GfxPluginUniBindlessNativePlugin")]
        private static extern bool IsGraphicsDebuggerAttached();

        private uint m_heapOffset = 0;

        private static readonly int kBindlessEventDataStride = Marshal.SizeOf<BindlessEventData>();

        private CommandBuffer m_cmd;

        private const int kEventRegisterSRV = 1;
        private const int kEventRegisterUAV = 2;
        private const int kEventUnregister = 3;
        private const int kEventForceDependencyTracking = 4;

        public bool RequiresDependencyTracking => m_requiresDependencyTracking;
        private bool m_requiresDependencyTracking = false;

        #region ---- Structs & Keys ----

        private struct ResourceKey : IEquatable<ResourceKey>
        {
            public int instanceId;
            public bool isUAV;

            public bool Equals(ResourceKey other) => instanceId == other.instanceId && isUAV == other.isUAV;
            public override int GetHashCode() => HashCode.Combine(instanceId, isUAV);
        }

        private class ResourceMeta
        {
            public uint index;
            public int refCount;
        }

        #endregion

        #region ---- Index Allocator ----
        private Queue<uint> m_freeIndices;
        private Queue<(uint index, int releaseFrame)> m_pendingRelease;
        private int m_maxQueuedFrames = 3;
        #endregion

        #region ---- 0GC Event Data Pool ----
        private const int MAX_FRAMES_IN_FLIGHT = 4;//注意, 必须大于等于QualitySettings.maxQueuedFrames+1
        private NativeArray<BindlessEventData>[] m_eventDataPools;
        private int[] m_currentEventDataIndices;
        private int m_currentFrameIndex = 0;
        private const int MAX_EVENTS_PER_FRAME = 16384;
        #endregion

        #region ---- Reference Counting & Deferred Release ----
        private Dictionary<ResourceKey, ResourceMeta> m_resourceMap = new Dictionary<ResourceKey, ResourceMeta>();
        private Dictionary<uint, ResourceKey> m_indexToResourceMap = new Dictionary<uint, ResourceKey>();
        private HashSet<ResourceKey> m_pendingZeroRef = new HashSet<ResourceKey>();
        #endregion

        #region ---- Constructor ----
        public BindlessResourceManager(uint capacity)
        {
            m_freeIndices = new Queue<uint>((int)capacity);
            for (uint i = 0; i < capacity; i++)
                m_freeIndices.Enqueue(i);

            m_pendingRelease = new Queue<(uint, int)>();
            m_eventDataPools = new NativeArray<BindlessEventData>[MAX_FRAMES_IN_FLIGHT];
            m_currentEventDataIndices = new int[MAX_FRAMES_IN_FLIGHT];
            for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
            {
                m_eventDataPools[i] = new NativeArray<BindlessEventData>(MAX_EVENTS_PER_FRAME, Allocator.Persistent);
                m_currentEventDataIndices[i] = 0;
            }

            m_cmd = new CommandBuffer()
            {
                name = "BindlessResBindingPass"
            };

            try
            {
                m_heapOffset = GetBindlessHeapOffset();
                Debug.Log($"[Bindless] 获取到堆偏移: {m_heapOffset}");
            }
            catch (Exception eh)
            {
                Debug.LogWarning($"[Bindless] 无法获取堆偏移: {eh.Message}");
            }

            try
            {
                m_requiresDependencyTracking = IsGraphicsDebuggerAttached();
            }
            catch (Exception eg)
            {
                Debug.LogWarning($"[Bindless] 无法检测到是否附加Debug工具: {eg.Message}");
            }
        }
        #endregion

        #region ---- Public Interfaces ----

        public void UpdateFrame()
        {
            int poolIndex = m_currentFrameIndex % MAX_FRAMES_IN_FLIGHT;

            foreach (var key in m_pendingZeroRef)
            {
                if (m_resourceMap.TryGetValue(key, out var meta) && meta.refCount == 0)
                {
                    uint index = meta.index;
                    m_pendingRelease.Enqueue((index, Time.frameCount));

                    m_resourceMap.Remove(key);
                    m_indexToResourceMap.Remove(index);
                }
            }
            m_pendingZeroRef.Clear();

            while (m_pendingRelease.Count > 0)
            {
                if (m_currentEventDataIndices[poolIndex] >= MAX_EVENTS_PER_FRAME)
                {
                    Debug.LogWarning($"[Bindless] 单帧事件超限 ({MAX_EVENTS_PER_FRAME})，剩余延迟释放任务推迟到下一帧!");
                    break;
                }

                var pending = m_pendingRelease.Peek();

                if (Time.frameCount >= pending.releaseFrame + m_maxQueuedFrames + 1)
                {
                    var readyToRelease = m_pendingRelease.Dequeue();

                    var data = new BindlessEventData { index = readyToRelease.index };
                    SubmitEventUnsafe(m_cmd, kEventUnregister, data);

                    m_freeIndices.Enqueue(readyToRelease.index);
                }
                else
                {
                    break;
                }
            }

            if (m_currentEventDataIndices[poolIndex] > 0)
            {
                Graphics.ExecuteCommandBuffer(m_cmd);
                m_cmd.Clear();
            }

            m_currentFrameIndex++;
            m_currentEventDataIndices[m_currentFrameIndex % MAX_FRAMES_IN_FLIGHT] = 0;
        }

        public uint RegisterTexture(Texture texture, bool isReadonly = true)
        {
            var data = new BindlessEventData { pResource = texture.GetNativeTexturePtr(), isBuffer = 0 };
            return HandleRegister(texture.GetInstanceID(), data, isReadonly);
        }

        public uint RegisterBuffer(GfxBufferHandle bufferHandle, bool isReadonly = true)
        {
            if (bufferHandle == null || bufferHandle.GetNativePtr() == IntPtr.Zero)
            {
                Debug.LogError("[Bindless] 尝试注册一个空或已释放的 BufferHandle!");
                return uint.MaxValue;
            }

            var data = new BindlessEventData
            {
                pResource = bufferHandle.GetNativePtr(),
                isBuffer = 1,

                isRawBuffer = bufferHandle.IsRawBuffer ? 1u : 0u,

                bufferNumElements = bufferHandle.IsRawBuffer ? (uint)(bufferHandle.Count * bufferHandle.Stride / 4) : (uint)bufferHandle.Count,

                bufferStride = bufferHandle.IsRawBuffer ? 0 : (uint)bufferHandle.Stride
            };

            return HandleRegister(bufferHandle.InstanceID, data, isReadonly);
        }

        public void Unregister(uint index)
        {
            if (m_indexToResourceMap.TryGetValue(index, out ResourceKey key))
            {
                if (m_resourceMap.TryGetValue(key, out var meta))
                {
                    if (meta.refCount <= 0)
                    {
                        Debug.LogWarning($"[Bindless] 尝试重复卸载已归零的资源索引: {index}");
                        return;
                    }

                    meta.refCount--;

                    if (meta.refCount == 0)
                    {
                        m_pendingZeroRef.Add(key);
                    }
                }
            }
            else
            {
                Debug.LogWarning($"[Bindless] 尝试卸载一个未注册或已回收的非法索引: {index}");
            }
        }

        public void ForceDependencyTrackingUnsafe(CommandBuffer cmd, IntPtr nativeTexturePtr)
        {
            if (nativeTexturePtr == IntPtr.Zero) return;

            var data = new BindlessEventData
            {
                pResource = nativeTexturePtr
            };

            SubmitEventUnsafe(cmd, kEventForceDependencyTracking, data);
        }
        #endregion

        #region ---- Private Interfaces ----

        private uint HandleRegister(int instanceId, BindlessEventData data, bool isReadonly)
        {
            var key = new ResourceKey { instanceId = instanceId, isUAV = !isReadonly };

            if (m_resourceMap.TryGetValue(key, out var meta))
            {
                meta.refCount++;
                if (meta.refCount == 1)
                {
                    m_pendingZeroRef.Remove(key);
                }
                return meta.index;
            }

            int poolIndex = m_currentFrameIndex % MAX_FRAMES_IN_FLIGHT;
            if (m_currentEventDataIndices[poolIndex] >= MAX_EVENTS_PER_FRAME)
            {
                Debug.LogError("[Bindless] 注册事件超过单帧最大限制 (MAX_EVENTS_PER_FRAME)! 拒绝分配索引。");
                return uint.MaxValue;
            }

            uint index = AllocateIndex();
            m_resourceMap[key] = new ResourceMeta { index = index, refCount = 1 };
            m_indexToResourceMap[index] = key;

            data.index = index;
            SubmitEventUnsafe(m_cmd, isReadonly ? kEventRegisterSRV : kEventRegisterUAV, data);

            return index;
        }

        private uint AllocateIndex()
        {
            if (m_freeIndices.Count == 0)
                throw new Exception("Bindless Heap 耗尽!");
            uint localIndex = m_freeIndices.Dequeue();

            return localIndex + m_heapOffset;
        }

        private unsafe void SubmitEventUnsafe(CommandBuffer cmd, int eventId, BindlessEventData data)
        {
            int poolIndex = m_currentFrameIndex % MAX_FRAMES_IN_FLIGHT;
            int eventIndex = m_currentEventDataIndices[poolIndex];

            m_eventDataPools[poolIndex][eventIndex] = data;

            IntPtr ptr = (IntPtr)NativeArrayUnsafeUtility.GetUnsafeBufferPointerWithoutChecks(m_eventDataPools[poolIndex]);
            ptr = IntPtr.Add(ptr, eventIndex * kBindlessEventDataStride);

            cmd.IssuePluginEventAndData(GetRenderEventAndDataFunc(), eventId, ptr);

            m_currentEventDataIndices[poolIndex]++;
        }
        #endregion

        public void Dispose()
        {
            if (m_eventDataPools != null)
            {
                for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
                {
                    if (m_eventDataPools[i].IsCreated)
                        m_eventDataPools[i].Dispose();
                }
            }
            m_cmd?.Release();
            m_cmd = null;

            try
            {
                EmergencyClearBindlessHeap();
            }
            catch (Exception e)
            {
                Debug.LogError($"[Bindless] 紧急清理 Native 堆时发生异常: {e}");
            }

            m_resourceMap.Clear();
            m_indexToResourceMap.Clear();
            m_pendingZeroRef.Clear();
            m_pendingRelease.Clear();
            m_freeIndices.Clear();
        }
    }
}