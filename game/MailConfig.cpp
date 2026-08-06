#include "MailConfig.h"

#include "Logging.h"
#include "MailConfigPath.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

}  // namespace

MailConfig &MailConfig::Instance() {
    static MailConfig g;
    return g;
}

bool MailConfig::LoadFromConfig() {
    const std::string &path = MailConfigPath::MailCnf();
    std::ifstream in(path);
    if (!in) {
        LOG_WARN << "MailConfig: cannot read " << path << ", using defaults";
        return false;
    }
    MailConfigValues v;
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
        if (key == "mailbox_capacity")
            v.mailbox_capacity = std::atoi(val.c_str());
        else if (key == "mailbox_overflow")
            v.mailbox_overflow = std::atoi(val.c_str());
        else if (key == "default_expire_days")
            v.default_expire_days = std::atoi(val.c_str());
        else if (key == "page_size_default")
            v.page_size_default = std::atoi(val.c_str());
        else if (key == "batch_op_max")
            v.batch_op_max = std::atoi(val.c_str());
        else if (key == "favorite_max")
            v.favorite_max = std::atoi(val.c_str());
        else if (key == "protect_hours")
            v.protect_hours = std::atoi(val.c_str());
        else if (key == "expire_scan_interval_sec")
            v.expire_scan_interval_sec = std::atof(val.c_str());
    }
    if (v.mailbox_capacity <= 0)
        v.mailbox_capacity = 100;
    if (v.mailbox_overflow < v.mailbox_capacity)
        v.mailbox_overflow = v.mailbox_capacity;
    if (v.page_size_default <= 0)
        v.page_size_default = 20;
    if (v.batch_op_max <= 0)
        v.batch_op_max = 20;
    if (v.favorite_max <= 0)
        v.favorite_max = 50;
    if (v.expire_scan_interval_sec <= 0.0)
        v.expire_scan_interval_sec = 60.0;
    values_ = v;
    LOG_INFO << "MailConfig: capacity=" << values_.mailbox_capacity
             << " overflow=" << values_.mailbox_overflow
             << " expire_days=" << values_.default_expire_days;
    return true;
}
