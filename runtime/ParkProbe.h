#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <ostream>
#include <unordered_map>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#ifndef SIOCOUTQNSD
#define SIOCOUTQNSD 0x894B
#endif

/** 停放连接上的字节种类。按写入时打标，用来看未发送队列里是谁占的。 */
enum class OutboundClass : int {
    Move = 1,
    Control = 2,
    Snapshot = 3,
    Reliable = 4,
    Prior = 5,
};

/**
 * 记录「内核未发送队列高于 16KB、位置帧停在应用缓冲」的连接。
 * 停放年龄和 TCP_INFO 只在这里累计。无关的 drain 不调用这里，因此不会把年龄清掉。
 * 这一帧被换掉时只清帧年龄；位置字节真正进内核才结束这一段停放。
 * 本机内核 3.10 的 tcp_info 没有 tcpi_snd_wnd / tcpi_notsent_bytes，对端窗口不在这里编造。
 */
class ParkProbe {
public:
    static void BeginFrame(uint64_t conn_id, int64_t now_ms) {
        if (conn_id == 0)
            return;
        std::lock_guard<std::mutex> lk(Mu());
        auto &c = Map()[conn_id];
        if (!c.parked) {
            c = Conn{};
            c.parked = true;
            c.episode_start_ms = now_ms;
            c.frame_start_ms = now_ms;
        } else if (c.frame_start_ms == 0) {
            c.frame_start_ms = now_ms;
        }
    }

    /** 停放帧换成更新坐标。连接仍停着，帧年龄清零，整段停放时间保留。 */
    static void EndFrame(uint64_t conn_id) {
        std::lock_guard<std::mutex> lk(Mu());
        auto it = Map().find(conn_id);
        if (it == Map().end())
            return;
        it->second.frame_start_ms = 0;
    }

    static void EndEpisode(uint64_t conn_id) {
        std::lock_guard<std::mutex> lk(Mu());
        Map().erase(conn_id);
    }

    static void AddApp(uint64_t conn_id, int cls, int n) {
        if (n <= 0)
            return;
        std::lock_guard<std::mutex> lk(Mu());
        auto it = Map().find(conn_id);
        if (it == Map().end() || !it->second.parked)
            return;
        Push(it->second.app, cls, n);
    }

    static void AddKernel(uint64_t conn_id, int cls, int n) {
        if (n <= 0)
            return;
        std::lock_guard<std::mutex> lk(Mu());
        auto it = Map().find(conn_id);
        if (it == Map().end() || !it->second.parked)
            return;
        Push(it->second.kernel, cls, n);
        it->second.kernel_sum += n;
        Trim(it->second, it->second.notsent > 0 ? it->second.notsent : it->second.kernel_sum);
    }

    /** 应用缓冲前面的字节已经 write 进内核。类别跟着字节走。 */
    static void AppToKernel(uint64_t conn_id, int n) {
        if (n <= 0)
            return;
        std::lock_guard<std::mutex> lk(Mu());
        auto it = Map().find(conn_id);
        if (it == Map().end() || !it->second.parked)
            return;
        int left = n;
        while (left > 0 && !it->second.app.empty()) {
            Seg &seg = it->second.app.front();
            const int take = seg.n < left ? seg.n : left;
            Push(it->second.kernel, seg.cls, take);
            it->second.kernel_sum += take;
            seg.n -= take;
            left -= take;
            if (seg.n <= 0)
                it->second.app.pop_front();
        }
    }

    static void Sample(uint64_t conn_id, int fd, int64_t now_ms) {
        std::lock_guard<std::mutex> lk(Mu());
        auto it = Map().find(conn_id);
        if (it == Map().end() || !it->second.parked)
            return;
        Conn &c = it->second;
        if (c.last_tcp_ms != 0 && now_ms - c.last_tcp_ms < 1000)
            return;
        c.last_tcp_ms = now_ms;
        int unsent = 0;
        if (fd >= 0 && ::ioctl(fd, SIOCOUTQNSD, &unsent) == 0 && unsent > 0)
            c.notsent = unsent;
        else
            c.notsent = 0;
        Trim(c, c.notsent);
        tcp_info info;
        std::memset(&info, 0, sizeof(info));
        socklen_t len = sizeof(info);
        c.tcp_ok = false;
        c.has_rcv_space = false;
        if (fd < 0 || ::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0)
            return;
        if (len < offsetof(tcp_info, tcpi_snd_cwnd) + sizeof(info.tcpi_snd_cwnd))
            return;
        c.tcp_ok = true;
        c.unacked = info.tcpi_unacked;
        c.retrans = info.tcpi_retrans;
        c.rto_us = info.tcpi_rto;
        c.cwnd = info.tcpi_snd_cwnd;
        c.rtt_us = info.tcpi_rtt;
        if (len >= offsetof(tcp_info, tcpi_rcv_space) + sizeof(info.tcpi_rcv_space)) {
            c.rcv_space = info.tcpi_rcv_space;
            c.has_rcv_space = true;
        }
    }

