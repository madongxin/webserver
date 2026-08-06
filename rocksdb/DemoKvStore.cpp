/**
 * @file DemoKvStore.cpp
 * @brief DemoKvStore：配置加载与 PlayerSnapshot 的 Save/Load/Remove
 */

#include "DemoKvStore.h"

#include "Logging.h"
#include "RocksDbClient.h"
#include "RocksDbConfigPath.h"
#include "WebServerPaths.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool ParseConfig(const std::string &path, std::string *db_path, bool *create_if_missing) {
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (key == "db_path")
            *db_path = val;
        else if (key == "create_if_missing")
            *create_if_missing = (val == "1" || val == "true" || val == "TRUE");
    }
    return !db_path->empty();
}

/** 相对路径落到项目 data/rocksdb；目录可不存在（Open 前会 mkdir） */
std::string ResolveDbPath(const std::string &configured) {
    if (!configured.empty() && configured[0] == '/')
        return configured;
    // ResolveProjectSubdir 依赖 realpath，目标目录不存在时会失败；先解析项目根再拼相对路径
    std::string project_root;
    if (WebServerPaths::ResolveProjectSubdir("config", &project_root)) {
        // .../config -> 项目根
        const auto slash = project_root.rfind('/');
        if (slash != std::string::npos)
            project_root.resize(slash);
        return project_root + "/data/rocksdb";
    }
    return configured;
}

bool EnsureParentDir(const std::string &db_path) {
    // RocksDB 会创建最后一级目录；确保上一级存在（如 data/）
    const auto slash = db_path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return true;
    const std::string parent = db_path.substr(0, slash);
    struct stat st {};
    if (stat(parent.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        return true;
    // 递归创建：简易两级 mkdir（项目根/data）
    std::string cur;
    for (std::size_t i = 0; i < parent.size(); ++i) {
        cur.push_back(parent[i]);
        if (parent[i] == '/' || i + 1 == parent.size()) {
            if (cur.empty() || cur == "/")
                continue;
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    }
    return true;
}

RocksDbClient &Client() {
    static RocksDbClient c;
    return c;
}

}  // namespace

DemoKvStore &DemoKvStore::Instance() {
    static DemoKvStore g;
    return g;
}

std::string DemoKvStore::SnapshotKey(uint64_t player_id) const {
    return "demo:player:" + std::to_string(player_id);
}

bool DemoKvStore::InitWithPath(const std::string &db_path, bool create_if_missing) {
    if (db_path.empty())
        return false;
    if (!EnsureParentDir(db_path)) {
        LOG_ERROR << "DemoKvStore: cannot create parent dir for " << db_path;
        return false;
    }
    if (!Client().Open(db_path, create_if_missing)) {
        available_ = false;
        return false;
    }
    available_ = true;
    LOG_INFO << "DemoKvStore ready path=" << db_path;
    return true;
}

bool DemoKvStore::InitFromConfig() {
    const std::string &cnf = RocksDbConfigPath::Cnf();
    std::string db_path;
    bool create_if_missing = true;
    if (!ParseConfig(cnf, &db_path, &create_if_missing)) {
        LOG_ERROR << "DemoKvStore: cannot parse " << cnf;
        available_ = false;
        return false;
    }
    const std::string resolved = ResolveDbPath(db_path);
    return InitWithPath(resolved, create_if_missing);
}

bool DemoKvStore::Save(const kvdemo::PlayerSnapshot &snap) {
    if (!available_ || !Client().Available())
        return false;
    if (snap.player_id() == 0) {
        LOG_ERROR << "DemoKvStore::Save: player_id == 0";
        return false;
    }
    return Client().PutProto(SnapshotKey(snap.player_id()), snap);
}

bool DemoKvStore::Load(uint64_t player_id, kvdemo::PlayerSnapshot *out) {
    if (!available_ || !Client().Available() || !out || player_id == 0)
        return false;
    return Client().GetProto(SnapshotKey(player_id), out);
}

bool DemoKvStore::Remove(uint64_t player_id) {
    if (!available_ || !Client().Available() || player_id == 0)
        return false;
    return Client().Delete(SnapshotKey(player_id));
}
