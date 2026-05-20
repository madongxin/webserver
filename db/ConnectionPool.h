#pragma once

#include "Connection.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

class ConnectionPool {
public:
    static ConnectionPool *getconnectionPool();
    std::shared_ptr<Connection> getConnection();
    bool isInitialized() const { return initialized_; }
    const std::string &dbname() const { return _dbname; }

private:
    ConnectionPool();

    bool loadConfigFile();
    void produceConnectionTask();
    void recycleConnectionTask();
    void addConnection();

    std::string _ip;
    unsigned short _port = 3306;
    std::string _username;
    std::string _password;
    std::string _dbname;
    int _initSize = 5;
    int _maxSize = 64;
    int _maxIdleTime = 60;
    int _connectionTimeout = 3000;

    std::queue<Connection *> _connectionQue;
    std::mutex _queueMutex;
    std::atomic<int> _connectionCnt{0};
    std::condition_variable cv;
    bool initialized_ = false;
};
