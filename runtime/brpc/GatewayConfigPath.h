#pragma once

#include "GameMeshPaths.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace GatewayConfigPath {

inline std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

inline void SplitCsv(const std::string &val, std::vector<std::string> *out) {
    out->clear();
    std::string cur;
    for (char c : val) {
        if (c == ',') {
            cur = Trim(cur);
            if (!cur.empty())
                out->push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    cur = Trim(cur);
    if (!cur.empty())
        out->push_back(cur);
}

inline const std::string &Cnf() {
    static std::string path = "../config/gateway.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/gateway.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

/** 读单个键；不存在返回空串 */
inline std::string ReadValue(const char *key) {
    std::ifstream in(Cnf());
    if (!in || !key)
        return {};
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        if (Trim(line.substr(0, eq)) == key)
            return Trim(line.substr(eq + 1));
    }
    return {};
}

/** logic_addrs / logic_instance_ids / world_addrs / rpc_timeout_ms */
inline bool Load(std::vector<std::string> *logic_addrs, std::vector<std::string> *logic_instance_ids,
                 std::vector<std::string> *world_addrs, int *timeout_ms) {
    logic_addrs->clear();
    if (logic_instance_ids)
        logic_instance_ids->clear();
    if (world_addrs)
        world_addrs->clear();
    *timeout_ms = 3000;
    std::ifstream in(Cnf());
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
        if (key == "logic_addrs") {
            SplitCsv(val, logic_addrs);
        } else if (key == "logic_instance_ids" && logic_instance_ids) {
            SplitCsv(val, logic_instance_ids);
        } else if (key == "world_addrs" && world_addrs) {
            SplitCsv(val, world_addrs);
        } else if (key == "rpc_timeout_ms" || key == "world_rpc_timeout_ms") {
            *timeout_ms = std::atoi(val.c_str());
        }
    }
    return !logic_addrs->empty();
}

inline bool Load(std::vector<std::string> *logic_addrs, std::vector<std::string> *logic_instance_ids,
                 int *timeout_ms) {
    return Load(logic_addrs, logic_instance_ids, nullptr, timeout_ms);
}

/** 兼容旧调用：忽略 instance_ids */
inline bool Load(std::vector<std::string> *logic_addrs, int *timeout_ms) {
    return Load(logic_addrs, nullptr, nullptr, timeout_ms);
}

}  // namespace GatewayConfigPath
