#include "ProcessMetricsSnapshot.h"

#include "EventLoopMetrics.h"
#include "LogicMetrics.h"
#include "ProcSelfStats.h"

#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>

namespace {

bool ReadCpuSeconds(double *cpu_out) {
    std::ifstream in("/proc/self/stat");
    if (!in)
        return false;
    std::string ignore;
    for (int i = 0; i < 13; ++i)
        in >> ignore;
    long utime = 0, stime = 0;
    in >> utime >> stime;
    const long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0)
        return false;
    *cpu_out = static_cast<double>(utime + stime) / static_cast<double>(hz);
    return true;
}

std::string CollectThreadStatesJson() {
    std::map<char, int> counts;
    DIR *dir = ::opendir("/proc/self/task");
    if (!dir)
        return "{}";
    while (dirent *ent = ::readdir(dir)) {
        if (ent->d_name[0] == '.')
            continue;
        std::string path = std::string("/proc/self/task/") + ent->d_name + "/stat";
        std::ifstream in(path);
        if (!in)
            continue;
        std::string ignore;
        char state = '?';
        for (int i = 0; i < 2; ++i)
            in >> ignore;
        in >> state;
        ++counts[state];
    }
    ::closedir(dir);
    std::ostringstream os;
    os << '{';
    bool first = true;
    for (const auto &kv : counts) {
        if (!first)
            os << ',';
        first = false;
        os << '"' << kv.first << "\":" << kv.second;
    }
    os << '}';
    return os.str();
}

int ReadMaxFds() {
    std::ifstream in("/proc/self/limits");
    if (!in)
        return -1;
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 14, "Max open files") != 0)
            continue;
        std::istringstream iss(line.substr(14));
        long soft = -1;
        iss >> soft;
        return static_cast<int>(soft);
    }
    return -1;
}

int64_t ReadVmMaxBytes() {
    rlimit rl{};
    if (::getrlimit(RLIMIT_AS, &rl) != 0)
        return -1;
    if (rl.rlim_cur == RLIM_INFINITY)
        return -1;
    return static_cast<int64_t>(rl.rlim_cur);
}

double ReadStartTimeSeconds() {
    std::ifstream statf("/proc/self/stat");
    std::string content;
    if (!statf || !std::getline(statf, content))
        return -1.0;
    const auto rparen = content.rfind(')');
    if (rparen == std::string::npos)
        return -1.0;
    std::istringstream iss(content.substr(rparen + 1));
    std::string skip;
    for (int i = 0; i < 19; ++i) {
        if (!(iss >> skip))
            return -1.0;
    }
    unsigned long long start_ticks = 0;
    if (!(iss >> start_ticks))
        return -1.0;
    const long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0)
        return -1.0;
    std::ifstream procstat("/proc/stat");
    std::string line;
    long long btime = 0;
    while (procstat && std::getline(procstat, line)) {
        if (line.compare(0, 6, "btime ") == 0) {
            btime = std::strtoll(line.c_str() + 6, nullptr, 10);
            break;
        }
    }
    if (btime <= 0)
        return -1.0;
    return static_cast<double>(btime) +
           static_cast<double>(start_ticks) / static_cast<double>(hz);
}

void ReadIoBytes(int64_t *read_bytes, int64_t *write_bytes) {
    *read_bytes = -1;
    *write_bytes = -1;
    std::ifstream in("/proc/self/io");
    if (!in)
        return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 12, "read_bytes: ") == 0)
            *read_bytes = std::strtoll(line.c_str() + 12, nullptr, 10);
        else if (line.compare(0, 13, "write_bytes: ") == 0)
            *write_bytes = std::strtoll(line.c_str() + 13, nullptr, 10);
    }
}

}  // namespace

bool FillProcessMetricsSnapshot(ProcessMetricsSnapshot *out) {
    if (!out)
        return false;
    out->time_unix = static_cast<int64_t>(std::time(nullptr));
    ProcResourceSnapshot snap;
    if (!FillProcResourceSnapshot(&snap))
        return false;
    out->rss_bytes = snap.vm_rss_kb > 0 ? snap.vm_rss_kb * 1024 : -1;
    out->vm_size_bytes = snap.vm_size_kb > 0 ? snap.vm_size_kb * 1024 : -1;
    out->open_fds = snap.open_fds;
    out->threads = snap.threads;
    out->tcp_send_queue_bytes = snap.tcp_send_queue_bytes;
    out->tcp_recv_queue_bytes = snap.tcp_recv_queue_bytes;
    out->max_fds = ReadMaxFds();
    out->vm_max_bytes = ReadVmMaxBytes();
    out->start_time_seconds = ReadStartTimeSeconds();
    ReadIoBytes(&out->io_read_bytes, &out->io_write_bytes);
    (void)ReadCpuSeconds(&out->cpu_seconds_total);
    out->eventloop_tick_sec = EventLoopMetrics::LastTickSeconds();
    out->eventloop_tick_peak_sec = EventLoopMetrics::PeakTickSeconds();
    out->logic_handle_sec = LogicMetrics::LastHandleSeconds();
    out->logic_handle_peak_sec = LogicMetrics::PeakHandleSeconds();
    out->thread_states_json = CollectThreadStatesJson();
    return true;
}
