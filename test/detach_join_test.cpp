/**
 * 启动后台线程后立刻 Stop，线程必须 join，不能在对象还活着时 detach。
 */
#include "ConnectionPool.h"
#include "Logging.h"
#include "MetricsDbWriter.h"
#include "PasswordExec.h"
#include "PlayerItemPersistQueue.h"

#include <chrono>
#include <cstdio>

int main() {
    Logger::setLogLevel(Logger::ERROR);

    PasswordExec::Instance().Post([]() {});
    PasswordExec::Instance().Stop();
    if (PasswordExec::Instance().Post([]() {})) {
        std::printf("FAIL password exec accepted work after stop\n");
        return 1;
    }

    MetricsDbWriter::Instance().StartPeriodic(nullptr, 1.0);
    MetricsDbWriter::Instance().Stop();
    MetricsDbWriter::Instance().Stop();

    PlayerItemPersistQueue::Instance().StartPeriodic(nullptr, 1.0);
    PlayerItemPersistQueue::Instance().Stop(std::chrono::milliseconds(1000));

    ConnectionPool *pool = ConnectionPool::getconnectionPool();
    pool->Stop();
    if (pool->getConnection()) {
        std::printf("FAIL mysql connection after pool stop\n");
        return 1;
    }

    std::printf("detach_join_test PASS\n");
    return 0;
}
