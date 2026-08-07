-- GameDB outbox（阶段 4）
-- 库名与 config/mysql.cnf 中 dbname 一致（默认 metrics）
USE metrics;

CREATE TABLE IF NOT EXISTS gamedb_outbox (
  id               BIGINT AUTO_INCREMENT PRIMARY KEY,
  event_type       VARCHAR(64)  NOT NULL,
  aggregate_type   VARCHAR(64)  NOT NULL,
  aggregate_id     VARCHAR(128) NOT NULL,
  idempotency_key  VARCHAR(128) NOT NULL,
  payload          TEXT         NOT NULL,
  created_at       BIGINT       NOT NULL,
  published_at     BIGINT       NULL,
  UNIQUE KEY uk_gamedb_outbox_idem (idempotency_key),
  KEY idx_gamedb_outbox_unpub (published_at, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
