#include "ConnectionPool.h"

#include "DbConfigPath.h"
#include "Logging.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

}  // namespace

ConnectionPool *ConnectionPool::getconnectionPool() {
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
            _connectionTimeout = std::atoi(value.c_str());
    }
    std::fclose(pf);
    LOG_INFO << "mysql pool config loaded from " << path << " initSize=" << _initSize
             << " maxSize=" << _maxSize;
    return true;
}

ConnectionPool::ConnectionPool() {
    if (!loadConfigFile())
        return;
    for (int i = 0; i < _initSize; ++i)
        addConnection();
    if (_connectionCnt.load() == 0)
        return;
    initialized_ = true;
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
    conn->refreshAliveTime();
    _connectionQue.push(conn);
    _connectionCnt.fetch_add(1);
}

void ConnectionPool::produceConnectionTask() {
    for (;;) {
        std::unique_lock<std::mutex> lock(_queueMutex);
        cv.wait(lock, [this]() { return _connectionQue.empty(); });
        if (_connectionCnt.load() < _maxSize) {
            lock.unlock();
            addConnection();
        }
        cv.notify_all();
    }
}

void ConnectionPool::recycleConnectionTask() {
    const clock_t max_idle_ticks = static_cast<clock_t>(_maxIdleTime) * CLOCKS_PER_SEC;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::unique_lock<std::mutex> lock(_queueMutex);
        while (_connectionCnt.load() > _initSize && !_connectionQue.empty()) {
            Connection *conn = _connectionQue.front();
            if (conn->getAlieTime() >= max_idle_ticks) {
                _connectionQue.pop();
                _connectionCnt.fetch_sub(1);
                delete conn;
            } else {
                break;
            }
        }
    }
}

std::shared_ptr<Connection> ConnectionPool::getConnection() {
    std::unique_lock<std::mutex> lock(_queueMutex);
    while (_connectionQue.empty()) {
        if (cv.wait_for(lock, std::chrono::milliseconds(_connectionTimeout)) ==
            std::cv_status::timeout) {
            if (_connectionQue.empty()) {
                LOG_ERROR << "getConnection: timeout";
                return nullptr;
            }
        }
    }
    Connection *raw = _connectionQue.front();
    _connectionQue.pop();
    std::shared_ptr<Connection> connptr(raw, [this](Connection *conn) {
        std::lock_guard<std::mutex> locker(_queueMutex);
        conn->refreshAliveTime();
        _connectionQue.push(conn);
        cv.notify_all();
    });
    cv.notify_all();
    return connptr;
}
