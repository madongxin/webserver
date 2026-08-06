#pragma once

#include "WebServerPaths.h"

#include <string>

namespace BrpcConfigPath {

inline const std::string &Cnf() {
    static std::string path = "../config/brpc.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (WebServerPaths::ResolveProjectSubdir("config/brpc.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

}  // namespace BrpcConfigPath
