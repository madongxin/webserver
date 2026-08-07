#pragma once

#include "GameMeshPaths.h"

#include <string>

namespace MailConfigPath {

inline const std::string &MailCnf() {
    static std::string path = "../config/mail.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/mail.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

}  // namespace MailConfigPath
