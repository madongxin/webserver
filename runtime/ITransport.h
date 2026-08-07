#pragma once

#include "SessionHandle.h"
#include "ReplySink.h"

#include <memory>
#include <string>

/** 投递玩家请求的传输抽象；阶段 1 仅 InProcess，阶段 2 再加 BrpcTransport */
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void PostPlayerRequest(const SessionHandle &handle,
                                   std::string request_payload,
                                   std::shared_ptr<ReplySink> sink) = 0;
};
