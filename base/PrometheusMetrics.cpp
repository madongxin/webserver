#include "PrometheusMetrics.h"

#include "OpsMetrics.h"
#include "ProcessMetricsSnapshot.h"

#include <cstdlib>
#include <sstream>

namespace {

void EmitGauge(std::ostringstream &os, const char *name, const char *help, double v) {
    os << "# HELP " << name << " " << help << "\n"
       << "# TYPE " << name << " gauge\n"
       << name << " " << v << "\n\n";
}

void EmitCounter(std::ostringstream &os, const char *name, const char *help, double v) {
    os << "# HELP " << name << " " << help << "\n"
       << "# TYPE " << name << " counter\n"
       << name << " " << v << "\n\n";
}

}  // namespace

std::string BuildPrometheusMetricsText() {
    ProcessMetricsSnapshot m;
    if (!FillProcessMetricsSnapshot(&m))
        return "# error collecting metrics\n";

    std::ostringstream os;
    // Prometheus client_golang process collector 同名指标
    EmitCounter(os, "process_cpu_seconds_total",
                "Total user and system CPU time spent in seconds.", m.cpu_seconds_total);
    if (m.rss_bytes >= 0)
        EmitGauge(os, "process_resident_memory_bytes", "Resident memory size in bytes.",
                  static_cast<double>(m.rss_bytes));
    if (m.vm_size_bytes >= 0)
        EmitGauge(os, "process_virtual_memory_bytes", "Virtual memory size in bytes.",
                  static_cast<double>(m.vm_size_bytes));
    if (m.vm_max_bytes >= 0)
        EmitGauge(os, "process_virtual_memory_max_bytes",
                  "Maximum amount of virtual memory available in bytes.",
                  static_cast<double>(m.vm_max_bytes));
    if (m.open_fds >= 0)
        EmitGauge(os, "process_open_fds", "Number of open file descriptors.", m.open_fds);
    if (m.max_fds >= 0)
        EmitGauge(os, "process_max_fds", "Maximum number of open file descriptors.", m.max_fds);
    if (m.threads >= 0)
        EmitGauge(os, "process_threads", "Number of OS threads in the process.", m.threads);
    if (m.start_time_seconds > 0)
        EmitGauge(os, "process_start_time_seconds",
                  "Start time of the process since unix epoch in seconds.", m.start_time_seconds);
    if (m.io_read_bytes >= 0)
        EmitCounter(os, "process_io_read_bytes_total",
                    "Number of bytes read by the process from storage.",
                    static_cast<double>(m.io_read_bytes));
    if (m.io_write_bytes >= 0)
        EmitCounter(os, "process_io_write_bytes_total",
                    "Number of bytes written by the process to storage.",
                    static_cast<double>(m.io_write_bytes));
    EmitGauge(os, "process_socket_send_queue_bytes",
              "Sum of socket send-queue bytes (SIOCOUTQ) for this process.",
              static_cast<double>(m.tcp_send_queue_bytes));
    EmitGauge(os, "process_socket_recv_queue_bytes",
              "Sum of socket recv-queue bytes (SIOCINQ) for this process.",
              static_cast<double>(m.tcp_recv_queue_bytes));

    // 兼容旧名（监控页不再使用）
    EmitGauge(os, "gamemesh_eventloop_tick_seconds", "Last event loop iteration duration.",
              m.eventloop_tick_sec);
    EmitGauge(os, "gamemesh_eventloop_tick_peak_seconds", "Peak event loop iteration duration.",
              m.eventloop_tick_peak_sec);
    EmitGauge(os, "gamemesh_logic_handle_seconds", "Last GameLogic::Handle duration.",
              m.logic_handle_sec);
    EmitGauge(os, "gamemesh_logic_handle_peak_seconds", "Peak GameLogic::Handle duration.",
              m.logic_handle_peak_sec);

    if (!m.thread_states_json.empty() && m.thread_states_json != "{}") {
        os << "# HELP gamemesh_os_threads Threads by /proc state letter.\n"
              "# TYPE gamemesh_os_threads gauge\n";
        for (std::size_t i = 0; i < m.thread_states_json.size(); ++i) {
            if (m.thread_states_json[i] == '"') {
                const char state = m.thread_states_json[i + 1];
                std::size_t colon = m.thread_states_json.find(':', i);
                std::size_t end = m.thread_states_json.find_first_of(",}", colon);
                if (colon != std::string::npos && end != std::string::npos) {
                    const int cnt = std::atoi(m.thread_states_json.c_str() + colon + 1);
                    os << "gamemesh_os_threads{state=\"" << state << "\"} " << cnt << "\n";
                }
            }
        }
        os << "\n";
    }

    os << OpsMetrics::Instance().PrometheusText();
    return os.str();
}
