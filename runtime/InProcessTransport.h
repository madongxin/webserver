#pragma once

#include "ITransport.h"

/** 单进程内投递：串行队列上调用 GameService::HandleFrame */
class InProcessTransport : public ITransport {
public:
    static InProcessTransport &Instance();

    void EnsureStarted(int shard_count = 0);

    void PostPlayerRequest(const SessionHandle &handle,
                           std::string request_payload,
                           std::shared_ptr<ReplySink> sink) override;
};
