#include "ProcSelfStats.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

std::string ProcPath(pid_t pid, const char *suffix) {
    char buf[64];
    if (pid == 0)
        std::snprintf(buf, sizeof(buf), "/proc/self/%s", suffix);
    else
        std::snprintf(buf, sizeof(buf), "/proc/%d/%s", static_cast<int>(pid), suffix);
    return std::string(buf);
}

bool ParseStatusKb(const std::string &line, const char *key, int64_t *out_kb) {
    const size_t n = std::strlen(key);
    if (line.size() < n || line.compare(0, n, key) != 0)
        return false;
    size_t i = n;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    char *end = nullptr;
    const long long v = std::strtoll(line.c_str() + i, &end, 10);
    if (end == line.c_str() + i)
        return false;
    *out_kb = static_cast<int64_t>(v);
    return true;
}

bool ParseStatusInt(const std::string &line, const char *key, int *out) {
    const size_t n = std::strlen(key);
    if (line.size() < n || line.compare(0, n, key) != 0)
        return false;
    size_t i = n;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    char *end = nullptr;
    const long v = std::strtol(line.c_str() + i, &end, 10);
    if (end == line.c_str() + i)
        return false;
    *out = static_cast<int>(v);
    return true;
}

int CountOpenFds(pid_t pid) {
    DIR *dir = ::opendir(ProcPath(pid, "fd").c_str());
    if (!dir)
        return -1;
    int n = 0;
    while (struct dirent *ent = ::readdir(dir)) {
        if (ent->d_name[0] == '.')
            continue;
        char *end = nullptr;
        (void)std::strtol(ent->d_name, &end, 10);
        if (end != ent->d_name && *end == '\0')
            ++n;
    }
    ::closedir(dir);
    return n;
}

bool ParseSmapsRollupLine(const std::string &line, const char *key, int64_t *out_kb) {
    const size_t kn = std::strlen(key);
    if (line.size() < kn || line.compare(0, kn, key) != 0)
        return false;
    size_t i = kn;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    char *end = nullptr;
    const long long v = std::strtoll(line.c_str() + i, &end, 10);
    if (end == line.c_str() + i)
        return false;
    *out_kb = static_cast<int64_t>(v);
    return true;
}

void FillSmapsRollup(ProcResourceSnapshot *out) {
    std::ifstream in(ProcPath(out->pid, "smaps_rollup").c_str());
    if (!in)
        return;
    std::string line;
    int64_t rss = -1, pss = -1;
    while (std::getline(in, line)) {
        if (rss < 0)
            ParseSmapsRollupLine(line, "Rss:", &rss);
        if (pss < 0)
            ParseSmapsRollupLine(line, "Pss:", &pss);
    }
    if (rss >= 0 || pss >= 0) {
        out->has_smaps_rollup = true;
        out->smaps_rss_kb = rss;
        out->smaps_pss_kb = pss;
    }
}

void FillMapsSummary(ProcResourceSnapshot *out) {
    std::ifstream in(ProcPath(out->pid, "maps").c_str());
    if (!in)
        return;
    std::string line;
    int entries = 0;
    int64_t vsum_bytes = 0;
    while (std::getline(in, line)) {
        ++entries;
        unsigned long long start = 0, end = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx", &start, &end) == 2 && end > start)
            vsum_bytes += static_cast<int64_t>(end - start);
    }
    out->map_entries = entries;
    out->maps_vsize_total_kb = vsum_bytes / 1024;
}

void SumSocketQueues(pid_t pid, uint64_t *send_sum, uint64_t *recv_sum) {
    *send_sum = 0;
    *recv_sum = 0;
    const std::string base = ProcPath(pid, "fd");
    DIR *dir = ::opendir(base.c_str());
    if (!dir)
        return;
    while (struct dirent *ent = ::readdir(dir)) {
        if (ent->d_name[0] == '.')
            continue;
        char *endptr = nullptr;
        (void)std::strtol(ent->d_name, &endptr, 10);
        if (endptr == ent->d_name || *endptr != '\0')
            continue;
        const std::string path = base + "/" + ent->d_name;
        const int dupfd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (dupfd < 0)
            continue;
        int q = 0;
        if (::ioctl(dupfd, SIOCOUTQ, &q) == 0 && q > 0)
            *send_sum += static_cast<unsigned>(q);
        int rq = 0;
        if (::ioctl(dupfd, SIOCINQ, &rq) == 0 && rq > 0)
            *recv_sum += static_cast<unsigned>(rq);
        ::close(dupfd);
    }
    ::closedir(dir);
}

}  // namespace

bool FillProcResourceSnapshot(ProcResourceSnapshot *out, pid_t pid) {
    if (!out)
        return false;
    *out = ProcResourceSnapshot{};
    out->pid = (pid == 0) ? ::getpid() : pid;

    std::ifstream in(ProcPath(pid, "status").c_str());
    if (!in)
        return false;

    std::string line;
    while (std::getline(in, line)) {
        if (out->vm_peak_kb < 0)
            ParseStatusKb(line, "VmPeak:", &out->vm_peak_kb);
        if (out->vm_size_kb < 0)
            ParseStatusKb(line, "VmSize:", &out->vm_size_kb);
        if (out->vm_hwm_kb < 0)
            ParseStatusKb(line, "VmHWM:", &out->vm_hwm_kb);
        if (out->vm_rss_kb < 0)
            ParseStatusKb(line, "VmRSS:", &out->vm_rss_kb);
        if (out->threads < 0)
            ParseStatusInt(line, "Threads:", &out->threads);
    }
    out->read_status_ok = (out->vm_rss_kb >= 0 || out->vm_size_kb >= 0);
    out->open_fds = CountOpenFds(pid);
    FillSmapsRollup(out);
    FillMapsSummary(out);
    SumSocketQueues(pid, &out->tcp_send_queue_bytes, &out->tcp_recv_queue_bytes);
    return out->read_status_ok;
}

std::string ProcResourceSnapshotFormatLine(const ProcResourceSnapshot &s) {
    std::ostringstream os;
    os << "pid=" << static_cast<int>(s.pid) << " VmRSS=" << s.vm_rss_kb << "kB VmSize=" << s.vm_size_kb
       << "kB VmHWM=" << s.vm_hwm_kb << "kB threads=" << s.threads << " fds=" << s.open_fds
       << " maps=" << s.map_entries << " maps_vsz=" << s.maps_vsize_total_kb
       << "kB tcp_send_q=" << s.tcp_send_queue_bytes << "B tcp_recv_q=" << s.tcp_recv_queue_bytes << "B";
    if (s.has_smaps_rollup)
        os << " smaps_Rss=" << s.smaps_rss_kb << "kB Pss=" << s.smaps_pss_kb << "kB";
    return os.str();
}
