using System;
using System.Runtime.InteropServices;

namespace UniBindlessSample
{
    [StructLayout(LayoutKind.Sequential)]
    public struct BindlessEventData
    {
        public IntPtr pResource;
        public uint index;
        public uint isBuffer;
        public uint bufferNumElements;
        public uint bufferStride;
        public uint isRawBuffer;
    }
}