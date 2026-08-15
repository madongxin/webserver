#include "GatewayConnGuard.h"

#include "ProtocolHandshake.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string IpFromFd(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0)
        return "unknown";
    char buf[INET_ADDRSTRLEN] = {0};
    if (!::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)))
        return "unknown";
    return buf;
}

struct ConnState {
    bool hello_ok = false;
    int64_t last_rx_ms = 0;
    int64_t last_hb_ms = 0;
    int64_t frame_window_ms = 0;
    uint32_t frames_in_window = 0;
    uint32_t bytes_in_window = 0;
    int64_t auth_window_ms = 0;
    uint32_t auth_in_window = 0;
    int64_t chat_window_ms = 0;
    uint32_t chat_in_window = 0;
    int64_t name_window_ms = 0;
    uint32_t name_in_window = 0;
    std::string ip;
};

struct IpBucket {
    int64_t window_ms = 0;
    uint32_t connects = 0;
};

}  // namespace

GatewayConnGuard &GatewayConnGuard::Instance() {
    static GatewayConnGuard g;
    return g;
}

namespace {
std::mutex g_mu;
std::unordered_map<uint64_t, ConnState> g_conns;
std::unordered_map<std::string, IpBucket> g_ips;
}  // namespace

void GatewayConnGuard::OnConnected(uint64_t conn_id, int fd) {
    ConnState st;
    st.last_rx_ms = NowMs();
    st.frame_window_ms = st.last_rx_ms;
    st.auth_window_ms = st.last_rx_ms;
    st.ip = IpFromFd(fd);
    std::lock_guard<std::mutex> lk(g_mu);
    g_conns[conn_id] = std::move(st);
}

void GatewayConnGuard::OnDisconnected(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_conns.erase(conn_id);
}

void GatewayConnGuard::NoteActivity(uint64_t conn_id, size_t frame_bytes) {
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conns.find(conn_id);
    if (it == g_conns.end())
        return;
    it->second.last_rx_ms = now;
    (void)frame_bytes;
}

bool GatewayConnGuard::HelloOk(uint64_t conn_id) const {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conns.find(conn_id);
    return it != g_conns.end() && it->second.hello_ok;
}

void GatewayConnGuard::SetHelloOk(uint64_t conn_id, bool ok) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conns.find(conn_id);
    if (it == g_conns.end())
        return;
    it->second.hello_ok = ok;
}

std::string GatewayConnGuard::CheckConnectRate(int fd) {
    const std::string ip = IpFromFd(fd);
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lk(g_mu);
    auto &b = g_ips[ip];
    if (now - b.window_ms >= 2000) {
        b.window_ms = now;
        b.connects = 0;
    }
    ++b.connects;
    // 2s 窗口 80 次：允许容量测试 51 连，拦截明显洪泛
    if (b.connects > 80)
        return "ERR_RATE_LIMITED";
    if (g_ips.size() > 4096) {
        for (auto it = g_ips.begin(); it != g_ips.end();) {
            if (now - it->second.window_ms > 60000)
                it = g_ips.erase(it);
            else
                ++it;
        }
    }
    return {};
}

std::string GatewayConnGuard::CheckFrameRate(uint64_t conn_id, size_t frame_bytes) {
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conns.find(conn_id);
    if (it == g_conns.end())
        return {};
    auto &st = it->second;
    if (now - st.frame_window_ms >= 1000) {
        st.frame_window_ms = now;
        st.frames_in_window = 0;
        st.bytes_in_window = 0;
    }
    ++st.frames_in_window;
    st.bytes_in_window += static_cast<uint32_t>(frame_bytes > 0xffffffffULL ? 0xffffffffULL : frame_bytes);
    st.last_rx_ms = now;
    if (st.frames_in_window > 120)
        return "ERR_RATE_LIMITED";
    if (st.bytes_in_window > 512 * 1024)
        return "ERR_RATE_LIMITED";
    return {};
}

std::string GatewayConnGuard::CheckHeartbeatRate(uint64_t conn_id, bool bound) {
    const int64_t now = NowMs();
    const int64_t min_gap = static_cast<int64_t>(gameproto::HeartbeatMinIntervalMs());
    const int64_t gap = bound ? min_gap : (min_gap > 400 ? min_gap : 400);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conns.find(conn_id);
    if (it == g_conns.end())
        return "ERR_UNAUTHENTICATED";
    if (it->second.last_hb_ms != 0 && now - it->second.last_hb_ms < gap)
        return "ERR_RATE_LIMITED";
    it->second.last_hb_ms = now;
    it->second.last_rx_ms = now;
    return {};
}

std::string GatewayConnGuard::CheckAuthCommandRate(uint64_t conn_id) {
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conns.find(conn_id);
    if (it == g_conns.end())
        return {};
    auto &st = it->second;
    if (now - st.auth_window_ms >= 10000) {
        st.auth_window_ms = now;
        st.auth_in_window = 0;
    }
    ++st.auth_in_window;
    if (st.auth_in_window > 20)
        return "ERR_RATE_LIMITED";
    return {};
}

std::string GatewayConnGuard::CheckChatRate(uint64_t conn_id) {
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conns.find(conn_id);
    if (it == g_conns.end())
        return {};
    auto &st = it->second;
    if (now - st.chat_window_ms >= 2000) {
        st.chat_window_ms = now;
        st.chat_in_window = 0;
    }
    ++st.chat_in_window;
    if (st.chat_in_window > 5)
        return "ERR_RATE_LIMITED";
    return {};
}

std::string GatewayConnGuard::CheckNameQueryRate(uint64_t conn_id) {
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conns.find(conn_id);
    if (it == g_conns.end())
        return {};
    auto &st = it->second;
    if (now - st.name_window_ms >= 10000) {
        st.name_window_ms = now;
        st.name_in_window = 0;
    }
    ++st.name_in_window;
    if (st.name_in_window > 8)
        return "ERR_RATE_LIMITED";
    return {};
}

bool GatewayConnGuard::IdleExpired(uint64_t conn_id, uint32_t idle_ms) const {
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conns.find(conn_id);
    if (it == g_conns.end())
        return true;
    return now - it->second.last_rx_ms >= static_cast<int64_t>(idle_ms);
}

std::string GatewayConnGuard::PeerIp(uint64_t conn_id) const {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conns.find(conn_id);
    if (it == g_conns.end())
        return {};
    return it->second.ip;
}
