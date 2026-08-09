#pragma once

/**
 * @file ConnectionPool.h
 * @brief MySQL 连接池（单例）：预建、按需扩容、空闲回收、RAII 借还
 *
 * =============================================================================
 * 设计动机
 * =============================================================================
 * MySQL 建连成本高（TCP + 认证）。业务侧（邮件 MailStore、道具落库、
 * Metrics 刷库等）统一从本池借连接，用完归还，避免每次 SQL 新建/关闭。
 *
 * =============================================================================
 * 架构
 * =============================================================================
 *
 *   ConnectionPool（Meyers 单例）
 *     ├── _connectionQue      空闲 Connection* 队列
 *     ├── produceConnectionTask  后台线程：队列空且未达 maxSize 时新建
 *     ├── recycleConnectionTask  后台线程：多余且空闲过久的连接销毁
 *     └── getConnection()        借出 shared_ptr，deleter 负责还池
 *
 *   Connection（见 Connection.h）
 *     └── 封装 MYSQL*：connect / update / query，并记录空闲起点时间
 *
 * =============================================================================
 * 典型流程
 * =============================================================================
 *
 * 【初始化】（首次 getconnectionPool()）
 *   读 config/mysql.cnf → 循环 addConnection() 建 initSize 根
 *   → 至少成功 1 根则 initialized_=true
 *   → detach 生产者线程 + 回收线程
 *
 * 【借出】getConnection()
 *   队列非空：pop → shared_ptr(自定义 deleter)
 *   队列为空：cv.wait_for(超时) 等待归还或生产者扩容；超时返回 nullptr
 *
 * 【归还】shared_ptr 析构 → deleter
 *   refreshAliveTime → push 回队列 → notify 等待者 / 生产者
 *
 * 【扩容】produceConnectionTask
 *   等队列为空 → 若 connectionCnt < maxSize → addConnection
 *
 * 【回收】recycleConnectionTask
 *   每 500ms：若总数 > initSize 且队首空闲 ≥ maxIdleTime → delete
 *
 * =============================================================================
 * 使用约定
 * =============================================================================
 * - 先查 isInitialized()，再 getConnection()；拿到 nullptr 表示超时或池不可用
 * - 勿在 Reactor IO 线程里长时间占着连接跑重 SQL（Metrics/道具刷库已丢后台线程）
 * - 归还后连接未做 mysql_ping；长空闲可能被服务端踢掉（已知局限）
 *
 * 配置见 config/mysql.cnf（路径由 DbConfigPath::MysqlCnf() 解析）
 */

#include "Connection.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

class ConnectionPool {
public:
    /**
     * 获取进程内唯一连接池（Meyers singleton，线程安全初始化）。
     * 首次调用会触发私有构造：读配置、预建连接、拉起后台线程。
     */
    static ConnectionPool *getconnectionPool();

    /**
     * 在首次 getconnectionPool() 之前调用：禁止本进程建立 MySQL 池（正式模式数据边界）。
     * 已初始化则不再建连；后续 isInitialized() 为 false。
     */
    static void ForbidInit(const char *reason);

    /** 测试/运维：是否被 ForbidInit */
    static bool IsForbidden();

    /**
     * 从池中借出一条空闲连接。
     * @return 非空 shared_ptr：离开作用域自动还池；nullptr：等待超时或队列仍空
     *
     * 线程安全；可被多个业务线程并发调用。
     */
    std::shared_ptr<Connection> getConnection();

    /** 配置加载且至少建连成功 1 根时为 true；否则业务应降级/跳过 MySQL */
    bool isInitialized() const { return initialized_; }

    /** 当前配置的数据库名（供 /api/db/ping 等探活展示） */
    const std::string &dbname() const { return _dbname; }

private:
    ConnectionPool();

    /** 解析 mysql.cnf：ip/port/账号/库名及池参数；文件不存在返回 false */
    bool loadConfigFile();

    /** 后台循环：空闲队列为空时尝试扩容到不超过 _maxSize */
    void produceConnectionTask();

    /** 后台循环：销毁「超出 initSize 且空闲过久」的队首连接 */
    void recycleConnectionTask();

    /** 新建一条 Connection 并入队；失败则 delete，不增加 _connectionCnt */
    void addConnection();

    // ---- 来自 mysql.cnf 的连接参数 ----
    std::string _ip;
    unsigned short _port = 3306;
    std::string _username;
    std::string _password;
    std::string _dbname;

    // ---- 池容量与超时（均可被配置覆盖）----
    int _initSize = 5;              ///< 启动预建数量；回收时不少于此值
    int _maxSize = 64;              ///< 池内连接总数上限（含借出中的）
    int _maxIdleTime = 60;          ///< 空闲多久可被回收（秒）
    int _connectionTimeout = 3000;  ///< getConnection 等待空闲连接的超时（毫秒）

    // ---- 运行时状态 ----
    std::queue<Connection *> _connectionQue;  ///< 空闲连接；借出中的不在队列里
    std::mutex _queueMutex;                   ///< 保护队列及借还/回收临界区
    std::atomic<int> _connectionCnt{0};       ///< 已创建且未销毁的连接总数
    std::condition_variable cv;               ///< 空池等待 / 归还与扩容唤醒
    bool initialized_ = false;                ///< 是否可对外提供连接
};
