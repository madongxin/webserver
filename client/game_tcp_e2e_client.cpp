/**
 * 双 Gateway / 最终 E2E TCP 客户端。
 *
 * 用法摘要:
 *   game_tcp_e2e_client register-login <host> <port> [device] [password]
 *   game_tcp_e2e_client enter-map <host> <port> <player> <token> <map_tpl> [map_inst]
 *   game_tcp_e2e_client map-ping <host> <port> <player> <token> <map_inst>
 *   game_tcp_e2e_client reconnect <host> <port> <player> <session_id> <ticket> [last_seq]
 *   game_tcp_e2e_client dual-gw <gw0_host> <gw0_port> <gw1_host> <gw1_port> [map_tpl] [map_inst]
 *   game_tcp_e2e_client hold-kill-reconnect <gw0_host> <gw0_port> <gw1_host> <gw1_port> [map_tpl] [map_inst]
 *   game_tcp_e2e_client drain-login <host> <port>   # 期望失败（摘流后）
 *   game_tcp_e2e_client register-login-profile <host> <port> [device] [password]
 *   game_tcp_e2e_client enter-public-map <host> <port> [device] [password] [map_tpl]
 *   game_tcp_e2e_client move <host> <port> <player> <password> <map_inst> <x> <y> <z> [yaw]
 *   game_tcp_e2e_client send-player-mail <host> <port> <player> <password> <receiver> <title> <body> [op]
 *   game_tcp_e2e_client mail-list <host> <port> <player> <password>
 *   game_tcp_e2e_client two-player-aoi <gw0_host> <gw0_port> <gw1_host> <gw1_port> [map_tpl]
 *   game_tcp_e2e_client map-capacity-51 <host> <port> [map_tpl] [n]
 *   game_tcp_e2e_client unity-contract-check <host> <port> [device] [password]
 *   game_tcp_e2e_client login-profile <host> <port> <player> <password> [device]
 *
 * 成功输出 key=value 行，便于脚本解析；失败非零。
 */
#include "ProtoFraming.h"
#include "game.pb.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

bool SendAll(int fd, const char *data, size_t n) {
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(fd, data + off, n - off);
        if (w <= 0)
            return false;
        off += static_cast<size_t>(w);
    }
    return true;
}

bool RecvAll(int fd, char *data, size_t n) {
    size_t off = 0;
    while (off < n) {
        const ssize_t r = ::read(fd, data + off, n - off);
        if (r <= 0)
            return false;
        off += static_cast<size_t>(r);
    }
    return true;
}

struct SessionState {
    uint64_t player_id = 0;
    std::string token;
    std::string session_id;
    uint64_t generation = 0;
    uint64_t last_server_seq = 0;
    uint64_t acked_seq = 0;
    uint64_t next_seq = 1;
    uint64_t map_instance_id = 0;
    uint64_t map_template_id = 0;
    std::string logic_id;
    uint64_t owner_epoch = 0;
    float spawn_x = 0, spawn_y = 0, spawn_z = 0, spawn_yaw = 0;
    std::string player_name;
    uint64_t stats_version = 0;
    std::vector<uint64_t> aoi_snapshot_ids;
};

struct TcpSession {
    int fd = -1;
    SessionState st;
    std::vector<game::GameResponse> inbox;
};

int Connect(const char *host, int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

#ifndef GAMEMESH_SCHEMA_SHA256
#define GAMEMESH_SCHEMA_SHA256 ""
#endif

bool Exchange(int fd, const game::GameRequest &req, game::GameResponse *rsp, int timeout_ms = 8000);

bool DoClientHello(int fd, const char *schema_override = nullptr, uint32_t proto_ver = 1,
                   game::GameResponse *out = nullptr) {
    game::GameRequest req;
    req.set_seq(0);
    auto *h = req.mutable_client_hello();
    h->set_protocol_version(proto_ver);
    h->set_schema_sha256(schema_override ? schema_override : GAMEMESH_SCHEMA_SHA256);
    h->set_client_version("e2e-1.0.0");
    h->set_platform("cpp-e2e");
    h->set_build_channel("test");
    game::GameResponse rsp;
    if (!Exchange(fd, req, &rsp, 8000))
        return false;
    if (out)
        *out = rsp;
    return rsp.ok() && rsp.has_server_hello() && rsp.server_hello().ok() &&
           rsp.error_code() == "OK";
}

int ConnectAndHello(const char *host, int port) {
    const int raw_fd = Connect(host, port);
    if (raw_fd < 0)
        return -1;
    if (!DoClientHello(raw_fd)) {
        ::close(raw_fd);
        return -1;
    }
    return raw_fd;
}

enum class RecvResult { kOk, kTimeout, kClosed };

RecvResult RecvFrameEx(int fd, game::GameResponse *rsp, int timeout_ms) {
    if (timeout_ms > 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int sel = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel == 0)
            return RecvResult::kTimeout;
        if (sel < 0)
            return RecvResult::kClosed;
    }
    uint32_t be_len = 0;
    if (!RecvAll(fd, reinterpret_cast<char *>(&be_len), 4))
        return RecvResult::kClosed;
    const uint32_t len = ntohl(be_len);
    if (len == 0 || len > 4 * 1024 * 1024)
        return RecvResult::kClosed;
    std::string body(len, '\0');
    if (!RecvAll(fd, &body[0], len))
        return RecvResult::kClosed;
    if (!rsp->ParseFromString(body))
        return RecvResult::kClosed;
    return RecvResult::kOk;
}

bool RecvFrame(int fd, game::GameResponse *rsp, int timeout_ms) {
    return RecvFrameEx(fd, rsp, timeout_ms) == RecvResult::kOk;
}

