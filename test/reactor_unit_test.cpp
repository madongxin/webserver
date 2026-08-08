/**
 * 阶段 0：Connection ID / Buffer / ProtoFraming 单元测试
 */
#include "Buffer.h"
#include "ProtoFraming.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

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

std::string MakeFrame(const std::string &payload) {
    std::string out;
    Expect(gameproto::EncodeFrame(payload, &out), "EncodeFrame");
    return out;
}

void TestConnIdMonotonic() {
    // 模拟 TcpServer：进程内单调 uint64，不回绕到 1
    uint64_t next = 1;
    std::unordered_set<uint64_t> seen;
    bool unique = true;
    bool sequential = true;
    for (int i = 0; i < 10000; ++i) {
        const uint64_t id = next++;
        if (!seen.insert(id).second)
            unique = false;
        if (id != static_cast<uint64_t>(i + 1))
            sequential = false;
    }
    Expect(unique, "10000 conn ids unique");
    Expect(sequential, "10000 conn ids sequential");
    Expect(next == 10001, "next after 10000");
    Expect(sizeof(next) == 8, "conn id uint64 width");
}

void TestBufferExactConsume() {
    Buffer buf;
    const char *data = "hello-world";
    buf.Append(data, 11);
    Expect(buf.readablebytes() == 11, "readable 11");
    buf.Retrieve(11);
    Expect(buf.readablebytes() == 0, "exact consume all");
    Expect(buf.prependablebytes() == kPrePendIndex, "reset prepend");
}

void TestHalfPacketThenComplete() {
    const std::string payload = "abc";
    const std::string frame = MakeFrame(payload);
    std::string stream = frame.substr(0, 3);
    std::string out;
    Expect(gameproto::DecodeOneFrame(&stream, &out) == gameproto::FrameDecodeResult::Incomplete,
           "half header Incomplete");
    stream.append(frame.substr(3));
    Expect(gameproto::DecodeOneFrame(&stream, &out) == gameproto::FrameDecodeResult::Complete,
           "second input Complete");
    Expect(out == payload, "payload match");
    Expect(stream.empty(), "stream consumed");
}

void TestCoalescedPackets() {
    std::string stream = MakeFrame("one") + MakeFrame("two") + MakeFrame("three");
    std::vector<std::string> got;
    std::string payload;
    while (gameproto::DecodeOneFrame(&stream, &payload) == gameproto::FrameDecodeResult::Complete)
        got.push_back(payload);
    Expect(got.size() == 3, "three frames");
    Expect(got[0] == "one" && got[1] == "two" && got[2] == "three", "frame order");
    Expect(stream.empty(), "all consumed");
}

void TestFramesPlusTrailingHalf() {
    std::string stream = MakeFrame("a") + MakeFrame("b");
    const std::string third = MakeFrame("c");
    stream.append(third.substr(0, 2));
    std::vector<std::string> got;
    std::string payload;
    for (;;) {
        auto r = gameproto::DecodeOneFrame(&stream, &payload);
        if (r != gameproto::FrameDecodeResult::Complete)
            break;
        got.push_back(payload);
    }
    Expect(got.size() == 2, "two complete frames");
    Expect(stream.size() == 2, "trailing half retained");
    Expect(gameproto::DecodeOneFrame(&stream, &payload) == gameproto::FrameDecodeResult::Incomplete,
           "trailing Incomplete");
}

void TestInvalidLengthRejected() {
    uint32_t be = htonl(0);
    std::string stream(reinterpret_cast<const char *>(&be), 4);
    std::string payload;
    Expect(gameproto::DecodeOneFrame(&stream, &payload) == gameproto::FrameDecodeResult::Invalid,
           "zero length Invalid");

    be = htonl(gameproto::kMaxFrameSize + 1);
    stream.assign(reinterpret_cast<const char *>(&be), 4);
    stream.append(16, 'x');
    Expect(gameproto::DecodeOneFrame(&stream, &payload) == gameproto::FrameDecodeResult::Invalid,
           "oversize Invalid");
}

void TestOversizeFramePolicy() {
    // 超大声明长度：Invalid；调用方应清空并关连（此处验证不无限增长消费）
    uint32_t be = htonl(gameproto::kMaxFrameSize + 100);
    std::string stream(reinterpret_cast<const char *>(&be), 4);
    stream.append(1024, 'z');
    const size_t before = stream.size();
    std::string payload;
    auto r = gameproto::DecodeOneFrame(&stream, &payload);
    Expect(r == gameproto::FrameDecodeResult::Invalid, "oversize Invalid result");
    Expect(stream.size() == before, "Invalid does not consume");
    stream.clear();
    Expect(stream.empty(), "caller clears stream");
}

}  // namespace

int main() {
    TestConnIdMonotonic();
    TestBufferExactConsume();
    TestHalfPacketThenComplete();
    TestCoalescedPackets();
    TestFramesPlusTrailingHalf();
    TestInvalidLengthRejected();
    TestOversizeFramePolicy();
    if (g_fail) {
        std::cerr << "reactor_unit_test failures=" << g_fail << "\n";
        return 1;
    }
    std::cout << "reactor_unit_test PASS\n";
    return 0;
}
