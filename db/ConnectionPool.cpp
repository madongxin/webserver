/**
 * @file ConnectionPool.cpp
 * @brief MySQL 连接池实现：配置加载、预建、生产者扩容、空闲回收、RAII 借还
 */

#include "ConnectionPool.h"

#include "DbConfigPath.h"
#include "Logging.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

namespace {

std::atomic<bool> g_mysql_forbidden{false};

/** 去掉行首尾空白与 \r\n，便于解析 key=value */
std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

}  // namespace

void ConnectionPool::ForbidInit(const char *reason) {
    g_mysql_forbidden.store(true);
    LOG_WARN << "ConnectionPool ForbidInit: " << (reason ? reason : "forbidden");
}

bool ConnectionPool::IsForbidden() { return g_mysql_forbidden.load(); }

ConnectionPool *ConnectionPool::getconnectionPool() {
    // C++11 起局部 static 初始化线程安全；整个进程共用一个池
    static ConnectionPool pool;
    return &pool;
}

bool ConnectionPool::loadConfigFile() {
    const std::string &path = DbConfigPath::MysqlCnf();
    FILE *pf = std::fopen(path.c_str(), "r");
    if (!pf) {
        LOG_ERROR << "mysql.cnf not found: " << path;
        return false;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), pf)) {
        std::string str = Trim(line);
        if (str.empty() || str[0] == '#')
            continue;
        const auto eq = str.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(str.substr(0, eq));
        const std::string value = Trim(str.substr(eq + 1));
        if (key == "ip")
            _ip = value;
        else if (key == "port")
            _port = static_cast<unsigned short>(std::atoi(value.c_str()));
        else if (key == "username")
            _username = value;
        else if (key == "password")
            _password = value;
        else if (key == "dbname")
            _dbname = value;
        else if (key == "initSize")
            _initSize = std::atoi(value.c_str());
        else if (key == "maxSize")
            _maxSize = std::atoi(value.c_str());
        else if (key == "maxIdleTime")
            _maxIdleTime = std::atoi(value.c_str());
        else if (key == "maxConnectionTimeQue")
            // 配置名历史遗留；语义是「取连接最长等待毫秒数」
            _connectionTimeout = std::atoi(value.c_str());
    }
    std::fclose(pf);
    LOG_INFO << "mysql pool config loaded from " << path << " initSize=" << _initSize
             << " maxSize=" << _maxSize;
    return true;
}

ConnectionPool::ConnectionPool() {
    if (g_mysql_forbidden.load() ||
        (std::getenv("GAMEMESH_FORBID_MYSQL") &&
         std::strcmp(std::getenv("GAMEMESH_FORBID_MYSQL"), "1") == 0)) {
        LOG_WARN << "ConnectionPool: MySQL init skipped (formal data boundary)";
        return;
    }
    // 无配置或全部建连失败时保持 initialized_=false，业务侧自行降级
    if (!loadConfigFile())
        return;
    for (int i = 0; i < _initSize; ++i)
        addConnection();
    if (_connectionCnt.load() == 0)
        return;
    initialized_ = true;

    // 生产者 / 回收器与进程同寿命；失败不 join（池作为进程级单例）
    std::thread producer(std::bind(&ConnectionPool::produceConnectionTask, this));
    producer.detach();
    std::thread recycler(std::bind(&ConnectionPool::recycleConnectionTask, this));
    recycler.detach();
}

void ConnectionPool::addConnection() {
    Connection *conn = new Connection();
    if (!conn->connect(_ip, _port, _username, _password, _dbname)) {
        delete conn;
        LOG_ERROR << "addConnection: connect failed";
        return;
    }
    std::lock_guard<std::mutex> lock(_queueMutex);
    conn->refreshAliveTime();  // 入队时刻起算空闲时间
    _connectionQue.push(conn);
    _connectionCnt.fetch_add(1);
}

void ConnectionPool::produceConnectionTask() {
    // 策略：仅当「空闲队列为空」时扩容，有空闲则复用、不主动建连
    for (;;) {
        std::unique_lock<std::mutex> lock(_queueMutex);
        cv.wait(lock, [this]() { return _connectionQue.empty(); });
        if (_connectionCnt.load() < _maxSize) {
            // addConnection 内部再次加锁，故先 unlock，避免自死锁
            lock.unlock();
            addConnection();
        }
        // 唤醒 getConnection 等待者（无论是否扩成功）
        cv.notify_all();
    }
}

void ConnectionPool::recycleConnectionTask() {
    // 队列是 FIFO：队首是归还最早、空闲最久的连接；从队首连续回收即可
    const clock_t max_idle_ticks = static_cast<clock_t>(_maxIdleTime) * CLOCKS_PER_SEC;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::unique_lock<std::mutex> lock(_queueMutex);
        while (_connectionCnt.load() > _initSize && !_connectionQue.empty()) {
            Connection *conn = _connectionQue.front();
            if (conn->getAlieTime() >= max_idle_ticks) {
                _connectionQue.pop();
                _connectionCnt.fetch_sub(1);
                delete conn;  // ~Connection → mysql_close
            } else {
                // 队首都未超时，后面更新；结束本轮
                break;
            }
        }
    }
}

std::shared_ptr<Connection> ConnectionPool::getConnection() {
    std::unique_lock<std::mutex> lock(_queueMutex);
    while (_connectionQue.empty()) {
        // 等待：业务归还（deleter）或生产者 addConnection 后的 notify
        if (cv.wait_for(lock, std::chrono::milliseconds(_connectionTimeout)) ==
            std::cv_status::timeout) {
            if (_connectionQue.empty()) {
                LOG_ERROR << "getConnection: timeout";
                return nullptr;
            }
        }
        // 被虚假唤醒或超时瞬间有人归还：循环再检查 empty
    }

    Connection *raw = _connectionQue.front();
    _connectionQue.pop();

    // 自定义 deleter：不 delete，而是还回空闲队列（RAII 借还）
    std::shared_ptr<Connection> connptr(raw, [this](Connection *conn) {
        std::lock_guard<std::mutex> locker(_queueMutex);
        conn->refreshAliveTime();
        _connectionQue.push(conn);
        cv.notify_all();  // 唤醒等待借连接的线程，以及生产者（若队列曾空）
    });

    // 队列可能因此变空，通知生产者考虑扩容
    cv.notify_all();
    return connptr;
}
