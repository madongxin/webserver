#pragma once

/**
 * @file RocksDbClient.h
 * @brief RocksDB 同步客户端封装（本地 KV，与 MySQL/Redis 并存）
 *
 * =============================================================================
 * 模块定位
 * =============================================================================
 *
 *   RocksDbClient 是对 rocksdb::DB 的轻量包装，提供 Open/Close 与 Put/Get/Delete。
 *   业务侧通过 PutProto/GetProto 将 protobuf Message 序列化为 value 字节串。
 *
 *   DemoKvStore::InitFromConfig()
 *     -> 读取 config/rocksdb.cnf
 *     -> RocksDbClient::Open(db_path, create_if_missing)
 *     -> Save/Load 使用 PutProto/GetProto
 *
 * =============================================================================
 * 设计要点
 * =============================================================================
 *
 * - 单 DB 实例：进程内一个路径对应一个 RocksDbClient（Demo 用单例）。
 * - Open/Close 由 mu_ 保护；RocksDB 本身支持多线程并发读写。
 * - PutProto/GetProto 使用 SerializeToString / ParseFromString，禁止 POD memcpy。
 * - Get 不存在 key 时返回 false（非错误）；真正的 DB 错误打日志后返回 false。
 */

#include <google/protobuf/message.h>

#include <mutex>
#include <string>

namespace rocksdb {
class DB;
}

class RocksDbClient {
public:
    RocksDbClient() = default;
    ~RocksDbClient();

    RocksDbClient(const RocksDbClient &) = delete;
    RocksDbClient &operator=(const RocksDbClient &) = delete;

    bool Open(const std::string &db_path, bool create_if_missing = true);
    void Close();

    bool Available() const { return db_ != nullptr; }
    const std::string &db_path() const { return db_path_; }

    bool Put(const std::string &key, const std::string &value);
    bool Get(const std::string &key, std::string *value);
    bool Delete(const std::string &key);

    bool PutProto(const std::string &key, const google::protobuf::Message &msg);
    bool GetProto(const std::string &key, google::protobuf::Message *msg);

private:
    mutable std::mutex mu_;
    rocksdb::DB *db_ = nullptr;
    std::string db_path_;
};