int RemainingMs(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
        return 0;
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

void NotePush(SessionState *st, const game::GameResponse &push,
              std::vector<game::GameResponse> *inbox) {
    if (inbox)
        inbox->push_back(push);
    if (!push.has_server_push())
        return;
    const uint64_t seq = push.server_push().server_seq();
    if (st && seq > st->last_server_seq)
        st->last_server_seq = seq;
}

bool IsUnsolicitedNotify(const game::GameResponse &cur) {
    return cur.has_server_push() || cur.has_chat_notify() || cur.has_mailbox_changed() ||
           cur.has_aoi_delta() || cur.has_session_replaced();
}

bool Exchange(int fd, const game::GameRequest &req, game::GameResponse *rsp, int timeout_ms,
              SessionState *st, std::vector<game::GameResponse> *inbox) {
    std::string body;
    if (!req.SerializeToString(&body))
        return false;
    std::string frame;
    if (!gameproto::EncodeFrame(body, &frame))
        return false;
    if (!SendAll(fd, frame.data(), frame.size()))
        return false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
    while (true) {
        const int left = RemainingMs(deadline);
        if (left <= 0)
            return false;
        game::GameResponse cur;
        if (!RecvFrame(fd, &cur, left))
            return false;
        if (cur.has_server_push()) {
            NotePush(st, cur, inbox);
            continue;
        }
        // 不可靠推送外层 seq=0，不能当成 RPC 应答（否则 MailList/ChatSend 会被 notify 偷走）。
        if (IsUnsolicitedNotify(cur) || (req.seq() != 0 && cur.seq() != req.seq())) {
            if (inbox)
                inbox->push_back(cur);
            else
                NotePush(st, cur, nullptr);
            continue;
        }
        *rsp = std::move(cur);
        return true;
    }
}

bool Exchange(int fd, const game::GameRequest &req, game::GameResponse *rsp, int timeout_ms) {
    return Exchange(fd, req, rsp, timeout_ms, nullptr, nullptr);
}

void PrintKv(const char *k, const std::string &v) {
    std::printf("%s=%s\n", k, v.c_str());
    std::fflush(stdout);
}
void PrintKv(const char *k, uint64_t v) {
    std::printf("%s=%llu\n", k, static_cast<unsigned long long>(v));
    std::fflush(stdout);
}
void PrintKv(const char *k, int v) {
    std::printf("%s=%d\n", k, v);
    std::fflush(stdout);
}
void PrintKv(const char *k, bool v) {
    std::printf("%s=%d\n", k, v ? 1 : 0);
    std::fflush(stdout);
}

std::string TrimCopy(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string NormalizeHex(std::string s) {
    s = TrimCopy(s);
    const auto sp = s.find_first_of(" \t");
    if (sp != std::string::npos)
        s = s.substr(0, sp);
    for (char &c : s) {
        if (c >= 'A' && c <= 'F')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string LoadMapSha256() {
    if (const char *e = std::getenv("GAMEMESH_MAP_SHA256")) {
        const std::string v = NormalizeHex(e);
        if (!v.empty())
            return v;
    }
    const char *path = std::getenv("GAMEMESH_MAP_SHA256_FILE");
    std::string p = path && *path ? path : "config/maps/map_1001.json.sha256";
    std::ifstream in(p.c_str());
    if (!in)
        return "";
    std::string line;
    std::getline(in, line);
    return NormalizeHex(line);
}

uint64_t LoadMapDataVersion() {
    if (const char *e = std::getenv("GAMEMESH_MAP_DATA_VERSION"))
        return std::strtoull(e, nullptr, 10);
    return 1;
}

void PrintProfile(const char *prefix, const game::PlayerAttributes &p) {
    const std::string pre = prefix ? prefix : "profile";
    PrintKv((pre + "_player_id").c_str(), p.player_id());
    PrintKv((pre + "_name").c_str(), p.player_name());
    PrintKv((pre + "_hp").c_str(), static_cast<uint64_t>(p.hp()));
    PrintKv((pre + "_max_hp").c_str(), static_cast<uint64_t>(p.max_hp()));
    PrintKv((pre + "_mp").c_str(), static_cast<uint64_t>(p.mp()));
    PrintKv((pre + "_max_mp").c_str(), static_cast<uint64_t>(p.max_mp()));
    PrintKv((pre + "_attack").c_str(), static_cast<uint64_t>(p.attack()));
    PrintKv((pre + "_spell_power").c_str(), static_cast<uint64_t>(p.spell_power()));
    PrintKv((pre + "_defense").c_str(), static_cast<uint64_t>(p.defense()));
    PrintKv((pre + "_magic_resistance").c_str(), static_cast<uint64_t>(p.magic_resistance()));
    PrintKv((pre + "_move_speed").c_str(), std::to_string(p.move_speed()));
    PrintKv((pre + "_attack_speed").c_str(), std::to_string(p.attack_speed()));
    PrintKv((pre + "_stats_version").c_str(), p.stats_version());
}

bool ProfileComplete(const game::PlayerAttributes &p) {
    return p.player_id() != 0 && !p.player_name().empty() && p.max_hp() > 0 && p.max_mp() > 0 &&
           p.move_speed() > 0.f && p.stats_version() != 0;
}

bool InnerFromPush(const game::GameResponse &outer, game::GameResponse *inner) {
    if (!inner || !outer.has_server_push())
        return false;
    return inner->ParseFromString(outer.server_push().payload());
}

bool Rpc(TcpSession *c, game::GameRequest *req, game::GameResponse *rsp, int timeout_ms = 8000) {
    if (!c || c->fd < 0 || !req || !rsp)
        return false;
    if (req->seq() == 0 && !req->has_push_ack())
        req->set_seq(c->st.next_seq++);
    if (req->session_token().empty() && !c->st.token.empty())
        req->set_session_token(c->st.token);
    return Exchange(c->fd, *req, rsp, timeout_ms, &c->st, &c->inbox);
}

bool MaybeAck(TcpSession *c) {
    if (!c || c->fd < 0)
        return true;
    if (c->st.last_server_seq == 0 || c->st.last_server_seq <= c->st.acked_seq)
        return true;
    game::GameRequest req;
    auto *a = req.mutable_push_ack();
    a->set_player_id(c->st.player_id);
    a->set_ack_server_seq(c->st.last_server_seq);
    a->set_session_id(c->st.session_id);
    a->set_fence_token(c->st.token);
    a->set_generation(c->st.generation);
    game::GameResponse rsp;
    if (!Rpc(c, &req, &rsp, 3000) || !rsp.has_push_ack())
        return true;
    if (rsp.push_ack().ok())
        c->st.acked_seq = c->st.last_server_seq;
    PrintKv("push_ack_ok", rsp.push_ack().ok());
    PrintKv("push_ack_seq", c->st.acked_seq);
    return true;
}

void DrainPushes(TcpSession *c, int timeout_ms) {
    if (!c || c->fd < 0 || timeout_ms <= 0)
        return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        const int left = RemainingMs(deadline);
        if (left <= 0)
            break;
        game::GameResponse cur;
        if (!RecvFrame(c->fd, &cur, left > 250 ? 250 : left))
            break;
        if (cur.has_server_push())
            NotePush(&c->st, cur, &c->inbox);
        else
            c->inbox.push_back(cur);
    }
    MaybeAck(c);
}

bool FindAoiEvent(const std::vector<game::GameResponse> &inbox, int op, uint64_t player_id,
                  game::EntitySnapshot *out, int *seen_n, size_t from = 0) {
    int n = 0;
    bool found = false;
    game::EntitySnapshot last;
    if (from > inbox.size())
        from = inbox.size();
    for (size_t idx = from; idx < inbox.size(); ++idx) {
        const auto &outer = inbox[idx];
        game::GameResponse inner;
        const game::AoiDelta *delta = nullptr;
        if (outer.has_aoi_delta()) {
            delta = &outer.aoi_delta();
        } else if (InnerFromPush(outer, &inner) && inner.has_aoi_delta()) {
            delta = &inner.aoi_delta();
        } else {
            continue;
        }
        for (int i = 0; i < delta->events_size(); ++i) {
            const auto &ev = delta->events(i);
            if (ev.op() != op)
                continue;
            if (player_id != 0 && ev.entity().player_id() != player_id)
                continue;
            ++n;
            last = ev.entity();
            found = true;
        }
    }
    if (seen_n)
        *seen_n = n;
    if (found && out)
        *out = last;
    return found;
}

bool WaitAoi(TcpSession *c, int op, uint64_t player_id, int timeout_ms, game::EntitySnapshot *out,
             int *seen_n, size_t from = 0) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        if (FindAoiEvent(c->inbox, op, player_id, out, seen_n, from))
            return true;
        const int left = RemainingMs(deadline);
        if (left <= 0) {
            PrintKv("aoi_wait_timeout_op", op);
            PrintKv("aoi_wait_want_player", player_id);
            PrintKv("aoi_inbox_n", static_cast<uint64_t>(c->inbox.size()));
            for (const auto &outer : c->inbox) {
                if (outer.has_server_push()) {
                    PrintKv("inbox_push_type", outer.server_push().message_type());
                    game::GameResponse inner;
                    if (InnerFromPush(outer, &inner)) {
                        PrintKv("inbox_inner_has_aoi", inner.has_aoi_delta());
                        if (inner.has_aoi_delta()) {
                            PrintKv("inbox_aoi_events",
                                    static_cast<uint64_t>(inner.aoi_delta().events_size()));
                            for (int i = 0; i < inner.aoi_delta().events_size(); ++i) {
                                PrintKv("inbox_aoi_op", inner.aoi_delta().events(i).op());
                                PrintKv("inbox_aoi_pid",
                                        inner.aoi_delta().events(i).entity().player_id());
                            }
                        }
                    } else {
                        PrintKv("inbox_inner_parse", 0);
                    }
                } else if (outer.has_aoi_delta()) {
                    PrintKv("inbox_outer_aoi", 1);
                }
            }
            return FindAoiEvent(c->inbox, op, player_id, out, seen_n, from);
        }
        DrainPushes(c, left > 300 ? 300 : left);
    }
}

bool FindMailboxChanged(const std::vector<game::GameResponse> &inbox, uint64_t player_id) {
    for (const auto &outer : inbox) {
        game::GameResponse inner;
        if (outer.has_mailbox_changed() &&
            (player_id == 0 || outer.mailbox_changed().player_id() == player_id))
            return true;
        if (InnerFromPush(outer, &inner) && inner.has_mailbox_changed() &&
            (player_id == 0 || inner.mailbox_changed().player_id() == player_id))
            return true;
        if (outer.has_server_push() && outer.server_push().message_type() == "mailbox.changed.v1")
            return true;
    }
    return false;
}

bool DoRegisterLogin(int fd, const std::string &device, const std::string &password,
                     SessionState *st, const std::string &display_name = "e2e") {
    game::GameRequest reg;
    reg.set_seq(1);
    auto *r = reg.mutable_register_();
    r->set_device_id(device);
    r->set_display_name(display_name.empty() ? "e2e" : display_name);
    r->set_password(password);
    game::GameResponse rr;
    if (!Exchange(fd, reg, &rr)) {
        std::printf("error=register msg=no_response\n");
        std::fflush(stdout);
        return false;
    }
    if (!rr.ok() || !rr.has_register_() || !rr.register_().ok()) {
        std::printf("error=register msg=%s\n",
                    rr.message().empty() && rr.has_register_() ? rr.register_().message().c_str()
                                                               : rr.message().c_str());
        std::fflush(stdout);
        return false;
    }
    st->player_id = rr.register_().player_id();
    PrintKv("player_id", st->player_id);

    game::GameRequest login;
    login.set_seq(2);
    auto *l = login.mutable_login();
    l->set_player_id(st->player_id);
    l->set_device_id(device);
    l->set_server_id(1);
    l->set_credential(password);
    game::GameResponse lr;
    if (!Exchange(fd, login, &lr) || !lr.ok() || !lr.has_login() || !lr.login().ok()) {
        std::printf("error=login msg=%s\n", lr.message().c_str());
        std::fflush(stdout);
        return false;
    }
    st->token = lr.login().token();
    st->session_id = lr.login().session_id();
    st->generation = lr.login().generation();
    st->next_seq = 3;
    PrintKv("token", st->token);
    PrintKv("session_id", st->session_id);
    PrintKv("generation", st->generation);
    if (lr.login().has_profile()) {
        st->player_name = lr.login().profile().player_name();
        st->stats_version = lr.login().profile().stats_version();
        PrintProfile("login_profile", lr.login().profile());
        PrintKv("login_profile_complete", ProfileComplete(lr.login().profile()));
    }
    PrintKv("login_ok", true);
    return true;
}

bool DoEnterMap(int fd, SessionState *st, uint64_t map_tpl, uint64_t map_inst,
                std::vector<game::GameResponse> *inbox = nullptr) {
    game::GameRequest req;
    req.set_seq(st->next_seq++);
    req.set_session_token(st->token);
    auto *e = req.mutable_enter_map();
    e->set_player_id(st->player_id);
    e->set_realm_id(1);
    e->set_map_template_id(map_tpl);
    e->set_map_instance_id(map_inst);
    const std::string hash = LoadMapSha256();
    if (!hash.empty() && map_tpl == 1001) {
        e->set_map_data_version(LoadMapDataVersion());
        e->set_map_data_sha256(hash);
    }
    e->set_operation_id(std::string("enter:") + std::to_string(st->player_id) + ":" +
                        std::to_string(map_tpl));
    game::GameResponse rsp;
    if (!Exchange(fd, req, &rsp, 15000, st, inbox) || !rsp.ok() || !rsp.has_enter_map() ||
        !rsp.enter_map().ok()) {
        std::printf("error=enter_map msg=%s\n", rsp.message().c_str());
        if (rsp.has_enter_map()) {
            PrintKv("enter_map_error", rsp.enter_map().message());
            if (!rsp.enter_map().map_data_sha256().empty())
                PrintKv("server_map_sha256", rsp.enter_map().map_data_sha256());
            PrintKv("server_map_data_version", rsp.enter_map().map_data_version());
        }
        std::fflush(stdout);
        return false;
    }
    st->map_instance_id = rsp.enter_map().map_instance_id();
    st->map_template_id = rsp.enter_map().map_template_id();
    st->logic_id = rsp.enter_map().gamelogic_instance_id();
    st->owner_epoch = rsp.enter_map().owner_epoch();
    if (rsp.enter_map().has_spawn_position()) {
        st->spawn_x = rsp.enter_map().spawn_position().x();
        st->spawn_y = rsp.enter_map().spawn_position().y();
        st->spawn_z = rsp.enter_map().spawn_position().z();
        st->spawn_yaw = rsp.enter_map().spawn_yaw();
        PrintKv("spawn_x", std::to_string(st->spawn_x));
        PrintKv("spawn_y", std::to_string(st->spawn_y));
        PrintKv("spawn_z", std::to_string(st->spawn_z));
        PrintKv("spawn_yaw", std::to_string(st->spawn_yaw));
    }
    if (rsp.enter_map().has_self())
        PrintKv("self_player_id", rsp.enter_map().self().player_id());
    st->aoi_snapshot_ids.clear();
    for (int i = 0; i < rsp.enter_map().aoi_snapshot_size(); ++i) {
        const uint64_t pid = rsp.enter_map().aoi_snapshot(i).player_id();
        st->aoi_snapshot_ids.push_back(pid);
        PrintKv("aoi_snapshot_player", pid);
    }
    PrintKv("aoi_snapshot_n", static_cast<uint64_t>(rsp.enter_map().aoi_snapshot_size()));
    PrintKv("map_instance_id", st->map_instance_id);
    PrintKv("gamelogic_instance_id", st->logic_id);
    PrintKv("owner_epoch", st->owner_epoch);
    PrintKv("route_version", rsp.enter_map().route_version());
    PrintKv("map_data_sha256", rsp.enter_map().map_data_sha256());
    PrintKv("enter_map_ok", true);

    // 可能紧随可靠 Push（enter_map_notify）；短超时避免压测把空等算进失败
    for (int i = 0; i < 3; ++i) {
        game::GameResponse push;
        if (!RecvFrame(fd, &push, 200))
            break;
        if (push.has_server_push()) {
            NotePush(st, push, inbox);
            PrintKv("push_recv", 1);
            PrintKv("server_seq", push.server_push().server_seq());
            PrintKv("push_msg", push.server_push().message_type());
        } else if (push.message() == "enter_map_notify" || push.has_enter_map()) {
            if (st->last_server_seq == 0)
                st->last_server_seq = 1;
            if (inbox)
                inbox->push_back(push);
            PrintKv("push_recv", 1);
            PrintKv("push_msg", push.message());
        } else if (inbox) {
            inbox->push_back(push);
        }
    }
    return true;
}

bool DoEnterMap(TcpSession *c, uint64_t map_tpl, uint64_t map_inst) {
    return DoEnterMap(c->fd, &c->st, map_tpl, map_inst, &c->inbox);
}

bool SnapshotHas(const SessionState &st, uint64_t player_id) {
    for (uint64_t id : st.aoi_snapshot_ids) {
        if (id == player_id)
            return true;
    }
    return false;
}

bool DoReconnect(int fd, SessionState *st, uint64_t last_seq, int *replay_n, bool *need_snap) {
    game::GameRequest req;
    req.set_seq(20);
    auto *r = req.mutable_reconnect();
    r->set_player_id(st->player_id);
    r->set_session_id(st->session_id);
    r->set_reconnect_ticket(st->token);
    r->set_last_server_seq(last_seq);
    game::GameResponse rsp;
    std::vector<game::GameResponse> pending;
    if (!Exchange(fd, req, &rsp, 15000, st, &pending) || !rsp.ok() || !rsp.has_reconnect() ||
        !rsp.reconnect().ok()) {
        std::printf("error=reconnect msg=%s\n", rsp.message().c_str());
        std::fflush(stdout);
        return false;
    }
    st->token = rsp.reconnect().token();
    st->session_id = rsp.reconnect().session_id();
    st->generation = rsp.reconnect().generation();
    if (need_snap)
        *need_snap = rsp.reconnect().need_full_snapshot();
    PrintKv("reconnect_ok", true);
    PrintKv("need_full_snapshot", rsp.reconnect().need_full_snapshot());
    PrintKv("replay_from_seq", rsp.reconnect().replay_from_seq());
    PrintKv("token", st->token);
    PrintKv("session_id", st->session_id);
    PrintKv("generation", st->generation);

    int n = 0;
    uint64_t max_seq = last_seq;
    auto consume_push = [&](const game::GameResponse &push) -> int {
        if (!push.has_server_push()) {
            PrintKv("replay_non_envelope", 1);
            PrintKv("replay_msg", push.message());
            return 0;
        }
        const uint64_t seq = push.server_push().server_seq();
        if (seq == 0) {
            std::printf("error=replay_missing_server_seq\n");
            std::fflush(stdout);
            return -1;
        }
        if (seq <= last_seq) {
            std::printf("error=replay_seq_not_after_last seq=%llu last=%llu\n",
                        static_cast<unsigned long long>(seq),
                        static_cast<unsigned long long>(last_seq));
            std::fflush(stdout);
            return -1;
        }
        if (max_seq != last_seq && seq != max_seq + 1 && !rsp.reconnect().need_full_snapshot()) {
            if (push.server_push().message_type() != "full_snapshot") {
                std::printf("error=replay_seq_gap got=%llu expect=%llu\n",
                            static_cast<unsigned long long>(seq),
                            static_cast<unsigned long long>(max_seq + 1));
                std::fflush(stdout);
                return -1;
            }
        }
        if (seq > max_seq)
            max_seq = seq;
        ++n;
        PrintKv("replay_frame", static_cast<uint64_t>(n));
        PrintKv("server_seq", seq);
        PrintKv("replay_msg", push.server_push().message_type());
        if (push.server_push().message_type() == "full_snapshot") {
            game::GameResponse inner;
            if (inner.ParseFromString(push.server_push().payload()) && inner.has_full_snapshot()) {
                const auto &fs = inner.full_snapshot();
                PrintKv("replay_snapshot_ok", fs.ok());
                PrintKv("replay_snapshot_aoi_n",
                        static_cast<uint64_t>(fs.aoi_entities_size()));
                PrintKv("replay_snapshot_map", fs.map_instance_id());
                PrintKv("replay_snapshot_recovery", fs.recovery_reason());
                PrintKv("replay_snapshot_baseline", fs.baseline_server_seq());
                if (fs.has_self())
                    PrintKv("replay_snapshot_self", fs.self().player_id());
            }
        }
        return 1;
    };
    for (const auto &p : pending) {
        const int cr = consume_push(p);
        if (cr < 0)
            return false;
    }
    for (int i = 0; i < 8; ++i) {
        game::GameResponse push;
        if (!RecvFrame(fd, &push, 200))
            break;
        const int cr = consume_push(push);
        if (cr < 0)
            return false;
    }
    if (max_seq > st->last_server_seq)
        st->last_server_seq = max_seq;
    if (replay_n)
        *replay_n = n;
    PrintKv("replay_n", n);
    return true;
}

bool DoGetSelfProfile(TcpSession *c, game::PlayerAttributes *out) {
    game::GameRequest req;
    req.mutable_get_self_profile()->set_player_id(c->st.player_id);
    game::GameResponse rsp;
    if (!Rpc(c, &req, &rsp, 8000) || !rsp.ok() || !rsp.has_get_self_profile() ||
        !rsp.get_self_profile().ok() || !rsp.get_self_profile().has_profile()) {
        std::printf("error=get_self_profile msg=%s\n", rsp.message().c_str());
        std::fflush(stdout);
        return false;
    }
    const auto &p = rsp.get_self_profile().profile();
    c->st.player_name = p.player_name();
    c->st.stats_version = p.stats_version();
    PrintProfile("profile", p);
    PrintKv("profile_complete", ProfileComplete(p));
    if (out)
        *out = p;
    return ProfileComplete(p);
}

bool DoWorldSnapshot(TcpSession *c, game::FullStateSnapshotRsp *out) {
    game::GameRequest req;
    auto *w = req.mutable_world_snapshot();
    w->set_player_id(c->st.player_id);
    w->set_last_applied_server_seq(c->st.last_server_seq);
    game::GameResponse rsp;
    if (!Rpc(c, &req, &rsp, 8000) || !rsp.ok() || !rsp.has_full_snapshot() ||
        !rsp.full_snapshot().ok()) {
        std::printf("error=world_snapshot msg=%s code=%s\n", rsp.message().c_str(),
                    rsp.error_code().c_str());
        std::fflush(stdout);
        return false;
    }
    const auto &fs = rsp.full_snapshot();
    PrintKv("snapshot_ok", 1);
    PrintKv("snapshot_error_code", rsp.error_code());
    PrintKv("snapshot_baseline", fs.baseline_server_seq());
    PrintKv("snapshot_map_instance_id", fs.map_instance_id());
    PrintKv("snapshot_template_id", fs.map_template_id());
    PrintKv("snapshot_recovery", fs.recovery_reason());
    PrintKv("snapshot_life_state", fs.life_state());
    PrintKv("snapshot_aoi_n", static_cast<uint64_t>(fs.aoi_entities_size()));
    if (fs.has_self()) {
        PrintKv("snapshot_self_id", fs.self().player_id());
        PrintKv("snapshot_self_x", std::to_string(fs.self().position().x()));
        PrintKv("snapshot_self_z", std::to_string(fs.self().position().z()));
    }
    if (fs.has_profile())
        PrintKv("snapshot_profile_id", fs.profile().player_id());
    if (out)
        *out = fs;
    if (fs.baseline_server_seq() > c->st.last_server_seq)
        c->st.last_server_seq = fs.baseline_server_seq();
    return fs.has_self() && fs.self().player_id() == c->st.player_id;
}

bool DoMoveTo(TcpSession *c, float x, float y, float z, float yaw, game::MoveRsp *out) {
    game::GameRequest req;
    auto *m = req.mutable_move();
    m->set_player_id(c->st.player_id);
    m->set_map_instance_id(c->st.map_instance_id);
    m->mutable_position()->set_x(x);
    m->mutable_position()->set_y(y);
    m->mutable_position()->set_z(z);
    m->set_yaw(yaw);
    m->set_client_time_ms(static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count()));
    game::GameResponse rsp;
    if (!Rpc(c, &req, &rsp, 8000) || !rsp.ok() || !rsp.has_move() || !rsp.move().ok()) {
        std::printf("error=move msg=%s code=%s\n", rsp.message().c_str(),
                    rsp.has_move() ? rsp.move().error_code().c_str() : "");
        std::fflush(stdout);
        return false;
    }
    PrintKv("move_ok", true);
    PrintKv("move_x", std::to_string(rsp.move().position().x()));
    PrintKv("move_y", std::to_string(rsp.move().position().y()));
    PrintKv("move_z", std::to_string(rsp.move().position().z()));
    PrintKv("move_state_seq", rsp.move().state_seq());
    if (out)
        *out = rsp.move();
    return true;
}

