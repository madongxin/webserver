#pragma once

#include <string>

/** 业务回包抽象：Handler 不直接握 TcpConnection* */
class ReplySink {
public:
    virtual ~ReplySink() = default;
    /** response_frame: 已含 4 字节长度前缀的完整外网帧 */
    virtual void SendFrame(const std::string &response_frame) = 0;
};
