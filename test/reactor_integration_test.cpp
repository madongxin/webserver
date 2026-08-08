/**
 * 阶段 0：socketpair 集成测试 — EPOLLOUT 关闭、发送队列上限、关闭清理
 */
#include "Buffer.h"
#include "EventLoop.h"
#include "TcpConnection.h"

#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/socket.h>

namespace {

int g_fail = 0;
void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        ++g_fail;
    } else {
        std::cout << "OK: " << msg << "\n";
    }
}

bool MakeNonBlockingPair(int fds[2]) {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return false;
    for (int i = 0; i < 2; ++i) {
        int flags = ::fcntl(fds[i], F_GETFL, 0);
        if (flags < 0 || ::fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0)
            return false;
    }
    return true;
}

void TestConnIdWidth() {
    int fds[2];
    Expect(MakeNonBlockingPair(fds), "socketpair id");
    EventLoop loop;
    const uint64_t big_id = 1000003ull;
    auto conn = std::make_shared<TcpConnection>(&loop, fds[0], big_id);
    conn->set_close_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->set_message_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->ConnectionEstablished();
    Expect(conn->id() == big_id, "uint64 connection id preserved");
    Expect(sizeof(conn->id()) == 8, "id width 8");
    conn->HandleClose();
    conn->ConnectionDestructor();
    ::close(fds[1]);
}

void TestCleanupOnClose() {
    int fds[2];
    Expect(MakeNonBlockingPair(fds), "socketpair cleanup");
    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, fds[0], 3);
    conn->set_close_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->set_message_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->ConnectionEstablished();
    conn->proto_stream().assign("pending-half");
    conn->HandleClose();
    Expect(conn->state() == TcpConnection::Disconected, "disconnected");
    Expect(conn->proto_stream().empty(), "stream cleared on close");
    conn->ConnectionDestructor();
    ::close(fds[1]);
}

void TestSendQueueLimitCloses() {
    int fds[2];
    Expect(MakeNonBlockingPair(fds), "socketpair bp");
    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, fds[0], 2);
    bool closed = false;
    conn->set_close_callback([&](const std::shared_ptr<TcpConnection> &c) {
        if (c->force_closed_for_backpressure())
            closed = true;
    });
    conn->set_message_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->ConnectionEstablished();
    conn->set_max_send_buf_bytes(4096);
    int snd = 1024;
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));

    for (int i = 0; i < 64; ++i) {
        if (conn->state() != TcpConnection::Connected)
            break;
        conn->Send(std::string(2048, 'B'));
    }
    Expect(closed || conn->force_closed_for_backpressure(),
           "slow peer closed by send queue limit");
    if (conn->state() != TcpConnection::Disconected)
        conn->HandleClose();
    conn->ConnectionDestructor();
    ::close(fds[1]);
}

void TestDisableWriteAfterDrain() {
    int fds[2];
    Expect(MakeNonBlockingPair(fds), "socketpair drain");
    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, fds[0], 1);
    conn->set_close_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->set_message_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->ConnectionEstablished();

    int snd = 4 * 1024;
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    conn->set_max_send_buf_bytes(1024 * 1024);
    conn->Send(std::string(64 * 1024, 'A'));
    Expect(conn->IsWriting() || conn->send_buf()->readablebytes() == 0,
           "EnableWrite when kernel buffer fills");

    char tmp[4096];
    for (int round = 0; round < 256; ++round) {
        // 先读对端，再写剩余
        for (;;) {
            ssize_t n = ::read(fds[1], tmp, sizeof(tmp));
            if (n > 0)
                continue;
            break;  // EAGAIN / EOF
        }
        if (conn->send_buf()->readablebytes() == 0)
            break;
        conn->Write();
    }

    Expect(conn->send_buf()->readablebytes() == 0, "send queue drained");
    Expect(!conn->IsWriting(), "DisableWrite after drain");

    conn->HandleClose();
    conn->ConnectionDestructor();
    ::close(fds[1]);
}

}  // namespace

int main() {
    TestConnIdWidth();
    TestCleanupOnClose();
    TestSendQueueLimitCloses();
    TestDisableWriteAfterDrain();
    if (g_fail) {
        std::cerr << "reactor_integration_test failures=" << g_fail << "\n";
        return 1;
    }
    std::cout << "reactor_integration_test PASS\n";
    return 0;
}