bool DoLogout(TcpSession *c) {
    game::GameRequest req;
    auto *l = req.mutable_logout();
    l->set_player_id(c->st.player_id);
    l->set_token(c->st.token);
    game::GameResponse rsp;
    if (!Rpc(c, &req, &rsp, 8000) || !rsp.ok() || (rsp.has_logout() && !rsp.logout().ok())) {
        std::printf("error=logout msg=%s\n", rsp.message().c_str());
        std::fflush(stdout);
        return false;
    }
    PrintKv("logout_ok", true);
    return true;
}

bool DoPlayerMailSend(TcpSession *c, uint64_t receiver, const std::string &title,
                      const std::string &body, const std::string &op, uint64_t *mail_id) {
    game::GameRequest req;
    auto *m = req.mutable_player_mail_send();
    m->set_sender_player_id(c->st.player_id);
    m->set_receiver_player_id(receiver);
    m->set_title(title);
    m->set_body(body);
    m->set_operation_id(op);
    game::GameResponse rsp;
    if (!Rpc(c, &req, &rsp, 15000) || !rsp.ok() || !rsp.has_player_mail_send() ||
        !rsp.player_mail_send().ok()) {
        std::printf("error=player_mail_send msg=%s code=%s\n", rsp.message().c_str(),
                    rsp.has_player_mail_send() ? rsp.player_mail_send().error_code().c_str() : "");
        std::fflush(stdout);
        return false;
    }
    PrintKv("mail_send_ok", true);
    PrintKv("mail_id", rsp.player_mail_send().mail_id());
    if (mail_id)
        *mail_id = rsp.player_mail_send().mail_id();
    return true;
}

bool DoMailList(TcpSession *c, uint64_t *first_id, int *count) {
    game::GameRequest req;
    auto *m = req.mutable_mail_list();
    m->set_player_id(c->st.player_id);
    m->set_limit(20);
    game::GameResponse rsp;
    const bool rpc_ok = Rpc(c, &req, &rsp, 8000);
    if (!rpc_ok || !rsp.ok() || !rsp.has_mail_list() || !rsp.mail_list().ok()) {
        std::printf("error=mail_list rpc=%d ok=%d has=%d msg=%s code=%s body=%d seq=%llu\n",
                    rpc_ok ? 1 : 0, rsp.ok() ? 1 : 0, rsp.has_mail_list() ? 1 : 0,
                    rsp.message().c_str(), rsp.error_code().c_str(),
                    static_cast<int>(rsp.body_case()),
                    static_cast<unsigned long long>(rsp.seq()));
        std::fflush(stdout);
        return false;
    }
    PrintKv("mail_list_ok", true);
    PrintKv("mail_list_n", static_cast<uint64_t>(rsp.mail_list().mails_size()));
    if (count)
        *count = rsp.mail_list().mails_size();
    if (first_id && rsp.mail_list().mails_size() > 0)
        *first_id = rsp.mail_list().mails(0).mail_id();
    return true;
}

bool DoMailGet(TcpSession *c, uint64_t mail_id, std::string *body_out) {
    game::GameRequest req;
    auto *m = req.mutable_mail_get();
    m->set_player_id(c->st.player_id);
    m->set_mail_id(mail_id);
    game::GameResponse rsp;
    if (!Rpc(c, &req, &rsp, 8000) || !rsp.ok() || !rsp.has_mail_get() || !rsp.mail_get().ok()) {
        std::printf("error=mail_get msg=%s\n", rsp.message().c_str());
        std::fflush(stdout);
        return false;
    }
    PrintKv("mail_get_ok", true);
    PrintKv("mail_get_body", rsp.mail_get().mail().body());
    if (body_out)
        *body_out = rsp.mail_get().mail().body();
    return true;
}

bool DoLoginExisting(int fd, uint64_t player_id, const std::string &device,
                     const std::string &password, SessionState *st) {
    game::GameRequest login;
    login.set_seq(st->next_seq++);
    auto *l = login.mutable_login();
    l->set_player_id(player_id);
    l->set_device_id(device);
    l->set_server_id(1);
    l->set_credential(password);
    l->set_kick_other_device(true);
    game::GameResponse lr;
    if (!Exchange(fd, login, &lr, 8000, st, nullptr) || !lr.ok() || !lr.has_login() ||
        !lr.login().ok()) {
        std::printf("error=login msg=%s\n", lr.message().c_str());
        std::fflush(stdout);
        return false;
    }
    st->player_id = player_id;
    st->token = lr.login().token();
    st->session_id = lr.login().session_id();
    st->generation = lr.login().generation();
    PrintKv("player_id", st->player_id);
    PrintKv("token", st->token);
    PrintKv("session_id", st->session_id);
    PrintKv("generation", st->generation);
    PrintKv("kicked_previous", lr.login().kicked_previous());
    if (lr.login().has_profile()) {
        st->player_name = lr.login().profile().player_name();
        st->stats_version = lr.login().profile().stats_version();
        PrintProfile("login_profile", lr.login().profile());
        PrintKv("login_profile_complete", ProfileComplete(lr.login().profile()));
    }
    PrintKv("login_ok", true);
    return true;
}

bool OpenRegister(TcpSession *c, const char *host, int port, const std::string &device,
                  const std::string &password, const std::string &display_name = "e2e") {
    c->fd = ConnectAndHello(host, port);
    if (c->fd < 0) {
        std::perror("connect");
        return false;
    }
    return DoRegisterLogin(c->fd, device, password, &c->st, display_name);
}

int CmdRegisterLogin(int argc, char **argv) {
    if (argc < 4)
        return 2;
    const char *host = argv[2];
    const int port = std::atoi(argv[3]);
    const std::string device = argc >= 5 ? argv[4] : ("e2e-" + std::to_string(::getpid()));
    const std::string password = argc >= 6 ? argv[5] : "e2epass1";
    const int fd = ConnectAndHello(host, port);
    if (fd < 0) {
        std::perror("connect");
        return 6;
    }
    SessionState st;
    const bool ok = DoRegisterLogin(fd, device, password, &st);
    ::close(fd);
    return ok ? 0 : 12;
}