    static bool Parked(uint64_t conn_id) {
        std::lock_guard<std::mutex> lk(Mu());
        auto it = Map().find(conn_id);
        return it != Map().end() && it->second.parked;
    }

    static int64_t FrameAgeMs(uint64_t conn_id, int64_t now_ms) {
        std::lock_guard<std::mutex> lk(Mu());
        auto it = Map().find(conn_id);
        if (it == Map().end() || it->second.frame_start_ms <= 0 || now_ms < it->second.frame_start_ms)
            return 0;
        return now_ms - it->second.frame_start_ms;
    }

    static int64_t EpisodeAgeMs(uint64_t conn_id, int64_t now_ms) {
        std::lock_guard<std::mutex> lk(Mu());
        auto it = Map().find(conn_id);
        if (it == Map().end() || it->second.episode_start_ms <= 0 || now_ms < it->second.episode_start_ms)
            return 0;
        return now_ms - it->second.episode_start_ms;
    }

    /** 测试里表示一次无关 drain。生产路径不调用它，年龄保持不变。 */
    static void NoteUnrelatedDrain(uint64_t) {}

    static void WritePrometheus(std::ostream &os, int64_t now_ms) {
        std::vector<int64_t> frame_age;
        std::vector<int64_t> episode;
        std::vector<int64_t> unacked, retrans, rto, cwnd, rtt, rcv_space, notsent;
        int prior = 0, move_b = 0, control_b = 0, snap_b = 0, reliable_b = 0;
        int parked = 0;
        {
            std::lock_guard<std::mutex> lk(Mu());
            for (auto &kv : Map()) {
                Conn &c = kv.second;
                if (!c.parked)
                    continue;
                ++parked;
                if (c.frame_start_ms > 0 && now_ms >= c.frame_start_ms)
                    frame_age.push_back(now_ms - c.frame_start_ms);
                if (c.episode_start_ms > 0 && now_ms >= c.episode_start_ms)
                    episode.push_back(now_ms - c.episode_start_ms);
                if (c.tcp_ok) {
                    unacked.push_back(c.unacked);
                    retrans.push_back(c.retrans);
                    rto.push_back(c.rto_us);
                    cwnd.push_back(c.cwnd);
                    rtt.push_back(c.rtt_us);
                    notsent.push_back(c.notsent);
                    if (c.has_rcv_space)
                        rcv_space.push_back(c.rcv_space);
                } else if (c.notsent > 0) {
                    notsent.push_back(c.notsent);
                }
                int kernel_tracked = 0;
                auto add_cls = [&](int cls, int n) {
                    if (n <= 0)
                        return;
                    if (cls == static_cast<int>(OutboundClass::Move))
                        move_b += n;
                    else if (cls == static_cast<int>(OutboundClass::Control))
                        control_b += n;
                    else if (cls == static_cast<int>(OutboundClass::Snapshot))
                        snap_b += n;
                    else if (cls == static_cast<int>(OutboundClass::Reliable))
                        reliable_b += n;
                };
                for (const Seg &seg : c.app)
                    add_cls(seg.cls, seg.n);
                for (const Seg &seg : c.kernel) {
                    add_cls(seg.cls, seg.n);
                    kernel_tracked += seg.n;
                }
                if (c.notsent > kernel_tracked)
                    prior += c.notsent - kernel_tracked;
            }
        }
        if (parked <= 0)
            return;
        os << "# HELP gamemesh_aoi_parked_kernel_conns Connections whose position frame is parked because kernel unsent is above 16KB. Unit: connections.\n"
              "# TYPE gamemesh_aoi_parked_kernel_conns gauge\n"
              "gamemesh_aoi_parked_kernel_conns "
           << parked << "\n";
        EmitQuantiles(os, "gamemesh_latest_move_hold_ms",
                      "Age of the current parked position frame. Cleared when that frame enters the kernel or is swapped. An unrelated drain does not clear it. Unit: milliseconds.",
                      frame_age);
        EmitQuantiles(os, "gamemesh_aoi_parked_episode_ms",
                      "How long the connection has stayed parked with kernel unsent above 16KB. Cleared only when a position byte enters the kernel. Unit: milliseconds.",
                      episode);
        EmitQuantiles(os, "gamemesh_aoi_parked_tcp_unacked",
                      "tcpi_unacked on parked connections. Unit: segments.", unacked);
        EmitQuantiles(os, "gamemesh_aoi_parked_tcp_retrans",
                      "tcpi_retrans on parked connections. Unit: segments.", retrans);
        EmitQuantiles(os, "gamemesh_aoi_parked_tcp_rto_us",
                      "tcpi_rto on parked connections. Unit: microseconds.", rto);
        EmitQuantiles(os, "gamemesh_aoi_parked_tcp_snd_cwnd",
                      "tcpi_snd_cwnd on parked connections. Unit: segments.", cwnd);
        EmitQuantiles(os, "gamemesh_aoi_parked_tcp_rtt_us",
                      "tcpi_rtt on parked connections. Unit: microseconds.", rtt);
        EmitQuantiles(os, "gamemesh_aoi_parked_tcp_rcv_space_bytes",
                      "tcpi_rcv_space on parked connections. This is the local receive window estimate, not the peer advertised window. Unit: bytes.",
                      rcv_space);
        EmitQuantiles(os, "gamemesh_aoi_parked_tcp_notsent_bytes",
                      "Bytes not yet handed to the NIC (SIOCOUTQNSD) on parked connections. Unit: bytes.",
                      notsent);
        auto emit_bytes = [&](const char *cls, int n) {
            if (n <= 0)
                return;
            os << "gamemesh_aoi_parked_unsent_bytes{class=\"" << cls << "\"} " << n << "\n";
        };
        if (move_b > 0 || control_b > 0 || snap_b > 0 || reliable_b > 0 || prior > 0) {
            os << "# HELP gamemesh_aoi_parked_unsent_bytes Bytes still not on the wire for parked connections, tagged when accepted. prior means kernel unsent that was already queued before this park. Unit: bytes.\n"
                  "# TYPE gamemesh_aoi_parked_unsent_bytes gauge\n";
            emit_bytes("move", move_b);
            emit_bytes("control", control_b);
            emit_bytes("snapshot", snap_b);
            emit_bytes("reliable", reliable_b);
            emit_bytes("prior", prior);
        }
    }

private:
    struct Seg {
        int cls = 0;
        int n = 0;
    };
    struct Conn {
        bool parked = false;
        int64_t episode_start_ms = 0;
        int64_t frame_start_ms = 0;
        int64_t last_tcp_ms = 0;
        bool tcp_ok = false;
        bool has_rcv_space = false;
        uint32_t unacked = 0;
        uint32_t retrans = 0;
        uint32_t rto_us = 0;
        uint32_t cwnd = 0;
        uint32_t rtt_us = 0;
        uint32_t rcv_space = 0;
        int notsent = 0;
        int kernel_sum = 0;
        std::deque<Seg> app;
        std::deque<Seg> kernel;
    };

