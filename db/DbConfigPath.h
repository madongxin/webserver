#pragma once

#include "WebServerPaths.h"

#include <string>

namespace DbConfigPath {

inline const std::string &MysqlCnf() {
    static std::string path = "../config/mysql.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (WebServerPaths::ResolveProjectSubdir("config/mysql.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

}  // namespace DbConfigPath
