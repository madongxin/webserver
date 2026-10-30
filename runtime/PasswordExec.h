#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/** 把 PBKDF2 挪出 brpc 工作线程。Stop() 先停循环再 join，析构同样如此。 */
class PasswordExec {
public:
    static PasswordExec &Instance();

    bool Post(std::function<void()> fn);
    void Stop();

private:
    PasswordExec();
    ~PasswordExec();
    void Loop();

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> workers_;
    bool stopped_ = false;
};
