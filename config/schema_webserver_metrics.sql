-- 监控落库表（MetricsDbWriter 也会自动 CREATE TABLE IF NOT EXISTS）
USE metrics;

CREATE TABLE IF NOT EXISTS webserver_metrics (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  ts_unix BIGINT NOT NULL,
  cpu_seconds_total DOUBLE,
  rss_bytes BIGINT,
  vm_size_bytes BIGINT,
  open_fds INT,
  process_threads INT,
  eventloop_tick_sec DOUBLE,
  eventloop_tick_peak_sec DOUBLE,
  thread_states_json TEXT,
  tcp_send_queue_bytes BIGINT,
  tcp_recv_queue_bytes BIGINT,
  KEY idx_ts (ts_unix)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