int CmdEnterMap(int argc, char **argv) {
    if (argc < 7)
        return 2;
    const char *host = argv[2];
    const int port = std::atoi(argv[3]);
    SessionState st;
    st.player_id = std::strtoull(argv[4], nullptr, 10);
    st.token = argv[5];
    const uint64_t tpl = std::strtoull(argv[6], nullptr, 10);
    const uint64_t inst = argc >= 8 ? std::strtoull(argv[7], nullptr, 10) : 0;
    const int fd = ConnectAndHello(host, port);
    if (fd < 0)
        return 6;
    // 需先 login 才能 enter — 本命令假定已在同一连接登录；独立连接需 login
    // 简化：重新 login
    game::GameRequest login;
    login.set_seq(2);
    auto *l = login.mutable_login();
    l->set_player_id(st.player_id);
    l->set_device_id("e2e");
    l->set_server_id(1);
    l->set_credential(argc >= 9 ? argv[8] : "e2epass1");
    game::GameResponse lr;
    if (!Exchange(fd, login, &lr) || !lr.ok() || !lr.has_login()) {
        std::printf("error=relogin\n");
        std::fflush(stdout);
        ::close(fd);
        return 12;
    }
    st.token = lr.login().token();
    st.session_id = lr.login().session_id();
    const bool ok = DoEnterMap(fd, &st, tpl, inst);
    ::close(fd);
    return ok ? 0 : 12;
}

int CmdReconnect(int argc, char **argv) {
    if (argc < 7)
        return 2;
    const char *host = argv[2];
    const int port = std::atoi(argv[3]);
    SessionState st;
    st.player_id = std::strtoull(argv[4], nullptr, 10);
    st.session_id = argv[5];
    st.token = argv[6];
    const uint64_t last = argc >= 8 ? std::strtoull(argv[7], nullptr, 10) : 0;
    const int fd = ConnectAndHello(host, port);
    if (fd < 0)
        return 6;
    int rn = 0;
    bool snap = false;
    const bool ok = DoReconnect(fd, &st, last, &rn, &snap);
    // 故障演练：保持连接一段时间，便于脚本断言 Redis ONLINE
    if (ok) {
        if (const char *hold = std::getenv("E2E_HOLD_MS")) {
            const int ms = std::atoi(hold);
            if (ms > 0)
                usleep(static_cast<useconds_t>(ms) * 1000);
        }
    }
    ::close(fd);
    return ok ? 0 : 12;
}

int CmdDualGw(int argc, char **argv) {
    if (argc < 6)
        return 2;
    const char *h0 = argv[2];
    const int p0 = std::atoi(argv[3]);
    const char *h1 = argv[4];
    const int p1 = std::atoi(argv[5]);
    const uint64_t map_tpl =
        argc >= 7 ? std::strtoull(argv[6], nullptr, 10)
                  : (900000ULL + static_cast<uint64_t>(::getpid() % 100000));
    const uint64_t map_inst = argc >= 8 ? std::strtoull(argv[7], nullptr, 10) : 0;
    const std::string device = "e2e-dual-" + std::to_string(::getpid());
    const std::string password = "e2epass1";

    const int fd0 = ConnectAndHello(h0, p0);
    if (fd0 < 0) {
        std::perror("connect gw0");
        return 6;
    }
    SessionState st;
    if (!DoRegisterLogin(fd0, device, password, &st)) {
        ::close(fd0);
        return 12;
    }
    if (!DoEnterMap(fd0, &st, map_tpl, map_inst)) {
        ::close(fd0);
        return 13;
    }
    // 故意不 PushAck，关闭连接触发 MarkDisconnected
    ::close(fd0);
    PrintKv("gw0_closed", 1);
    ::usleep(80000);  // 80ms：覆盖 MarkDisconnected/Unbind 落库，避免压测空等

    const int fd1 = ConnectAndHello(h1, p1);
    if (fd1 < 0) {
        std::perror("connect gw1");
        return 7;
    }
    int replay_n = 0;
    bool need_snap = false;
    // last_seq=0：期望回放进图 Push（若已写入 ReplayStore）
    if (!DoReconnect(fd1, &st, 0, &replay_n, &need_snap)) {
        ::close(fd1);
        return 14;
    }
    ::close(fd1);

    // 门禁：reconnect 成功；必须有可靠回放（真实 server_seq）或明确 need_full_snapshot
    if (replay_n == 0 && !need_snap) {
        std::printf("error=no_replay_and_no_snapshot\n");
        std::fflush(stdout);
        return 16;
    }
    PrintKv("dual_gw_ok", 1);
    PrintKv("map_tpl", map_tpl);
    return 0;
}

int CmdDrainLogin(int argc, char **argv) {
    if (argc < 4)
        return 2;
    const int fd = ConnectAndHello(argv[2], std::atoi(argv[3]));
    if (fd < 0)
        return 6;
    game::GameRequest login;
    login.set_seq(1);
    auto *l = login.mutable_login();
    l->set_player_id(1);
    l->set_device_id("drain");
    l->set_server_id(1);
    l->set_credential("e2epass1");
    game::GameResponse rsp;
    const bool got = Exchange(fd, login, &rsp, 3000);
    ::close(fd);
    if (!got) {
        PrintKv("drain_reject", 1);
        PrintKv("reason", "no_response_or_closed");
        return 0;  // 连接被关也算摘流成功
    }
    if (!rsp.ok() || rsp.message().find("drain") != std::string::npos) {
        PrintKv("drain_reject", 1);
        PrintKv("msg", rsp.message());
        return 0;
    }
    std::printf("error=login_succeeded_while_draining\n");
    std::fflush(stdout);
    return 15;
}

int CmdHoldKillReconnect(int argc, char **argv) {
    if (argc < 6)
        return 2;
    const char *h0 = argv[2];
    const int p0 = std::atoi(argv[3]);
    const char *h1 = argv[4];
    const int p1 = std::atoi(argv[5]);
    const uint64_t map_tpl =
        argc >= 7 ? std::strtoull(argv[6], nullptr, 10)
                  : (910000ULL + static_cast<uint64_t>(::getpid() % 100000));
    const uint64_t map_inst = argc >= 8 ? std::strtoull(argv[7], nullptr, 10) : 0;
    const std::string device = "e2e-hold-" + std::to_string(::getpid());
    const std::string password = "e2epass1";

    const int fd0 = ConnectAndHello(h0, p0);
    if (fd0 < 0) {
        std::perror("connect gw0");
        return 6;
    }
    SessionState st;
    if (!DoRegisterLogin(fd0, device, password, &st)) {
        ::close(fd0);
        return 12;
    }
    if (!DoEnterMap(fd0, &st, map_tpl, map_inst)) {
        ::close(fd0);
        return 13;
    }
    const std::string old_token = st.token;
    const uint64_t old_gen = st.generation;
    PrintKv("READY_TO_KILL", 1);
    PrintKv("reconnect_ticket", st.token);
    PrintKv("last_server_seq", st.last_server_seq);
    PrintKv("old_generation", old_gen);
    std::fflush(stdout);

    // 保持 TCP 打开，等待对端 EOF（脚本 SIGKILL gw0），禁止主动 close
    int wait_ms = 30000;
    if (const char *e = std::getenv("E2E_HOLD_KILL_WAIT_MS")) {
        const int v = std::atoi(e);
        if (v > 0)
            wait_ms = v;
    }
    game::GameResponse ignored;
    const bool eof = !RecvFrame(fd0, &ignored, wait_ms);
    (void)eof;
    ::close(fd0);
    PrintKv("gw0_eof", 1);

    const int fd1 = ConnectAndHello(h1, p1);
    if (fd1 < 0) {
        std::perror("connect gw1");
        return 7;
    }
    int replay_n = 0;
    bool need_snap = false;
    if (!DoReconnect(fd1, &st, st.last_server_seq, &replay_n, &need_snap)) {
        ::close(fd1);
        return 14;
    }
    PrintKv("old_token", old_token);
    PrintKv("old_fence_still_equal", old_token == st.token);
    if (st.generation <= old_gen) {
        std::printf("error=generation_not_increased old=%llu new=%llu\n",
                    static_cast<unsigned long long>(old_gen),
                    static_cast<unsigned long long>(st.generation));
        std::fflush(stdout);
        ::close(fd1);
        return 17;
    }
    if (replay_n == 0 && !need_snap) {
        std::printf("error=no_replay_and_no_snapshot\n");
        std::fflush(stdout);
        ::close(fd1);
        return 16;
    }
    PrintKv("hold_kill_reconnect_ok", 1);
    ::close(fd1);
    return 0;
}

int CmdRegisterLoginProfile(int argc, char **argv) {
    if (argc < 4)
        return 2;
    TcpSession c;
    const std::string device = argc >= 5 ? argv[4] : ("e2e-p-" + std::to_string(::getpid()));
    const std::string password = argc >= 6 ? argv[5] : "e2epass1";
    if (!OpenRegister(&c, argv[2], std::atoi(argv[3]), device, password)) {
        if (c.fd >= 0)
            ::close(c.fd);
        return 12;
    }
    game::PlayerAttributes attrs;
    const bool ok = DoGetSelfProfile(&c, &attrs);
    if (ok && attrs.player_name() == "player") {
        std::printf("error=profile_default_name\n");
        std::fflush(stdout);
        ::close(c.fd);
        return 18;
    }
    ::close(c.fd);
    return ok ? 0 : 12;
}

int CmdLoginProfile(int argc, char **argv) {
    if (argc < 6)
        return 2;
    const char *host = argv[2];
    const int port = std::atoi(argv[3]);
    const uint64_t player = std::strtoull(argv[4], nullptr, 10);
    const std::string password = argv[5];
    const std::string device = argc >= 7 ? argv[6] : ("e2e-lp-" + std::to_string(::getpid()));
    TcpSession c;
    c.fd = ConnectAndHello(host, port);
    if (c.fd < 0)
        return 6;
    if (!DoLoginExisting(c.fd, player, device, password, &c.st)) {
        ::close(c.fd);
        return 12;
    }
    game::PlayerAttributes attrs;
    const bool ok = DoGetSelfProfile(&c, &attrs);
    ::close(c.fd);
    return ok ? 0 : 12;
}

int CmdEnterPublicMap(int argc, char **argv) {
    if (argc < 4)
        return 2;
    TcpSession c;
    const std::string device = argc >= 5 ? argv[4] : ("e2e-m-" + std::to_string(::getpid()));
    const std::string password = argc >= 6 ? argv[5] : "e2epass1";
    const uint64_t tpl = argc >= 7 ? std::strtoull(argv[6], nullptr, 10) : 1001;
    if (!OpenRegister(&c, argv[2], std::atoi(argv[3]), device, password)) {
        if (c.fd >= 0)
            ::close(c.fd);
        return 12;
    }
    const bool ok = DoEnterMap(&c, tpl, 0);
    DrainPushes(&c, 400);
    ::close(c.fd);
    return ok ? 0 : 13;
}

int CmdMove(int argc, char **argv) {
    if (argc < 10)
        return 2;
    TcpSession c;
    c.fd = ConnectAndHello(argv[2], std::atoi(argv[3]));
    if (c.fd < 0)
        return 6;
    const uint64_t player = std::strtoull(argv[4], nullptr, 10);
    const std::string password = argv[5];
    c.st.map_instance_id = std::strtoull(argv[6], nullptr, 10);
    const float x = std::strtof(argv[7], nullptr);
    const float y = std::strtof(argv[8], nullptr);
    const float z = std::strtof(argv[9], nullptr);
    const float yaw = argc >= 11 ? std::strtof(argv[10], nullptr) : 0.f;
    if (!DoLoginExisting(c.fd, player, "e2e-move", password, &c.st)) {
        ::close(c.fd);
        return 12;
    }
    const bool ok = DoMoveTo(&c, x, y, z, yaw, nullptr);
    ::close(c.fd);
    return ok ? 0 : 13;
}

