#pragma once

#include <memory>
#include <string>

class TcpConnection;

/** 业务回包抽象：Handler 不直接握 TcpConnection* */
class ReplySink {
public:
    virtual ~ReplySink() = default;
    /** response_frame: 已含 4 字节长度前缀的完整外网帧 */
    virtual void SendFrame(const std::string &response_frame) = 0;
    /** 将关闭投递到连接所属 EventLoop；默认空操作 */
    virtual void CloseConnection() {}
    /** 连接弱引用；Push 回调必须用连接而非 per-message sink */
    virtual std::shared_ptr<TcpConnection> tcp_connection() const { return nullptr; }
};
