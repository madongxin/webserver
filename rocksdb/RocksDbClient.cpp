/**
 * @file RocksDbClient.cpp
 * @brief RocksDB 客户端：Open/Close、字节串 KV、protobuf Put/Get
 */

#include "RocksDbClient.h"

#include "Logging.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>

RocksDbClient::~RocksDbClient() {
    Close();
}

bool RocksDbClient::Open(const std::string &db_path, bool create_if_missing) {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) {
        LOG_WARN << "RocksDbClient::Open: already open at " << db_path_;
        return true;
    }

    rocksdb::Options options;
    options.create_if_missing = create_if_missing;
    // 单机 Demo：适度并行压缩即可；后续可按业务调优
    options.IncreaseParallelism();
    options.OptimizeLevelStyleCompaction();

    rocksdb::DB *db = nullptr;
    rocksdb::Status s = rocksdb::DB::Open(options, db_path, &db);
    if (!s.ok()) {
        LOG_ERROR << "RocksDbClient::Open failed path=" << db_path << " err=" << s.ToString();
        return false;
    }
    db_ = db;
    db_path_ = db_path;
    LOG_INFO << "RocksDbClient opened: " << db_path_;
    return true;
}

void RocksDbClient::Close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_)
        return;
    delete db_;
    db_ = nullptr;
    LOG_INFO << "RocksDbClient closed: " << db_path_;
    db_path_.clear();
}

bool RocksDbClient::Put(const std::string &key, const std::string &value) {
    if (!db_)
        return false;
    rocksdb::Status s = db_->Put(rocksdb::WriteOptions(), key, value);
    if (!s.ok()) {
        LOG_ERROR << "RocksDbClient::Put key=" << key << " err=" << s.ToString();
        return false;
    }
    return true;
}

bool RocksDbClient::Get(const std::string &key, std::string *value) {
    if (!db_ || !value)
        return false;
    rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), key, value);
    if (s.IsNotFound())
        return false;
    if (!s.ok()) {
        LOG_ERROR << "RocksDbClient::Get key=" << key << " err=" << s.ToString();
        return false;
    }
    return true;
}

bool RocksDbClient::Delete(const std::string &key) {
    if (!db_)
        return false;
    rocksdb::Status s = db_->Delete(rocksdb::WriteOptions(), key);
    if (!s.ok()) {
        LOG_ERROR << "RocksDbClient::Delete key=" << key << " err=" << s.ToString();
        return false;
    }
    return true;
}

bool RocksDbClient::PutProto(const std::string &key, const google::protobuf::Message &msg) {
    std::string bytes;
    if (!msg.SerializeToString(&bytes)) {
        LOG_ERROR << "RocksDbClient::PutProto serialize failed key=" << key;
        return false;
    }
    return Put(key, bytes);
}

bool RocksDbClient::GetProto(const std::string &key, google::protobuf::Message *msg) {
    if (!msg)
        return false;
    std::string bytes;
    if (!Get(key, &bytes))
        return false;
    if (!msg->ParseFromString(bytes)) {
        LOG_ERROR << "RocksDbClient::GetProto parse failed key=" << key;
        return false;
    }
    return true;
}