int CmdSendPlayerMail(int argc, char **argv) {
    if (argc < 9)
        return 2;
    TcpSession c;
    c.fd = ConnectAndHello(argv[2], std::atoi(argv[3]));
    if (c.fd < 0)
        return 6;
    const uint64_t player = std::strtoull(argv[4], nullptr, 10);
    const std::string password = argv[5];
    const uint64_t receiver = std::strtoull(argv[6], nullptr, 10);
    const std::string title = argv[7];
    const std::string body = argv[8];
    const std::string op = argc >= 10 ? argv[9]
                                      : ("mail:" + std::to_string(player) + ":" +
                                         std::to_string(::getpid()));
    if (!DoLoginExisting(c.fd, player, "e2e-mail", password, &c.st)) {
        ::close(c.fd);
        return 12;
    }
    uint64_t mid = 0;
    const bool ok = DoPlayerMailSend(&c, receiver, title, body, op, &mid);
    ::close(c.fd);
    return ok ? 0 : 13;
}

int CmdMailList(int argc, char **argv) {
    if (argc < 6)
        return 2;
    TcpSession c;
    c.fd = ConnectAndHello(argv[2], std::atoi(argv[3]));
    if (c.fd < 0)
        return 6;
    if (!DoLoginExisting(c.fd, std::strtoull(argv[4], nullptr, 10), "e2e-ml", argv[5], &c.st)) {
        ::close(c.fd);
        return 12;
    }
    uint64_t first = 0;
    int n = 0;
    const bool ok = DoMailList(&c, &first, &n);
    if (ok && first != 0)
        PrintKv("mail_list_first_id", first);
    ::close(c.fd);
    return ok ? 0 : 13;
}

int CmdUnityContractCheck(int argc, char **argv) {
    if (argc < 4)
        return 2;
    const char *host = argv[2];
    const int port = std::atoi(argv[3]);
    const std::string device = argc >= 5 ? argv[4] : ("e2e-uc-" + std::to_string(::getpid()));
    const std::string password = argc >= 6 ? argv[5] : "e2epass1";
    TcpSession c;
    if (!OpenRegister(&c, host, port, device, password)) {
        if (c.fd >= 0)
            ::close(c.fd);
        return 12;
    }
    game::PlayerAttributes attrs;
    if (!DoGetSelfProfile(&c, &attrs) || attrs.player_name() == "player") {
        std::printf("error=profile_incomplete_or_default\n");
        std::fflush(stdout);
        ::close(c.fd);
        return 18;
    }
    PrintKv("csharp_namespace", std::string("GameMesh.Protocol"));
    PrintKv("enter_map_req_sha256_field", 6);
    PrintKv("aoi_delta_field", 62);
    PrintKv("mailbox_changed_field", 64);

    game::GameRequest bad;
    auto *e = bad.mutable_enter_map();
    e->set_player_id(c.st.player_id);
    e->set_realm_id(1);
    e->set_map_template_id(1001);
    e->set_map_instance_id(0);
    e->set_map_data_version(1);
    e->set_map_data_sha256("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    e->set_operation_id("mismatch:" + std::to_string(c.st.player_id));
    game::GameResponse br;
    if (!Rpc(&c, &bad, &br, 15000)) {
        std::printf("error=mismatch_no_response\n");
        std::fflush(stdout);
        ::close(c.fd);
        return 19;
    }
    const bool mismatch = !br.ok() && br.message().find("ERR_MAP_DATA_MISMATCH") != std::string::npos;
    PrintKv("map_hash_mismatch_rejected", mismatch);
    if (br.has_enter_map() && !br.enter_map().map_data_sha256().empty())
        PrintKv("server_map_sha256", br.enter_map().map_data_sha256());
    if (!mismatch) {
        std::printf("error=expected_map_data_mismatch got=%s\n", br.message().c_str());
        std::fflush(stdout);
        ::close(c.fd);
        return 19;
    }
    if (!DoEnterMap(&c, 1001, 0)) {
        ::close(c.fd);
        return 13;
    }
    PrintKv("unity_contract_ok", 1);
    ::close(c.fd);
    return 0;
}

int CmdTwoPlayerAoi(int argc, char **argv) {
    if (argc < 6)
        return 2;
    const char *h0 = argv[2];
    const int p0 = std::atoi(argv[3]);
    const char *h1 = argv[4];
    const int p1 = std::atoi(argv[5]);
    const uint64_t tpl = argc >= 7 ? std::strtoull(argv[6], nullptr, 10) : 1001ULL;
    const std::string pass = "e2epass1";
    TcpSession a, b;
    if (!OpenRegister(&a, h0, p0, "e2e-aoi-a-" + std::to_string(::getpid()), pass)) {
        if (a.fd >= 0)
            ::close(a.fd);
        return 12;
    }
    if (!OpenRegister(&b, h1, p1, "e2e-aoi-b-" + std::to_string(::getpid()), pass)) {
        if (b.fd >= 0)
            ::close(b.fd);
        ::close(a.fd);
        return 12;
    }
    if (a.st.player_id == b.st.player_id) {
        std::printf("error=same_player_id\n");
        std::fflush(stdout);
        ::close(a.fd);
        ::close(b.fd);
        return 20;
    }
    game::PlayerAttributes pa, pb;
    if (!DoGetSelfProfile(&a, &pa) || !DoGetSelfProfile(&b, &pb)) {
        ::close(a.fd);
        ::close(b.fd);
        return 18;
    }
    if (!DoEnterMap(&a, tpl, 0)) {
        ::close(a.fd);
        ::close(b.fd);
        return 13;
    }
    DrainPushes(&a, 300);
    if (!DoEnterMap(&b, tpl, 0)) {
        ::close(a.fd);
        ::close(b.fd);
        return 13;
    }
    PrintKv("a_map_instance_id", a.st.map_instance_id);
    PrintKv("b_map_instance_id", b.st.map_instance_id);
    PrintKv("a_owner", a.st.logic_id);
    PrintKv("b_owner", b.st.logic_id);
    if (a.st.map_instance_id == 0 || a.st.map_instance_id != b.st.map_instance_id ||
        a.st.logic_id != b.st.logic_id) {
        std::printf("error=not_same_instance_or_owner\n");
        std::fflush(stdout);
        ::close(a.fd);
        ::close(b.fd);
        return 21;
    }
    PrintKv("same_instance", 1);
    PrintKv("same_owner", 1);
    PrintKv("a_inbox_after_enter", static_cast<uint64_t>(a.inbox.size()));
    PrintKv("a_last_seq_after_enter", a.st.last_server_seq);

    DrainPushes(&a, 2000);
    PrintKv("a_inbox_after_b_enter", static_cast<uint64_t>(a.inbox.size()));
    PrintKv("a_last_seq_after_b_enter", a.st.last_server_seq);

    game::EntitySnapshot snap;
    int seen = 0;
    const bool b_saw_a =
        SnapshotHas(b.st, a.st.player_id) || WaitAoi(&b, 1, a.st.player_id, 8000, &snap, &seen);
    PrintKv("b_aoi_enter_a", b_saw_a);
    const bool a_saw_b =
        SnapshotHas(a.st, b.st.player_id) || WaitAoi(&a, 1, b.st.player_id, 8000, &snap, &seen);
    PrintKv("a_aoi_enter_b", a_saw_b);
    if (!b_saw_a || !a_saw_b) {
        std::printf("error=missing_mutual_aoi_enter\n");
        std::fflush(stdout);
        ::close(a.fd);
        ::close(b.fd);
        return 22;
    }

    const float nx = a.st.spawn_x + 1.0f;
    game::MoveRsp mv;
    if (!DoMoveTo(&a, nx, a.st.spawn_y, a.st.spawn_z, a.st.spawn_yaw, &mv)) {
        ::close(a.fd);
        ::close(b.fd);
        return 23;
    }
    game::EntitySnapshot moved;
    if (!WaitAoi(&b, 2, a.st.player_id, 8000, &moved, &seen)) {
        std::printf("error=missing_aoi_move\n");
        std::fflush(stdout);
        ::close(a.fd);
        ::close(b.fd);
        return 24;
    }
    PrintKv("b_aoi_move_player", moved.player_id());
    PrintKv("b_aoi_move_state_seq", moved.state_seq());
    PrintKv("b_aoi_move_x", std::to_string(moved.position().x()));
    if (moved.player_id() != a.st.player_id || moved.state_seq() == 0) {
        std::printf("error=aoi_move_fields\n");
        std::fflush(stdout);
        ::close(a.fd);
        ::close(b.fd);
        return 24;
    }
    if (std::fabs(moved.position().x() - nx) > 0.5f) {
        std::printf("error=aoi_move_coord\n");
        std::fflush(stdout);
        ::close(a.fd);
        ::close(b.fd);
        return 24;
    }
    PrintKv("aoi_move_ok", 1);

    const std::string mail_body = "hello-from-a";
    uint64_t mail_id = 0;
    if (!DoPlayerMailSend(&a, b.st.player_id, "s3-mail", mail_body,
                          "s3mail:" + std::to_string(a.st.player_id) + ":" +
                              std::to_string(::getpid()),
                          &mail_id)) {
        ::close(a.fd);
        ::close(b.fd);
        return 25;
    }
    const auto mail_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
    bool got_notify = FindMailboxChanged(b.inbox, b.st.player_id);
    while (!got_notify && RemainingMs(mail_deadline) > 0)
        DrainPushes(&b, 300), got_notify = FindMailboxChanged(b.inbox, b.st.player_id);
    PrintKv("mailbox_changed", got_notify);
    uint64_t listed = mail_id;
    int list_n = 0;
    bool list_ok = false;
    for (int i = 0; i < 5 && !list_ok; ++i) {
        if (i > 0)
            DrainPushes(&b, 250);
        list_ok = DoMailList(&b, &listed, &list_n) && list_n > 0;
    }
    if (!list_ok) {
        std::printf("error=mail_list_empty\n");
        std::fflush(stdout);
        ::close(a.fd);
        ::close(b.fd);
        return 25;
    }
    const uint64_t get_id = mail_id != 0 ? mail_id : listed;
    std::string got_body;
    if (!DoMailGet(&b, get_id, &got_body) || got_body != mail_body) {
        std::printf("error=mail_body_mismatch got=%s\n", got_body.c_str());
        std::fflush(stdout);
        ::close(a.fd);
        ::close(b.fd);
        return 25;
    }
    PrintKv("mail_e2e_ok", 1);

    const uint64_t old_map = a.st.map_instance_id;
    const std::string old_owner = a.st.logic_id;
    ::close(a.fd);
    a.fd = -1;
    PrintKv("a_gw0_closed", 1);
    ::usleep(120000);
    game::EntitySnapshot left;
    const bool b_leave = WaitAoi(&b, 3, a.st.player_id, 8000, &left, &seen);
    PrintKv("b_aoi_leave_on_disconnect", b_leave);

    const size_t reconnect_from = b.inbox.size();
    a.fd = ConnectAndHello(h1, p1);
    if (a.fd < 0) {
        std::perror("connect gw1");
        ::close(b.fd);
        return 7;
    }
    int replay_n = 0;
    bool need_snap = false;
    if (!DoReconnect(a.fd, &a.st, a.st.last_server_seq, &replay_n, &need_snap)) {
        ::close(a.fd);
        ::close(b.fd);
        return 14;
    }
    DrainPushes(&a, 800);
    DrainPushes(&b, 800);
    game::EntitySnapshot reenter;
    const bool b_reenter = WaitAoi(&b, 1, a.st.player_id, 8000, &reenter, &seen, reconnect_from);
    PrintKv("b_aoi_enter_on_reconnect", b_reenter);
    PrintKv("reconnect_same_map", old_map);
    PrintKv("reconnect_owner", old_owner);
    int enter_n = 0;
    FindAoiEvent(b.inbox, 1, a.st.player_id, nullptr, &enter_n, reconnect_from);
    PrintKv("b_enter_events_for_a", static_cast<uint64_t>(enter_n));
    PrintKv("duplicate_entity", enter_n > 1 ? 1 : 0);
    if (!b_reenter) {
        std::printf("error=aoi_not_restored_after_reconnect\n");
        std::fflush(stdout);
        ::close(a.fd);
        ::close(b.fd);
        return 26;
    }
    PrintKv("reconnect_aoi_restored", 1);

    const size_t logout_from = b.inbox.size();
    if (!DoLogout(&a)) {
        ::close(a.fd);
        ::close(b.fd);
        return 27;
    }
    const bool b_leave_logout = WaitAoi(&b, 3, a.st.player_id, 8000, &left, &seen, logout_from);
    PrintKv("b_aoi_leave_on_logout", b_leave_logout);
    ::close(a.fd);
    a.fd = ConnectAndHello(h1, p1);
    bool session_released = false;
    if (a.fd >= 0) {
        int rn = 0;
        bool snap = false;
        SessionState stale = a.st;
        const bool rec = DoReconnect(a.fd, &stale, stale.last_server_seq, &rn, &snap);
        session_released = !rec;
        PrintKv("reconnect_after_logout", rec);
        ::close(a.fd);
        a.fd = -1;
    }
    PrintKv("session_released", session_released);
    if (!b_leave_logout || !session_released) {
        std::printf("error=logout_did_not_release\n");
        std::fflush(stdout);
        ::close(b.fd);
        return 27;
    }
    PrintKv("two_player_aoi_ok", 1);
    ::close(b.fd);
    return 0;
}

int CmdMapCapacity51(int argc, char **argv) {
    if (argc < 4)
        return 2;
    const char *host = argv[2];
    const int port = std::atoi(argv[3]);
    const uint64_t tpl = argc >= 5 ? std::strtoull(argv[4], nullptr, 10) : 1001ULL;
    const int n = argc >= 6 ? std::atoi(argv[5]) : 51;
    if (n < 2 || n > 120)
        return 2;
    std::vector<TcpSession> ss(static_cast<size_t>(n));
    std::unordered_map<uint64_t, int> counts;
    std::unordered_map<uint64_t, std::string> owners;
    int opened = 0;
    for (int i = 0; i < n; ++i) {
        auto &c = ss[static_cast<size_t>(i)];
        const std::string device =
            "e2e-cap-" + std::to_string(::getpid()) + "-" + std::to_string(i);
        if (!OpenRegister(&c, host, port, device, "e2epass1")) {
            std::printf("error=capacity_register i=%d\n", i);
            std::fflush(stdout);
            break;
        }
        if (!DoEnterMap(&c, tpl, 0)) {
            std::printf("error=capacity_enter i=%d\n", i);
            std::fflush(stdout);
            break;
        }
        ++opened;
        counts[c.st.map_instance_id] += 1;
        owners[c.st.map_instance_id] = c.st.logic_id;
        PrintKv(("cap_i" + std::to_string(i) + "_map").c_str(), c.st.map_instance_id);
    }
    int max_n = 0;
    std::vector<int> parts;
    parts.reserve(counts.size());
    for (const auto &kv : counts) {
        if (kv.second > max_n)
            max_n = kv.second;
        parts.push_back(kv.second);
        PrintKv(("occ_" + std::to_string(kv.first)).c_str(), static_cast<uint64_t>(kv.second));
        PrintKv(("owner_" + std::to_string(kv.first)).c_str(), owners[kv.first]);
    }
    std::sort(parts.begin(), parts.end(), std::greater<int>());
    std::string layout;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i)
            layout += ',';
        layout += std::to_string(parts[i]);
    }
    PrintKv("occupancy_layout", layout);
    PrintKv("capacity_players", static_cast<uint64_t>(opened));
    PrintKv("capacity_instances", static_cast<uint64_t>(counts.size()));
    PrintKv("max_instance_n", static_cast<uint64_t>(max_n));
    bool ok = opened == n && max_n <= 50 && !layout.empty();
    if (n == 51)
        ok = ok && layout == "50,1";
    else if (n == 101)
        ok = ok && layout == "50,50,1";
    else
        ok = ok && counts.size() >= 2;
    PrintKv("map_capacity_ok", ok);
    for (auto &c : ss) {
        if (c.fd >= 0)
            ::close(c.fd);
    }
    if (!ok) {
        std::printf("error=capacity_gate opened=%d instances=%zu max=%d\n", opened, counts.size(),
                    max_n);
        std::fflush(stdout);
        return 28;
    }
    return 0;
}

