#include "PrometheusMetrics.h"

#include "ProcessMetricsSnapshot.h"

#include <sstream>

std::string BuildPrometheusMetricsText() {
    ProcessMetricsSnapshot m;
    if (!FillProcessMetricsSnapshot(&m))
        return "# error collecting metrics\n";

    std::ostringstream os;
    os << "# HELP process_cpu_seconds_total Total user+system CPU time spent in seconds.\n"
       << "# TYPE process_cpu_seconds_total counter\n"
       << "process_cpu_seconds_total " << m.cpu_seconds_total << "\n\n";

    os << "# HELP process_resident_memory_bytes Resident memory size in bytes.\n"
       << "# TYPE process_resident_memory_bytes gauge\n"
       << "process_resident_memory_bytes " << m.rss_bytes << "\n\n";

    os << "# HELP process_open_fds Number of open file descriptors.\n"
       << "# TYPE process_open_fds gauge\n"
       << "process_open_fds " << m.open_fds << "\n\n";

    os << "# HELP webserver_eventloop_tick_seconds Last event loop iteration duration.\n"
       << "# TYPE webserver_eventloop_tick_seconds gauge\n"
       << "webserver_eventloop_tick_seconds " << m.eventloop_tick_sec << "\n\n";

    os << "# HELP webserver_eventloop_tick_peak_seconds Peak event loop iteration duration.\n"
       << "# TYPE webserver_eventloop_tick_peak_seconds gauge\n"
       << "webserver_eventloop_tick_peak_seconds " << m.eventloop_tick_peak_sec << "\n\n";

    os << "# HELP webserver_logic_handle_seconds Last GameLogic::Handle duration.\n"
       << "# TYPE webserver_logic_handle_seconds gauge\n"
       << "webserver_logic_handle_seconds " << m.logic_handle_sec << "\n\n";

    os << "# HELP webserver_logic_handle_peak_seconds Peak GameLogic::Handle duration.\n"
       << "# TYPE webserver_logic_handle_peak_seconds gauge\n"
       << "webserver_logic_handle_peak_seconds " << m.logic_handle_peak_sec << "\n\n";

    os << "# HELP process_threads OS thread count.\n"
       << "# TYPE process_threads gauge\n"
       << "process_threads " << m.threads << "\n\n";

    if (!m.thread_states_json.empty() && m.thread_states_json != "{}") {
        os << "# HELP webserver_os_threads Threads by /proc state letter.\n"
           << "# TYPE webserver_os_threads gauge\n";
        for (std::size_t i = 0; i < m.thread_states_json.size(); ++i) {
            if (m.thread_states_json[i] == '"') {
                const char state = m.thread_states_json[i + 1];
                std::size_t colon = m.thread_states_json.find(':', i);
                std::size_t end = m.thread_states_json.find_first_of(",}", colon);
                if (colon != std::string::npos && end != std::string::npos) {
                    const int cnt = std::atoi(m.thread_states_json.c_str() + colon + 1);
                    os << "webserver_os_threads{state=\"" << state << "\"} " << cnt << "\n";
                }
            }
        }
        os << "\n";
    }

    return os.str();
}
