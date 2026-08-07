#include "GameRequestTransport.h"

#include "InProcessTransport.h"

std::mutex GameRequestTransport::mu_;
ITransport *GameRequestTransport::transport_ = nullptr;

void GameRequestTransport::Set(ITransport *transport) {
    std::lock_guard<std::mutex> lk(mu_);
    transport_ = transport;
}

ITransport &GameRequestTransport::Get() {
    std::lock_guard<std::mutex> lk(mu_);
    if (transport_)
        return *transport_;
    return InProcessTransport::Instance();
}