int CmdClientHello(int argc, char **argv) {
    if (argc < 4)
        return 2;
    const int fd = Connect(argv[2], std::atoi(argv[3]));
    if (fd < 0)
        return 6;
    game::GameResponse rsp;
    const bool ok = DoClientHello(fd, nullptr, 1, &rsp);
    PrintKv("hello_ok", ok);
    PrintKv("error_code", rsp.error_code());
    if (rsp.has_server_hello()) {
        PrintKv("schema_sha256", rsp.server_hello().schema_sha256());
        PrintKv("protocol_version", static_cast<uint64_t>(rsp.server_hello().protocol_version()));
        PrintKv("heartbeat_interval_ms",
                static_cast<uint64_t>(rsp.server_hello().heartbeat_interval_ms()));
        PrintKv("idle_timeout_ms", static_cast<uint64_t>(rsp.server_hello().idle_timeout_ms()));
        PrintKv("server_time_ms", static_cast<uint64_t>(rsp.server_hello().server_time_ms()));
        PrintKv("gameplay_config_version",
                static_cast<uint64_t>(rsp.server_hello().gameplay_config_version()));
        PrintKv("map_manifest_version",
                static_cast<uint64_t>(rsp.server_hello().map_manifest_version()));
        PrintKv("hello_maps_n", static_cast<uint64_t>(rsp.server_hello().maps_size()));
    }
    ::close(fd);
    return ok ? 0 : 12;
}

int CmdHelloRejectLogin(int argc, char **argv) {
    if (argc < 4)
        return 2;
    const char *mode = argc >= 5 ? argv[4] : "version";
    const int fd = Connect(argv[2], std::atoi(argv[3]));
    if (fd < 0)
        return 6;
    game::GameResponse hello_rsp;
    bool hello_ok = false;
    if (std::string(mode) == "version")
        hello_ok = DoClientHello(fd, nullptr, 99, &hello_rsp);
    else
        hello_ok = DoClientHello(fd, "deadbeef", 1, &hello_rsp);
    PrintKv("hello_ok", hello_ok);
    PrintKv("hello_error_code", hello_rsp.error_code());
    if (hello_rsp.has_server_hello())
        PrintKv("server_schema_sha256", hello_rsp.server_hello().schema_sha256());
    game::GameRequest login;
    login.set_seq(2);
    login.mutable_login()->set_player_id(1);
    login.mutable_login()->set_device_id("e2e-nohello");
    login.mutable_login()->set_credential("e2epass1");
    game::GameResponse lr;
    const bool got = Exchange(fd, login, &lr, 4000);
    PrintKv("login_exchanged", got);
    PrintKv("login_ok", got && lr.ok());
    PrintKv("login_error_code", got ? lr.error_code() : "NO_RESPONSE");
    const bool blocked = hello_ok == false && (!got || !lr.ok());
    PrintKv("login_blocked", blocked);
    ::close(fd);
    return blocked ? 0 : 12;
}

int CmdHeartbeat(int argc, char **argv) {
    if (argc < 4)
        return 2;
    const int fd = ConnectAndHello(argv[2], std::atoi(argv[3]));
    if (fd < 0)
        return 6;
    const int64_t echo =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    game::GameRequest req;
    req.set_seq(1);
    auto *hb = req.mutable_heartbeat();
    hb->set_client_monotonic_ms(echo);
    hb->set_echo_ms(echo);
    hb->set_last_server_seq(0);
    game::GameResponse rsp;
    if (!Exchange(fd, req, &rsp, 4000) || !rsp.ok() || !rsp.has_heartbeat()) {
        PrintKv("heartbeat_ok", 0);
        PrintKv("error_code", rsp.error_code());
        ::close(fd);
        return 12;
    }
    PrintKv("heartbeat_ok", 1);
    PrintKv("error_code", rsp.error_code());
    PrintKv("server_time_ms", static_cast<uint64_t>(rsp.heartbeat().server_time_ms()));
    PrintKv("echo_ms", static_cast<uint64_t>(rsp.heartbeat().echo_ms()));
    PrintKv("jitter_hint_ms", static_cast<uint64_t>(rsp.heartbeat().jitter_hint_ms()));
    PrintKv("echo_match", rsp.heartbeat().echo_ms() == echo);
    ::close(fd);
    return rsp.heartbeat().echo_ms() == echo ? 0 : 12;
}

int CmdHeartbeatFlood(int argc, char **argv) {
    if (argc < 4)
        return 2;
    const int fd = ConnectAndHello(argv[2], std::atoi(argv[3]));
    if (fd < 0)
        return 6;
    int limited = 0;
    int ok_n = 0;
    for (int i = 0; i < 20; ++i) {
        game::GameRequest req;
        req.set_seq(static_cast<uint64_t>(i + 1));
        req.mutable_heartbeat()->set_echo_ms(i);
        game::GameResponse rsp;
        if (!Exchange(fd, req, &rsp, 2000))
            break;
        if (rsp.error_code() == "ERR_RATE_LIMITED")
            ++limited;
        else if (rsp.ok())
            ++ok_n;
    }
    PrintKv("heartbeat_ok_n", ok_n);
    PrintKv("heartbeat_limited_n", limited);
    PrintKv("heartbeat_flood_ok", limited > 0);
    ::close(fd);
    return limited > 0 ? 0 : 12;
}

int CmdIdleReconnect(int argc, char **argv) {
    if (argc < 4)
        return 2;
    const char *host = argv[2];
    const int port = std::atoi(argv[3]);
    const int fd = Connect(host, port);
    if (fd < 0)
        return 6;
    game::GameResponse hello;
    if (!DoClientHello(fd, nullptr, 1, &hello) || !hello.has_server_hello()) {
        ::close(fd);
        return 12;
    }
    const uint32_t idle_ms = hello.server_hello().idle_timeout_ms();
    PrintKv("idle_timeout_ms", static_cast<uint64_t>(idle_ms));
    SessionState st;
    if (!DoRegisterLogin(fd, "e2e-idle-" + std::to_string(::getpid()), "e2epass1", &st)) {
        ::close(fd);
        return 12;
    }
    const int wait_ms = static_cast<int>(idle_ms + 1500);
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    game::GameRequest hb;
    hb.set_seq(90);
    hb.mutable_heartbeat()->set_echo_ms(1);
    game::GameResponse hbr;
    const bool still_alive = Exchange(fd, hb, &hbr, 1500);
    PrintKv("idle_conn_alive", still_alive);
    ::close(fd);
    const int fd2 = ConnectAndHello(host, port);
    if (fd2 < 0)
        return 6;
    game::GameRequest recon;
    recon.set_seq(1);
    auto *r = recon.mutable_reconnect();
    r->set_player_id(st.player_id);
    r->set_session_id(st.session_id);
    r->set_reconnect_ticket(st.token);
    r->set_last_server_seq(st.last_server_seq);
    game::GameResponse rr;
    const bool recon_ok = Exchange(fd2, recon, &rr, 8000) && rr.ok() && rr.has_reconnect() &&
                          rr.reconnect().ok();
    PrintKv("reconnect_ok", recon_ok);
    PrintKv("idle_disconnected_not_logout", recon_ok && !still_alive);
    ::close(fd2);
    return (recon_ok && !still_alive) ? 0 : 12;
}