    static std::mutex &Mu() {
        static std::mutex mu;
        return mu;
    }
    static std::unordered_map<uint64_t, Conn> &Map() {
        static std::unordered_map<uint64_t, Conn> map;
        return map;
    }

    static void Push(std::deque<Seg> &q, int cls, int n) {
        if (!q.empty() && q.back().cls == cls)
            q.back().n += n;
        else
            q.push_back(Seg{cls, n});
    }

    static void Trim(Conn &c, int unsent) {
        if (unsent < 0)
            unsent = 0;
        while (c.kernel_sum > unsent && !c.kernel.empty()) {
            const int drop = c.kernel_sum - unsent;
            if (c.kernel.front().n <= drop) {
                c.kernel_sum -= c.kernel.front().n;
                c.kernel.pop_front();
            } else {
                c.kernel.front().n -= drop;
                c.kernel_sum -= drop;
            }
        }
    }

    static void EmitQuantiles(std::ostream &os, const char *name, const char *help,
                              std::vector<int64_t> samples) {
        if (samples.empty())
            return;
        std::sort(samples.begin(), samples.end());
        auto at = [&](double q) {
            size_t idx = static_cast<size_t>(q * static_cast<double>(samples.size() - 1));
            if (idx >= samples.size())
                idx = samples.size() - 1;
            return samples[idx];
        };
        os << "# HELP " << name << " " << help << "\n# TYPE " << name << " gauge\n";
        os << name << "{quantile=\"0.5\"} " << at(0.50) << "\n";
        os << name << "{quantile=\"0.9\"} " << at(0.90) << "\n";
        os << name << "{quantile=\"0.99\"} " << at(0.99) << "\n";
        os << name << "{quantile=\"1\"} " << samples.back() << "\n";
    }
};
