#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>

// 对齐 top(RES/VIRT)、/proc/pid/status；fd 数来自 /proc/pid/fd。
// TCP 收发队列：对本进程各 fd 做 ioctl(SIOCOUTQ/SIOCINQ)，近似 ss/netstat 中的 Send-Q/Recv-Q。

struct ProcResourceSnapshot {
    pid_t pid = 0;
    int64_t vm_rss_kb = -1;
    int64_t vm_hwm_kb = -1;
    int64_t vm_size_kb = -1;
    int64_t vm_peak_kb = -1;
    int threads = -1;
    int open_fds = -1;
    bool has_smaps_rollup = false;
    int64_t smaps_rss_kb = -1;
    int64_t smaps_pss_kb = -1;
    int map_entries = -1;
    int64_t maps_vsize_total_kb = -1;
    bool read_status_ok = false;
    uint64_t tcp_send_queue_bytes = 0;
    uint64_t tcp_recv_queue_bytes = 0;
};

bool FillProcResourceSnapshot(ProcResourceSnapshot *out, pid_t pid = 0);
std::string ProcResourceSnapshotFormatLine(const ProcResourceSnapshot &s);