int CmdS2WorldRecovery(int argc, char **argv) {
    if (argc < 6)
        return 2;
    const char *h0 = argv[2];
    const int p0 = std::atoi(argv[3]);
    const char *h1 = argv[4];
    const int p1 = std::atoi(argv[5]);
    const uint64_t tpl = argc >= 7 ? std::strtoull(argv[6], nullptr, 10) : 1001ULL;
    const std::string pass = "e2epass1";
    TcpSession a, b;
    if (!OpenRegister(&a, h0, p0, "e2e-s2-a-" + std::to_string(::getpid()), pass) ||
        !OpenRegister(&b, h1, p1, "e2e-s2-b-" + std::to_string(::getpid()), pass)) {
        if (a.fd >= 0)
            ::close(a.fd);
        if (b.fd >= 0)
            ::close(b.fd);
        return 12;
    }
    if (!DoEnterMap(&a, tpl, 0) || !DoEnterMap(&b, tpl, 0)) {
        ::close(a.fd);
        ::close(b.fd);
        return 13;
    }
    DrainPushes(&a, 400);
    DrainPushes(&b, 400);
    PrintKv("same_instance", a.st.map_instance_id == b.st.map_instance_id);
    game::FullStateSnapshotRsp sa;
    if (!DoWorldSnapshot(&a, &sa)) {
        ::close(a.fd);
        ::close(b.fd);
        return 14;
    }
    bool saw_b = false;
    for (int i = 0; i < sa.aoi_entities_size(); ++i) {
        if (sa.aoi_entities(i).player_id() == b.st.player_id)
            saw_b = true;
    }
    PrintKv("snapshot_sees_peer", saw_b);

    const float warmup_x = a.st.spawn_x + 0.8f;
    game::MoveRsp warmup;
    if (!DoMoveTo(&a, warmup_x, a.st.spawn_y, a.st.spawn_z, a.st.spawn_yaw, &warmup)) {
        ::close(a.fd);
        ::close(b.fd);
        return 15;
    }

    game::GameRequest bad;
    bad.set_seq(a.st.next_seq++);
    auto *mv = bad.mutable_move();
    mv->set_player_id(a.st.player_id);
    mv->set_map_instance_id(a.st.map_instance_id);
    mv->mutable_position()->set_x(1e8f);
    mv->mutable_position()->set_y(0);
    mv->mutable_position()->set_z(1e8f);
    game::GameResponse br;
    const bool illegal_ex = Rpc(&a, &bad, &br, 4000);
    const bool illegal_rejected = illegal_ex && !br.ok();
    PrintKv("illegal_pos_rejected", illegal_rejected);
    PrintKv("illegal_error_code", br.has_move() ? br.move().error_code() : br.error_code());

    const uint64_t old_seq = 1;
    game::GameRequest stale;
    stale.set_seq(old_seq);
    auto *sm = stale.mutable_move();
    sm->set_player_id(a.st.player_id);
    sm->set_map_instance_id(a.st.map_instance_id);
    sm->mutable_position()->set_x(a.st.spawn_x);
    sm->mutable_position()->set_y(a.st.spawn_y);
    sm->mutable_position()->set_z(a.st.spawn_z);
    game::GameResponse sr;
    Rpc(&a, &stale, &sr, 4000);
    PrintKv("stale_seq_error_code", sr.has_move() ? sr.move().error_code() : sr.error_code());
    PrintKv("stale_seq_message", sr.message());
    const bool stale_seq = !sr.ok() && (sr.move().error_code() == "ERR_STALE_SEQ" ||
                                        sr.error_code() == "ERR_STALE_SEQ" ||
                                        sr.error_code() == "ERR_CLIENT_SEQ_OUT_OF_ORDER" ||
                                        sr.message().find("STALE") != std::string::npos ||
                                        sr.message().find("OUT_OF_ORDER") != std::string::npos);
    PrintKv("stale_seq_rejected", stale_seq);

    game::GameRequest ack;
    auto *ak = ack.mutable_push_ack();
    ak->set_player_id(a.st.player_id);
    ak->set_ack_server_seq(1);
    ak->set_session_id(a.st.session_id);
    ak->set_fence_token(a.st.token);
    ak->set_generation(a.st.generation);
    game::GameResponse ar;
    Rpc(&a, &ack, &ar, 4000);
    PrintKv("old_ack_error_code", ar.error_code());
    const bool ack_resync = ar.error_code() == "ERR_AOI_RESYNC_REQUIRED" ||
                            ar.message() == "ERR_AOI_RESYNC_REQUIRED";
    PrintKv("ack_resync_required", ack_resync);
    game::FullStateSnapshotRsp after_gap;
    const bool gap_snap = DoWorldSnapshot(&a, &after_gap);
    PrintKv("gap_snapshot_ok", gap_snap);
    PrintKv("gap_baseline", after_gap.baseline_server_seq());

    const float nx = after_gap.has_self() ? after_gap.self().position().x() + 0.4f
                                          : a.st.spawn_x + 1.5f;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    game::MoveRsp moved;
    const bool moved_ok = DoMoveTo(&a, nx, a.st.spawn_y, a.st.spawn_z, a.st.spawn_yaw, &moved);
    PrintKv("post_snap_move_ok", moved_ok);

    SessionState ast = a.st;
    const std::string old_fence = a.st.token;
    ::close(a.fd);
    a.fd = -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const int fd1 = ConnectAndHello(h1, p1);
    if (fd1 < 0) {
        ::close(b.fd);
        return 7;
    }
    int replay_n = 0;
    bool need_snap = false;
    const bool recon = DoReconnect(fd1, &ast, 999999999ULL, &replay_n, &need_snap);
    PrintKv("gw1_reconnect_ok", recon);
    PrintKv("gw1_need_snapshot", need_snap);
    TcpSession a2;
    a2.fd = fd1;
    a2.st = ast;
    game::FullStateSnapshotRsp recon_snap;
    const bool recon_world = DoWorldSnapshot(&a2, &recon_snap);
    bool still_sees = false;
    for (int i = 0; i < recon_snap.aoi_entities_size(); ++i) {
        if (recon_snap.aoi_entities(i).player_id() == b.st.player_id)
            still_sees = true;
    }
    PrintKv("reconnect_snapshot_ok", recon_world);
    PrintKv("reconnect_same_map", recon_snap.map_instance_id() == b.st.map_instance_id);
    PrintKv("reconnect_sees_peer", still_sees);
    PrintKv("reconnect_recovery", recon_snap.recovery_reason());

    bool old_fence_rejected = false;
    {
        const int fd_old = ConnectAndHello(h1, p1);
        if (fd_old >= 0) {
            SessionState stale_st = ast;
            stale_st.token = old_fence;
            int rn = 0;
            bool ns = false;
            old_fence_rejected = !DoReconnect(fd_old, &stale_st, 0, &rn, &ns);
            ::close(fd_old);
        }
    }
    PrintKv("old_fence_rejected", old_fence_rejected);

    const bool ok = recon && recon_world && still_sees && illegal_rejected && gap_snap && moved_ok &&
                    stale_seq && old_fence_rejected &&
                    recon_snap.map_instance_id() == b.st.map_instance_id;
    PrintKv("s2_world_recovery_ok", ok);
    ::close(a2.fd);
    ::close(b.fd);
    return ok ? 0 : 12;
}

int CmdLastSafeRestart(int argc, char **argv) {
    if (argc < 4)
        return 2;
    TcpSession c;
    if (!OpenRegister(&c, argv[2], std::atoi(argv[3]),
                      "e2e-safe-" + std::to_string(::getpid()), "e2epass1")) {
        if (c.fd >= 0)
            ::close(c.fd);
        return 12;
    }
    if (!DoEnterMap(&c, 1001, 0)) {
        ::close(c.fd);
        return 13;
    }
    const float nx = c.st.spawn_x + 2.0f;
    game::MoveRsp mv;
    if (!DoMoveTo(&c, nx, c.st.spawn_y, c.st.spawn_z, c.st.spawn_yaw, &mv)) {
        ::close(c.fd);
        return 14;
    }
    PrintKv("saved_x", std::to_string(mv.position().x()));
    PrintKv("saved_z", std::to_string(mv.position().z()));
    PrintKv("player_id", c.st.player_id);
    PrintKv("session_id", c.st.session_id);
    PrintKv("token", c.st.token);
    PrintKv("map_instance_id", c.st.map_instance_id);
    PrintKv("map_template_id", c.st.map_template_id);
    ::close(c.fd);
    return 0;
}

int CmdLastSafeVerify(int argc, char **argv) {
    if (argc < 8)
        return 2;
    SessionState st;
    st.player_id = std::strtoull(argv[4], nullptr, 10);
    st.session_id = argv[5];
    st.token = argv[6];
    const float want_x = std::strtof(argv[7], nullptr);
    int fd = -1;
    bool recon = false;
    for (int i = 0; i < 8 && !recon; ++i) {
        if (fd >= 0)
            ::close(fd);
        fd = ConnectAndHello(argv[2], std::atoi(argv[3]));
        if (fd < 0)
            return 6;
        int rn = 0;
        bool snap = false;
        recon = DoReconnect(fd, &st, 0, &rn, &snap);
        if (!recon)
            ::usleep(400000);
    }
    if (!recon) {
        ::close(fd);
        return 12;
    }
    TcpSession c;
    c.fd = fd;
    c.st = st;
    if (c.st.map_template_id == 0)
        c.st.map_template_id = 1001;
    if (!DoEnterMap(&c, 1001, 0)) {
        ::close(fd);
        return 13;
    }
    game::FullStateSnapshotRsp fs;
    if (!DoWorldSnapshot(&c, &fs) || !fs.has_self()) {
        ::close(fd);
        return 14;
    }
    const float got = fs.self().position().x();
    PrintKv("restored_x", std::to_string(got));
    PrintKv("want_x", std::to_string(want_x));
    PrintKv("recovery_reason", fs.recovery_reason());
    const bool ok = std::fabs(got - want_x) < 1.5f;
    PrintKv("last_safe_restored", ok);
    ::close(fd);
    return ok ? 0 : 12;
}

