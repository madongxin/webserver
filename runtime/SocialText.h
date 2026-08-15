#pragma once

#include "Utf8Text.h"

#include <string>

/** 公开资料名 / 世界聊天文本校验。不含 Redis/MySQL。 */
namespace social {

inline std::string TrimCopy(const std::string &s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

inline bool NormalizePlayerName(const std::string &raw, std::string *out, std::string *err_code) {
    const std::string n = TrimCopy(raw);
    std::string tec;
    if (!utf8text::ValidBoundedText(n, 1, 64, false, &tec)) {
        if (err_code)
            *err_code = tec.empty() ? "ERR_TEXT_LENGTH" : tec;
        return false;
    }
    if (out)
        *out = n;
    return true;
}

inline bool ValidWorldChannel(const std::string &channel) {
    return channel.empty() || channel == "world";
}

/** 世界聊天：1–max_cp 码点，且 UTF-8 字节 ≤ max_bytes；不允许控制符。 */
inline bool ValidWorldChatText(const std::string &text, size_t max_cp, size_t max_bytes,
                               std::string *err_code) {
    if (text.size() > max_bytes) {
        if (err_code)
            *err_code = "ERR_TEXT_LENGTH";
        return false;
    }
    std::string tec;
    if (!utf8text::ValidBoundedText(text, 1, max_cp, false, &tec)) {
        if (err_code)
            *err_code = tec.empty() ? "ERR_TEXT_LENGTH" : tec;
        return false;
    }
    return true;
}

}  // namespace social
