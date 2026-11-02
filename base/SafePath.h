#pragma once

#include <cctype>
#include <string>

/** 用户提供的相对路径。拒绝绝对路径、盘符和任何包含 ".." 的段。 */
inline bool SafeJoinUnderRoot(const std::string &root, const std::string &user_path,
                              std::string *out) {
    if (root.empty() || user_path.empty())
        return false;
    if (user_path[0] == '/' || user_path[0] == '\\')
        return false;
    if (user_path.find(':') != std::string::npos)
        return false;

    std::string rel;
    for (size_t i = 0; i < user_path.size();) {
        size_t j = i;
        while (j < user_path.size() && user_path[j] != '/' && user_path[j] != '\\')
            ++j;
        const std::string seg = user_path.substr(i, j - i);
        if (seg.empty() || seg == ".") {
            i = j < user_path.size() ? j + 1 : j;
            continue;
        }
        if (seg.find("..") != std::string::npos)
            return false;
        for (unsigned char c : seg) {
            if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-'))
                return false;
        }
        if (!rel.empty())
            rel.push_back('/');
        rel += seg;
        i = j < user_path.size() ? j + 1 : j;
    }
    if (rel.empty())
        return false;
    if (out)
        *out = root + "/" + rel;
    return true;
}

/** multipart 文件名只留最后一段，并套用同一套白名单。 */
inline bool SafeBasename(const std::string &name, std::string *out) {
    if (name.empty())
        return false;
    const size_t slash = name.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    return SafeJoinUnderRoot(".", base, out) && out && out->size() > 2;
}