int CmdS3Social(int argc, char **argv) {
    if (argc < 6)
        return 2;
    const char *h0 = argv[2];
    const int p0 = std::atoi(argv[3]);
    const char *h1 = argv[4];
    const int p1 = std::atoi(argv[5]);
    const uint64_t uniq =
        static_cast<uint64_t>(::getpid()) * 1000ULL +
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000);
    const std::string name_a = "s3a_" + std::to_string(uniq);
    const std::string name_b = "s3b_" + std::to_string(uniq);
    const std::string name_dup = "s3dup_" + std::to_string(uniq);
    TcpSession a, b, d1, d2;
    if (!OpenRegister(&a, h0, p0, "s3a-" + std::to_string(uniq), "e2epass1", name_a) ||
        !OpenRegister(&b, h1, p1, "s3b-" + std::to_string(uniq), "e2epass1", name_b)) {
        return 6;
    }
    PrintKv("a_player_id", a.st.player_id);
    PrintKv("b_player_id", b.st.player_id);
    PrintKv("a_name", a.st.player_name.empty() ? name_a : a.st.player_name);
    PrintKv("b_name", b.st.player_name.empty() ? name_b : b.st.player_name);

    auto rpc_code = [](const game::GameResponse &rsp) -> std::string {
        if (!rsp.error_code().empty() && rsp.error_code() != "OK")
            return rsp.error_code();
        if (rsp.has_get_player_brief() && !rsp.get_player_brief().error_code().empty())
            return rsp.get_player_brief().error_code();
        if (rsp.has_query_online_state() && !rsp.query_online_state().error_code().empty())
            return rsp.query_online_state().error_code();
        if (rsp.has_chat_send() && !rsp.chat_send().error_code().empty())
            return rsp.chat_send().error_code();
        if (rsp.has_friend_list() && !rsp.friend_list().error_code().empty())
            return rsp.friend_list().error_code();
        return rsp.error_code();
    };

    game::GameRequest br;
    auto *gb = br.mutable_get_player_brief();
    gb->set_player_id(a.st.player_id);
    gb->set_target_player_id(b.st.player_id);
    game::GameResponse brsp;
    if (!Rpc(&a, &br, &brsp, 8000) || !brsp.ok() || !brsp.has_get_player_brief() ||
        !brsp.get_player_brief().ok() ||
        brsp.get_player_brief().brief().player_id() != b.st.player_id) {
        PrintKv("brief_by_id_ok", false);
        return 12;
    }
    PrintKv("brief_by_id_ok", true);
    PrintKv("brief_by_id_name", brsp.get_player_brief().brief().player_name());
    PrintKv("brief_max_hp", static_cast<uint64_t>(brsp.get_player_brief().brief().max_hp()));

    game::GameRequest bn;
    auto *gn = bn.mutable_get_player_brief();
    gn->set_player_id(a.st.player_id);
    gn->set_player_name(b.st.player_name.empty() ? name_b : b.st.player_name);
    game::GameResponse nsp;
    if (!Rpc(&a, &bn, &nsp, 8000) || !nsp.ok() || !nsp.has_get_player_brief() ||
        !nsp.get_player_brief().ok() ||
        nsp.get_player_brief().brief().player_id() != b.st.player_id) {
        PrintKv("brief_by_name_ok", false);
        PrintKv("brief_by_name_msg", nsp.message());
        return 13;
    }
    PrintKv("brief_by_name_ok", true);

    game::GameRequest qo;
    auto *qs = qo.mutable_query_online_state();
    qs->set_player_id(a.st.player_id);
    qs->set_target_player_id(b.st.player_id);
    game::GameResponse qsp;
    if (!Rpc(&a, &qo, &qsp, 8000) || !qsp.ok() || !qsp.has_query_online_state() ||
        qsp.query_online_state().state() != "online") {
        PrintKv("online_state", qsp.has_query_online_state() ? qsp.query_online_state().state() : "");
        PrintKv("online_ok", false);
        return 14;
    }
    PrintKv("online_ok", true);
    PrintKv("online_state", qsp.query_online_state().state());

    game::GameRequest chat;
    auto *cs = chat.mutable_chat_send();
    cs->set_player_id(a.st.player_id);
    cs->set_channel("world");
    cs->set_text("s3 hello " + std::to_string(uniq));
    game::GameResponse crsp;
    if (!Rpc(&a, &chat, &crsp, 8000) || !crsp.ok() || !crsp.has_chat_send() ||
        !crsp.chat_send().ok() || crsp.chat_send().message_id() == 0) {
        PrintKv("chat_send_ok", false);
        PrintKv("chat_send_msg", crsp.message());
        PrintKv("chat_send_code", rpc_code(crsp));
        return 15;
    }
    const uint64_t mid = crsp.chat_send().message_id();
    PrintKv("chat_send_ok", true);
    PrintKv("chat_message_id", mid);

    bool saw = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !saw) {
        DrainPushes(&b, 400);
        for (const auto &outer : b.inbox) {
            game::GameResponse inner;
            const game::ChatNotify *n = nullptr;
            if (outer.has_chat_notify())
                n = &outer.chat_notify();
            else if (InnerFromPush(outer, &inner) && inner.has_chat_notify())
                n = &inner.chat_notify();
            else if (outer.has_server_push() &&
                     outer.server_push().message_type() == "chat.world.v1") {
                game::GameResponse payload;
                if (payload.ParseFromString(outer.server_push().payload()) &&
                    payload.has_chat_notify())
                    n = &payload.chat_notify();
            }
            if (n && n->message_id() == mid && n->sender_player_id() == a.st.player_id) {
                saw = true;
                PrintKv("chat_notify_text", n->text());
                break;
            }
        }
    }
    PrintKv("chat_notify_ok", saw);
    if (!saw)
        return 16;

    game::GameRequest fr;
    fr.mutable_friend_list()->set_player_id(a.st.player_id);
    game::GameResponse fsp;
    (void)Rpc(&a, &fr, &fsp, 5000);
    const bool friend_stub = rpc_code(fsp) == "NOT_IMPLEMENTED";
    PrintKv("friend_not_implemented", friend_stub);
    if (!friend_stub)
        return 21;

    if (!OpenRegister(&d1, h0, p0, "s3d1-" + std::to_string(uniq), "e2epass1", name_dup) ||
        !OpenRegister(&d2, h1, p1, "s3d2-" + std::to_string(uniq), "e2epass1", name_dup)) {
        PrintKv("dup_register_ok", false);
        return 17;
    }
    game::GameRequest amb;
    auto *am = amb.mutable_get_player_brief();
    am->set_player_id(a.st.player_id);
    am->set_player_name(name_dup);
    game::GameResponse asp;
    (void)Rpc(&a, &amb, &asp, 8000);
    const std::string acode = rpc_code(asp);
    PrintKv("name_ambiguous_ok", acode == "ERR_NAME_AMBIGUOUS");
    if (acode != "ERR_NAME_AMBIGUOUS") {
        PrintKv("name_ambiguous_code", acode);
        PrintKv("name_ambiguous_msg", asp.message());
        return 18;
    }

    int limited = 0;
    for (int i = 0; i < 16; ++i) {
        game::GameRequest qn;
        auto *q = qn.mutable_get_player_brief();
        q->set_player_id(a.st.player_id);
        q->set_player_name("nobody_s3_" + std::to_string(i) + "_" + std::to_string(uniq));
        game::GameResponse nr;
        (void)Rpc(&a, &qn, &nr, 3000);
        if (rpc_code(nr) == "ERR_RATE_LIMITED") {
            ++limited;
            break;
        }
    }
    PrintKv("name_rate_limited", limited > 0);
    if (limited == 0)
        return 19;

    int chat_lim = 0;
    for (int i = 0; i < 12; ++i) {
        game::GameRequest c2;
        auto *cbody = c2.mutable_chat_send();
        cbody->set_player_id(a.st.player_id);
        cbody->set_channel("world");
        cbody->set_text("flood");
        game::GameResponse r2;
        (void)Rpc(&a, &c2, &r2, 3000);
        if (rpc_code(r2) == "ERR_RATE_LIMITED") {
            ++chat_lim;
            break;
        }
    }
    PrintKv("chat_rate_limited", chat_lim > 0);
    if (chat_lim == 0)
        return 20;

    (void)DoLogout(&a);
    (void)DoLogout(&b);
    (void)DoLogout(&d1);
    (void)DoLogout(&d2);
    if (a.fd >= 0)
        ::close(a.fd);
    if (b.fd >= 0)
        ::close(b.fd);
    if (d1.fd >= 0)
        ::close(d1.fd);
    if (d2.fd >= 0)
        ::close(d2.fd);
    PrintKv("s3_social_ok", true);
    return 0;
}

int CmdDuplicateLogin(int argc, char **argv) {
    if (argc < 6)
        return 2;
    const char *h0 = argv[2];
    const int p0 = std::atoi(argv[3]);
    const char *h1 = argv[4];
    const int p1 = std::atoi(argv[5]);
    const std::string password = argc >= 7 ? argv[6] : "e2epass1";
    const std::string dev0 = std::string("dup-a-") + std::to_string(::getpid());
    const std::string dev1 = std::string("dup-b-") + std::to_string(::getpid());

    TcpSession a;
    if (!OpenRegister(&a, h0, p0, dev0, password, "dup"))
        return 12;
    const uint64_t pid = a.st.player_id;
    const std::string old_token = a.st.token;
    const uint64_t old_gen = a.st.generation;

    std::atomic<bool> got_notify{false};
    std::atomic<bool> saw_eof{false};
    std::atomic<bool> stop_drain{false};
    std::string reason;
    std::mutex reason_mu;
    std::thread drain([&]() {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
        while (!stop_drain.load() && std::chrono::steady_clock::now() < until) {
            game::GameResponse cur;
            const RecvResult st = RecvFrameEx(a.fd, &cur, 200);
            if (st == RecvResult::kTimeout)
                continue;
            if (st == RecvResult::kClosed) {
                saw_eof = true;
                break;
            }
            game::GameResponse inner;
            const game::SessionReplacedNotify *n = nullptr;
            if (cur.has_session_replaced())
                n = &cur.session_replaced();
            else if (InnerFromPush(cur, &inner) && inner.has_session_replaced())
                n = &inner.session_replaced();
            if (n) {
                std::lock_guard<std::mutex> lk(reason_mu);
                reason = n->reason_code();
                got_notify = true;
                break;
            }
        }
    });
    auto stop_old_drain = [&]() {
        stop_drain = true;
        if (drain.joinable())
            drain.join();
    };

    TcpSession b;
    b.fd = ConnectAndHello(h1, p1);
    if (b.fd < 0) {
        stop_old_drain();
        if (a.fd >= 0)
            ::close(a.fd);
        return 7;
    }
    b.st.next_seq = 1;
    if (!DoLoginExisting(b.fd, pid, dev1, password, &b.st)) {
        stop_old_drain();
        if (b.fd >= 0)
            ::close(b.fd);
        if (a.fd >= 0)
            ::close(a.fd);
        return 12;
    }
    PrintKv("new_generation", b.st.generation);
    PrintKv("generation_increased", b.st.generation > old_gen);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (!got_notify.load() && !saw_eof.load()) {
        if (RemainingMs(deadline) <= 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    stop_old_drain();
    {
        std::lock_guard<std::mutex> lk(reason_mu);
        PrintKv("old_got_session_replaced", got_notify.load());
        PrintKv("replaced_reason", reason);
    }
    PrintKv("old_conn_closed", saw_eof.load() || got_notify.load());

    game::PlayerAttributes attrs;
    const bool profile_ok = DoGetSelfProfile(&b, &attrs);
    PrintKv("new_profile_ok", profile_ok);

    TcpSession stale;
    stale.fd = ConnectAndHello(h0, p0);
    if (stale.fd < 0) {
        if (b.fd >= 0)
            ::close(b.fd);
        if (a.fd >= 0)
            ::close(a.fd);
        return 7;
    }
    stale.st.player_id = pid;
    stale.st.token = old_token;
    stale.st.session_id = a.st.session_id;
    stale.st.generation = old_gen;
    stale.st.next_seq = 10;
    game::PlayerAttributes stale_attrs;
    const bool stale_ok = DoGetSelfProfile(&stale, &stale_attrs);
    PrintKv("old_fence_rejected", !stale_ok);

    if (stale.fd >= 0)
        ::close(stale.fd);
    if (a.fd >= 0)
        ::close(a.fd);
    if (b.fd >= 0)
        ::close(b.fd);

    const bool pass = got_notify.load() && profile_ok && !stale_ok && b.st.generation > old_gen;
    PrintKv("duplicate_login_ok", pass);
    return pass ? 0 : 12;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <register-login|enter-map|reconnect|dual-gw|drain-login|"
                     "hold-kill-reconnect|register-login-profile|enter-public-map|move|"
                     "send-player-mail|mail-list|two-player-aoi|map-capacity-51|"
                     "unity-contract-check|login-profile|client-hello|hello-reject-login|"
                     "heartbeat|heartbeat-flood|idle-reconnect|s2-world-recovery|"
                     "last-safe-save|last-safe-verify|s3-social|duplicate-login> ...\n",
                     argv[0]);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "register-login")
        return CmdRegisterLogin(argc, argv);
    if (cmd == "enter-map")
        return CmdEnterMap(argc, argv);
    if (cmd == "reconnect")
        return CmdReconnect(argc, argv);
    if (cmd == "dual-gw")
        return CmdDualGw(argc, argv);
    if (cmd == "drain-login")
        return CmdDrainLogin(argc, argv);
    if (cmd == "hold-kill-reconnect")
        return CmdHoldKillReconnect(argc, argv);
    if (cmd == "register-login-profile")
        return CmdRegisterLoginProfile(argc, argv);
    if (cmd == "login-profile")
        return CmdLoginProfile(argc, argv);
    if (cmd == "enter-public-map")
        return CmdEnterPublicMap(argc, argv);
    if (cmd == "move")
        return CmdMove(argc, argv);
    if (cmd == "send-player-mail")
        return CmdSendPlayerMail(argc, argv);
    if (cmd == "mail-list")
        return CmdMailList(argc, argv);
    if (cmd == "two-player-aoi")
        return CmdTwoPlayerAoi(argc, argv);
    if (cmd == "map-capacity-51")
        return CmdMapCapacity51(argc, argv);
    if (cmd == "unity-contract-check")
        return CmdUnityContractCheck(argc, argv);
    if (cmd == "client-hello")
        return CmdClientHello(argc, argv);
    if (cmd == "hello-reject-login")
        return CmdHelloRejectLogin(argc, argv);
    if (cmd == "heartbeat")
        return CmdHeartbeat(argc, argv);
    if (cmd == "heartbeat-flood")
        return CmdHeartbeatFlood(argc, argv);
    if (cmd == "idle-reconnect")
        return CmdIdleReconnect(argc, argv);
    if (cmd == "s2-world-recovery")
        return CmdS2WorldRecovery(argc, argv);
    if (cmd == "last-safe-save")
        return CmdLastSafeRestart(argc, argv);
    if (cmd == "last-safe-verify")
        return CmdLastSafeVerify(argc, argv);
    if (cmd == "s3-social")
        return CmdS3Social(argc, argv);
    if (cmd == "duplicate-login")
        return CmdDuplicateLogin(argc, argv);
    std::fprintf(stderr, "unknown cmd %s\n", cmd.c_str());
    return 2;
}
