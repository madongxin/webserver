/**
 * 阶段 0：socketpair 集成测试 — EPOLLOUT 关闭、发送队列上限、关闭清理
 */
#include "Buffer.h"
#include "EventLoop.h"
#include "TcpConnection.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <chrono>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <thread>
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

void TestLatestMoveSendsWhenIdle() {
    int fds[2];
    Expect(MakeNonBlockingPair(fds), "socketpair latest idle");
    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, fds[0], 12);
    conn->set_close_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->set_message_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->ConnectionEstablished();
    const std::string frame(32, 'm');
    Expect(conn->TrySendLatestMove(frame), "idle latest move accepted");
    char tmp[64];
    const ssize_t n = ::read(fds[1], tmp, sizeof(tmp));
    Expect(n == static_cast<ssize_t>(frame.size()), "idle latest move reached peer");
    Expect(conn->send_buf()->readablebytes() == 0, "idle latest move not held");
    conn->HandleClose();
    conn->ConnectionDestructor();
    ::close(fds[1]);
}

void TestLatestMoveHeldWhileKernelBusy() {
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    Expect(lfd >= 0, "tcp listen");
    int yes = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    Expect(::bind(lfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "bind");
    Expect(::listen(lfd, 1) == 0, "listen");
    socklen_t alen = sizeof(addr);
    Expect(::getsockname(lfd, reinterpret_cast<sockaddr *>(&addr), &alen) == 0, "getsockname");

    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    Expect(cfd >= 0, "tcp client");
    Expect(::connect(cfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "connect");
    int afd = ::accept(lfd, nullptr, nullptr);
    Expect(afd >= 0, "accept");
    int flags = ::fcntl(cfd, F_GETFL, 0);
    ::fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
    flags = ::fcntl(afd, F_GETFL, 0);
    ::fcntl(afd, F_SETFL, flags | O_NONBLOCK);

    std::string block(4096, 'B');
    while (true) {
        const ssize_t n = ::write(cfd, block.data(), block.size());
        if (n > 0)
            continue;
        break;
    }

    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, cfd, 11);
    conn->set_close_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->set_message_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->ConnectionEstablished();
    Expect(conn->unsent_bytes() > 8 * 1024, "kernel unsent above move budget");

    const std::string frame(128, 'M');
    Expect(conn->TrySendLatestMove(frame), "latest move parked while kernel busy");
    Expect(conn->send_buf()->readablebytes() == static_cast<int>(frame.size()),
           "one latest frame in app buffer");
    Expect(!conn->IsWriting(), "held move does not arm EPOLLOUT");
    const int unsent_before = conn->unsent_bytes();
    conn->Write();
    Expect(conn->send_buf()->readablebytes() == static_cast<int>(frame.size()),
           "write leaves held move in app buffer");
    Expect(conn->unsent_bytes() >= unsent_before, "held move not pushed into kernel");

    const std::string ack(16, 'A');
    const auto ack_rc = conn->TrySendControl(ack);
    Expect(ack_rc == TcpConnection::WrittenDirect || ack_rc == TcpConnection::Buffered,
           "ack prepended ahead of held move");
    const std::string left = conn->CopySendBuf();
    Expect(left.size() >= frame.size() &&
               left.compare(left.size() - frame.size(), frame.size(), frame) == 0,
           "move frame stays at the tail");

    conn->HandleClose();
    conn->ConnectionDestructor();
    ::close(afd);
    ::close(lfd);
}

void TestParkedMoveRetriesAfterKernelDrains() {
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    Expect(lfd >= 0, "tcp listen retry");
    int yes = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    Expect(::bind(lfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "bind retry");
    Expect(::listen(lfd, 1) == 0, "listen retry");
    socklen_t alen = sizeof(addr);
    Expect(::getsockname(lfd, reinterpret_cast<sockaddr *>(&addr), &alen) == 0, "getsockname retry");

    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    Expect(cfd >= 0, "tcp client retry");
    Expect(::connect(cfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "connect retry");
    int afd = ::accept(lfd, nullptr, nullptr);
    Expect(afd >= 0, "accept retry");
    int flags = ::fcntl(cfd, F_GETFL, 0);
    ::fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
    flags = ::fcntl(afd, F_GETFL, 0);
    ::fcntl(afd, F_SETFL, flags | O_NONBLOCK);

    std::string block(4096, 'B');
    while (true) {
        const ssize_t n = ::write(cfd, block.data(), block.size());
        if (n > 0)
            continue;
        break;
    }

    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, cfd, 12);
    conn->set_close_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->set_message_callback([](const std::shared_ptr<TcpConnection> &) {});
    conn->ConnectionEstablished();
    const std::string frame(64, 'M');
    Expect(conn->TrySendLatestMove(frame), "parked move accepted");
    Expect(conn->holding_latest_move(), "move is held");

    std::thread drain([&]() {
        char buf[4096];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            const ssize_t n = ::read(afd, buf, sizeof(buf));
            if (n > 0)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            break;
        }
    });
    loop.RunAfter(0.45, [&loop]() { loop.Quit(); });
    loop.Loop();
    drain.join();
    Expect(!conn->holding_latest_move(), "timer released the parked move");
    Expect(conn->send_buf()->readablebytes() == 0, "parked move left the app buffer");

    conn->HandleClose();
    conn->ConnectionDestructor();
    ::close(afd);
    ::close(lfd);
}

}  // namespace

int main() {
    TestConnIdWidth();
    TestCleanupOnClose();
    TestSendQueueLimitCloses();
    TestDisableWriteAfterDrain();
    TestLatestMoveSendsWhenIdle();
    TestLatestMoveHeldWhileKernelBusy();
    TestParkedMoveRetriesAfterKernelDrains();
    if (g_fail) {
        std::cerr << "reactor_integration_test failures=" << g_fail << "\n";
        return 1;
    }
    std::cout << "reactor_integration_test PASS\n";
    return 0;
}
