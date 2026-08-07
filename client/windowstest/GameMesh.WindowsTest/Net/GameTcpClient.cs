using System.Net.Sockets;
using Game;
using Google.Protobuf;

namespace GameMesh.WindowsTest.Net;

/// <summary>长连接 TCP 客户端：GameRequest / GameResponse + ProtoFraming。</summary>
public sealed class GameTcpClient : IDisposable
{
    private readonly object _gate = new();
    private TcpClient? _tcp;
    private NetworkStream? _stream;
    private readonly List<byte> _recvBuf = new();
    private ulong _seq;

    public bool IsConnected
    {
        get
        {
            lock (_gate)
                return _tcp is { Connected: true };
        }
    }

    public string Host { get; private set; } = "";
    public int Port { get; private set; }

    public void Connect(string host, int port, int timeoutMs = 5000)
    {
        Disconnect();
        var tcp = new TcpClient();
        var ar = tcp.BeginConnect(host, port, null, null);
        if (!ar.AsyncWaitHandle.WaitOne(timeoutMs) || !tcp.Connected)
        {
            try { tcp.Close(); } catch { /* ignore */ }
            throw new TimeoutException($"connect {host}:{port} timeout/fail");
        }
        tcp.EndConnect(ar);
        tcp.NoDelay = true;
        lock (_gate)
        {
            _tcp = tcp;
            _stream = tcp.GetStream();
            _recvBuf.Clear();
            _seq = 0;
            Host = host;
            Port = port;
        }
    }

    public void Disconnect()
    {
        lock (_gate)
        {
            try { _stream?.Close(); } catch { /* ignore */ }
            try { _tcp?.Close(); } catch { /* ignore */ }
            _stream = null;
            _tcp = null;
            _recvBuf.Clear();
        }
    }

    public void Dispose() => Disconnect();

    public GameResponse Exchange(GameRequest req, int timeoutMs = 10000)
    {
        NetworkStream stream;
        lock (_gate)
        {
            if (_stream == null || _tcp is not { Connected: true })
                throw new InvalidOperationException("not connected");
            stream = _stream;
            if (req.Seq == 0)
                req.Seq = ++_seq;
        }

        var payload = req.ToByteArray();
        var frame = ProtoFraming.EncodeFrame(payload);
        stream.Write(frame, 0, frame.Length);
        stream.Flush();

        var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (true)
        {
            lock (_gate)
            {
                if (ProtoFraming.TryDecodeOneFrame(_recvBuf, out var rspBytes))
                {
                    var rsp = GameResponse.Parser.ParseFrom(rspBytes);
                    return rsp;
                }
            }

            var remain = (int)(deadline - DateTime.UtcNow).TotalMilliseconds;
            if (remain <= 0)
                throw new TimeoutException("recv GameResponse timeout");

            if (!stream.DataAvailable)
            {
                Thread.Sleep(10);
                if (!stream.DataAvailable && DateTime.UtcNow >= deadline)
                    throw new TimeoutException("recv GameResponse timeout");
                if (!stream.DataAvailable)
                    continue;
            }

            var tmp = new byte[8192];
            var n = stream.Read(tmp, 0, tmp.Length);
            if (n <= 0)
                throw new IOException("connection closed by peer");
            lock (_gate)
                _recvBuf.AddRange(tmp.AsSpan(0, n).ToArray());
        }
    }

    public GameRequest NewRequest(string? sessionToken = null)
    {
        var req = new GameRequest();
        if (!string.IsNullOrEmpty(sessionToken))
            req.SessionToken = sessionToken;
        return req;
    }
}
