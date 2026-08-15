#pragma once

#include <cstdint>
#include <string>

/** UTF-8 校验与 Unicode 码点计数（邮件标题/正文、玩家名）。 */
namespace utf8text {

inline bool IsUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

/** 非法 UTF-8 返回 false；*code_points 为码点个数（非字节）。 */
inline bool CountCodePoints(const std::string &s, size_t *code_points) {
    if (code_points)
        *code_points = 0;
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t need = 0;
        uint32_t cp = 0;
        if (c <= 0x7F) {
            need = 1;
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            need = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + need > s.size())
            return false;
        for (size_t j = 1; j < need; ++j) {
            const unsigned char cc = static_cast<unsigned char>(s[i + j]);
            if (!IsUtf8Continuation(cc))
                return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) || (need == 4 && cp < 0x10000))
            return false;
        if (cp > 0x10FFFF)
            return false;
        ++n;
        i += need;
    }
    if (code_points)
        *code_points = n;
    return true;
}

inline bool IsC0OrDel(uint32_t cp) { return cp <= 0x1F || cp == 0x7F; }

inline bool IsC1(uint32_t cp) { return cp >= 0x80 && cp <= 0x9F; }

/** 标题/玩家名：禁止全部控制字符。 */
inline bool HasForbiddenControl(const std::string &s, bool allow_newline) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t need = 1;
        uint32_t cp = c;
        if (c <= 0x7F) {
            cp = c;
            need = 1;
        } else if ((c & 0xE0) == 0xC0) {
            need = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
            cp = c & 0x07;
        } else {
            return true;
        }
        if (i + need > s.size())
            return true;
        for (size_t j = 1; j < need; ++j)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + j]) & 0x3F);
        if (allow_newline && (cp == '\n' || cp == '\r' || cp == '\t')) {
            i += need;
            continue;
        }
        if (IsC0OrDel(cp) || IsC1(cp))
            return true;
        i += need;
    }
    return false;
}

inline bool ValidBoundedText(const std::string &s, size_t min_cp, size_t max_cp, bool allow_newline,
                             std::string *err_code) {
    size_t n = 0;
    if (!CountCodePoints(s, &n)) {
        if (err_code)
            *err_code = "ERR_INVALID_UTF8";
        return false;
    }
    if (n < min_cp || n > max_cp) {
        if (err_code)
            *err_code = "ERR_TEXT_LENGTH";
        return false;
    }
    if (HasForbiddenControl(s, allow_newline)) {
        if (err_code)
            *err_code = "ERR_TEXT_CONTROL";
        return false;
    }
    return true;
}

}  // namespace utf8text
