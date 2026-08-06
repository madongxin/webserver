#include "ProcessMetricsSnapshot.h"

#include "EventLoopMetrics.h"
#include "LogicMetrics.h"
#include "ProcSelfStats.h"

#include <ctime>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>
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
    (void)ReadCpuSeconds(&out->cpu_seconds_total);
    out->eventloop_tick_sec = EventLoopMetrics::LastTickSeconds();
    out->eventloop_tick_peak_sec = EventLoopMetrics::PeakTickSeconds();
    out->logic_handle_sec = LogicMetrics::LastHandleSeconds();
    out->logic_handle_peak_sec = LogicMetrics::PeakHandleSeconds();
    out->thread_states_json = CollectThreadStatesJson();
    return true;
}
