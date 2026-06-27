//测试纹理: Earth from space：https://sipi.usc.edu/database/database.php?volume=aerials&image=11#top
using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using UniBindlessSample;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.InputSystem;

public class SplitBindless : MonoBehaviour
{
    #region ----Res----
    public Texture2D SrcTexture;
    public ComputeShader cs;
    #endregion

    #region ----RTs----
    private const int kSplitSliceX = 8;
    private const int kSplitSliceY = 8;
    private const int kSplitSliceCount = kSplitSliceX * kSplitSliceY;

    private Texture2D[] m_splitTexSlices;
    private Dictionary<Texture2D, uint> m_splitTexMap;
    private uint[] m_registeredHeaps;

    private RenderTexture m_randomSliceRT;

    private int m_sliceWidth;
    private int m_sliceHeight;

    private int m_texWidth;
    private int m_texHeight;

    private void SetupTextures()
    {
        if (SrcTexture == null)
        {
#if UNITY_EDITOR
            Debug.LogError("SrcTexture不应为空!");
            UnityEditor.EditorApplication.ExitPlaymode();
#endif
            return;
        }
        //
        int width = SrcTexture.width;
        int height = SrcTexture.height;
        m_texWidth = width;
        m_texHeight = height;
        var texFormat = SrcTexture.graphicsFormat;
        int sw = width / kSplitSliceX;
        int sh = height / kSplitSliceY;
        m_sliceWidth = sw;
        m_sliceHeight = sh;
        if (sw <= 0 || sh <= 0)
        {
#if UNITY_EDITOR
            Debug.LogError("SrcTexture分割后尺寸太小!");
            UnityEditor.EditorApplication.ExitPlaymode();
#endif
            return;
        }
        //
        m_splitTexSlices = new Texture2D[kSplitSliceCount];
        m_splitTexMap = new Dictionary<Texture2D, uint>(kSplitSliceCount);
        m_registeredHeaps = new uint[kSplitSliceCount];
        for (int i = 0; i < kSplitSliceX; i++)
        {
            for (int j = 0; j < kSplitSliceY; j++)
            {
                int index = i + j * kSplitSliceX;
                m_splitTexSlices[index] = new Texture2D(sw, sh, texFormat, TextureCreationFlags.None);
                int startX = i * sw;
                int startY = j * sh;
                Graphics.CopyTexture(SrcTexture, 0, 0, startX, startY, sw, sh, m_splitTexSlices[index], 0, 0, 0, 0);
            }
        }
        //
        for (int i = 0; i < kSplitSliceX; i++)
        {
            for (int j = 0; j < kSplitSliceY; j++)
            {
                int index = i + j * kSplitSliceX;
                var t = m_splitTexSlices[index];
                uint hindex = m_bindlessResManager.RegisterTexture(t);
                m_splitTexMap[t] = hindex;
                m_registeredHeaps[index] = hindex;
            }
        }
        //
        m_randomSliceRT = new RenderTexture(width, height, 0, GraphicsFormat.R8G8B8A8_UNorm);
        m_randomSliceRT.enableRandomWrite = true;
        m_randomSliceRT.Create();
        //
        m_bindlessResManager.UpdateFrame();
    }

    private void ReleaseTextures()
    {
        if (m_splitTexSlices != null)
        {
            for (int i = 0; i < kSplitSliceX; i++)
            {
                for (int j = 0; j < kSplitSliceY; j++)
                {
                    int index = i + j * kSplitSliceX;
                    var t = m_splitTexSlices[index];
                    if (t != null)
                    {
                        var hindex = m_splitTexMap[t];
                        m_bindlessResManager.Unregister(hindex);//其实这里的意义不是很大, 因为实际插件内部维护的已经来不及靠这个清理了(cmd有延迟)
                        //
                        Destroy(t);
#pragma warning disable IDE0059 // 不需要赋值
                        t = null;
#pragma warning restore IDE0059 // 不需要赋值
                    }
                }
            }
        }
        m_splitTexMap.Clear();
        m_splitTexMap = null;
        m_splitTexSlices = null;
        //
        if (m_randomSliceRT != null)
        {
            Destroy(m_randomSliceRT);
            m_randomSliceRT = null;
        }
    }
    #endregion

