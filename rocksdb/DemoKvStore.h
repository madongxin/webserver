#pragma once

/**
 * @file DemoKvStore.h
 * @brief RocksDB Demo：用 protobuf PlayerSnapshot 做本地 KV 读写示例
 *
 * key 约定：demo:player:{player_id}
 * 与 MySQL 邮件/道具、Redis 会话无关，仅演示基础设施用法。
 */

#include "kv_demo.pb.h"

#include <cstdint>
#include <string>

class DemoKvStore {
public:
    static DemoKvStore &Instance();

    /** 读 config/rocksdb.cnf 并 Open RocksDB；失败返回 false */
    bool InitFromConfig();

    /** 使用指定路径打开（测试用，不读配置文件） */
    bool InitWithPath(const std::string &db_path, bool create_if_missing = true);

    bool Available() const { return available_; }

    bool Save(const kvdemo::PlayerSnapshot &snap);
    bool Load(uint64_t player_id, kvdemo::PlayerSnapshot *out);
    bool Remove(uint64_t player_id);

private:
    DemoKvStore() = default;
    std::string SnapshotKey(uint64_t player_id) const;

    bool available_ = false;
};
