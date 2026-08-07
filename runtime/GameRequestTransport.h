#pragma once

#include "ITransport.h"

#include <memory>
#include <mutex>

/** 进程内当前游戏请求传输后端（InProcess 或 Brpc） */
class GameRequestTransport {
public:
    static void Set(ITransport *transport);
    static ITransport &Get();

private:
    static std::mutex mu_;
    static ITransport *transport_;
};