    #region ----ResHeapBuffer----
    private GraphicsBuffer m_resHeapBuffer;
    private void SetupBuffer()
    {
        m_resHeapBuffer = new GraphicsBuffer(GraphicsBuffer.Target.Structured, kSplitSliceCount, sizeof(uint));
        m_resHeapBuffer.SetData(m_registeredHeaps);
    }

    private void ReleaseBuffer()
    {
        m_resHeapBuffer?.Release();
        m_resHeapBuffer = null;
    }

    private static void ShuffleBuffer(uint[] array)
    {
        System.Random rand = new System.Random();
        for (int i = array.Length - 1; i > 0; i--)
        {
            int j = rand.Next(i + 1);
            (array[i], array[j]) = (array[j], array[i]);
        }
    }

    private void SetRandomData()
    {
        ShuffleBuffer(m_registeredHeaps);
        m_resHeapBuffer.SetData(m_registeredHeaps);
    }
    #endregion

    #region ----Rendering----
    private static readonly int k_ShaderProperty_SliceCountX = Shader.PropertyToID("_SliceCountX");
    private static readonly int k_ShaderProperty_SliceWidth = Shader.PropertyToID("_SliceWidth");
    private static readonly int k_ShaderProperty_SliceHeight = Shader.PropertyToID("_SliceHeight");
    private static readonly int k_ShaderProperty_RegisteredHeapsMapBuffer = Shader.PropertyToID("_RegisteredHeapsMapBuffer");
    private static readonly int k_ShaderProperty_rw_outputTex = Shader.PropertyToID("rw_outputTex");

    private void OnDrawRandomSplitBlocks(RenderTexture dest)
    {
        if (cs == null) return;
        CommandBuffer cmd = new CommandBuffer()
        {
            name = "Bindless Draw Sample Pass"
        };
#if DEVELOPMENT_BUILD
        if (m_bindlessResManager.RequiresDependencyTracking)
        {
            for (int i = 0; i < m_splitTexSlices.Length; i++)
            {
                var tex = m_splitTexSlices[i];
                if (tex != null)
                {
                    m_bindlessResManager.ForceDependencyTrackingUnsafe(cmd, tex.GetNativeTexturePtr());
                }
            }
        }
#endif
        //
        cmd.SetGlobalBuffer("_RegisteredHeapsMapBuffer", m_resHeapBuffer);
        //
        {
            int kernelIndex = cs.FindKernel("BindlessSampleKernel");
            cmd.SetComputeIntParam(cs, k_ShaderProperty_SliceCountX, kSplitSliceX);
            cmd.SetComputeIntParam(cs, k_ShaderProperty_SliceWidth, m_sliceWidth);
            cmd.SetComputeIntParam(cs, k_ShaderProperty_SliceHeight, m_sliceHeight);
            cmd.SetComputeBufferParam(cs, kernelIndex, k_ShaderProperty_RegisteredHeapsMapBuffer, m_resHeapBuffer);
            cmd.SetComputeTextureParam(cs, kernelIndex, k_ShaderProperty_rw_outputTex, m_randomSliceRT);
            cmd.DispatchCompute(cs, kernelIndex, Mathf.CeilToInt(m_texWidth / 8.0f), Mathf.CeilToInt(m_texHeight / 8.0f), 1);
        }
        //
        Graphics.ExecuteCommandBuffer(cmd);
        cmd.Release();
        //
        Graphics.Blit(m_randomSliceRT, dest);
    }
    #endregion

    #region ----BM----
    private BindlessResourceManager m_bindlessResManager;
    #endregion

    #region ----Unity----
    private void Awake()
    {
        m_bindlessResManager = new BindlessResourceManager(kSplitSliceCount * 2);
    }

    private void OnEnable()
    {
        SetupTextures();
        SetupBuffer();
    }

    private void Update()
    {
        if (Keyboard.current != null && Keyboard.current.rKey.wasPressedThisFrame)
        {
            SetRandomData();
        }
    }

    private void OnRenderImage(RenderTexture source, RenderTexture destination)
    {
        OnDrawRandomSplitBlocks(destination);
    }

    private void OnDisable()
    {
        ReleaseTextures();
        ReleaseBuffer();
    }

    private void OnDestroy()
    {
        m_bindlessResManager.Dispose();
    }
#endregion
}
