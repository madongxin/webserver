using System.Buffers.Binary;

namespace GameMesh.WindowsTest.Net;

/// <summary>
/// 与 C++ gameproto::EncodeFrame / TryDecodeOneFrame 一致：
/// [uint32 BE length][protobuf payload]
/// </summary>
public static class ProtoFraming
{
    public const int MaxFrameSize = 4 * 1024 * 1024;

    public static byte[] EncodeFrame(ReadOnlySpan<byte> payload)
    {
        if (payload.Length > MaxFrameSize)
            throw new InvalidOperationException($"payload too large: {payload.Length}");
        var frame = new byte[4 + payload.Length];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(0, 4), (uint)payload.Length);
        payload.CopyTo(frame.AsSpan(4));
        return frame;
    }

    /// <summary>从流缓冲尝试切一帧；成功则消费缓冲头部。</summary>
    public static bool TryDecodeOneFrame(List<byte> buffer, out byte[] payload)
    {
        payload = Array.Empty<byte>();
        if (buffer.Count < 4)
            return false;
        Span<byte> header = stackalloc byte[4];
        header[0] = buffer[0];
        header[1] = buffer[1];
        header[2] = buffer[2];
        header[3] = buffer[3];
        var len = BinaryPrimitives.ReadUInt32BigEndian(header);
        if (len == 0 || len > MaxFrameSize)
            throw new InvalidOperationException($"invalid frame length: {len}");
        if (buffer.Count < 4 + (int)len)
            return false;
        payload = buffer.GetRange(4, (int)len).ToArray();
        buffer.RemoveRange(0, 4 + (int)len);
        return true;
    }
}
