using System;
using System.Threading;
using UnityEngine;

namespace UniBindlessSample
{
    public interface IBindlessResourceHandle
    {
        int InstanceID { get; }
        IntPtr GetNativePtr();
    }

    public class GfxBufferHandle : IBindlessResourceHandle, IDisposable
    {
        private static int s_GlobalIdCounter = 0;

        public int InstanceID { get; }
        public bool IsRawBuffer { get; }
        public int Count { get; }
        public int Stride { get; }

        private ComputeBuffer m_computeBuffer;
        private GraphicsBuffer m_graphicsBuffer;

        public GfxBufferHandle(int count, int stride, ComputeBufferType type)
        {
            InstanceID = Interlocked.Increment(ref s_GlobalIdCounter);
            Count = count;
            Stride = stride;
            IsRawBuffer = (type & ComputeBufferType.Raw) != 0;

            m_computeBuffer = new ComputeBuffer(count, stride, type);
        }

        public GfxBufferHandle(GraphicsBuffer.Target target, int count, int stride)
        {
            InstanceID = Interlocked.Increment(ref s_GlobalIdCounter);
            Count = count;
            Stride = stride;
            IsRawBuffer = (target & GraphicsBuffer.Target.Raw) != 0;

            m_graphicsBuffer = new GraphicsBuffer(target, count, stride);
        }

        public IntPtr GetNativePtr()
        {
            if (m_computeBuffer != null) return m_computeBuffer.GetNativeBufferPtr();
            if (m_graphicsBuffer != null) return m_graphicsBuffer.GetNativeBufferPtr();
            return IntPtr.Zero;
        }

        public ComputeBuffer GetComputeBuffer() => m_computeBuffer;
        public GraphicsBuffer GetGraphicsBuffer() => m_graphicsBuffer;

        public void Dispose()
        {
            m_computeBuffer?.Release();
            m_computeBuffer = null;

            m_graphicsBuffer?.Release();
            m_graphicsBuffer = null;
        }
    }
}