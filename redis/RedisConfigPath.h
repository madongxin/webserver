#pragma once

#include "WebServerPaths.h"

#include <string>

namespace RedisConfigPath {

inline const std::string &RedisCnf() {
    static std::string path = "../config/redis.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (WebServerPaths::ResolveProjectSubdir("config/redis.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

}  // namespace RedisConfigPath
