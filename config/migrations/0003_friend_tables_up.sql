-- 好友关系 / 申请 / 黑名单 / 幂等（GameDB 权威）
CREATE TABLE IF NOT EXISTS friend_relation (
  player_id          BIGINT UNSIGNED NOT NULL,
  friend_player_id   BIGINT UNSIGNED NOT NULL,
  remark             VARCHAR(64) NOT NULL DEFAULT '',
  source             TINYINT UNSIGNED NOT NULL DEFAULT 0,
  created_at         BIGINT UNSIGNED NOT NULL,
  updated_at         BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (player_id, friend_player_id),
  KEY idx_friend_player (friend_player_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS friend_request (
  request_id         BIGINT UNSIGNED NOT NULL,
  from_player_id     BIGINT UNSIGNED NOT NULL,
  to_player_id       BIGINT UNSIGNED NOT NULL,
  status             TINYINT UNSIGNED NOT NULL,
  created_at         BIGINT UNSIGNED NOT NULL,
  updated_at         BIGINT UNSIGNED NOT NULL,
  expire_at          BIGINT UNSIGNED NOT NULL,
  idempotency_key    VARCHAR(96) NOT NULL DEFAULT '',
  PRIMARY KEY (request_id),
  KEY idx_to_status_time (to_player_id, status, created_at),
  KEY idx_from_to_status (from_player_id, to_player_id, status),
  UNIQUE KEY uk_idempotency (idempotency_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS friend_block (
  player_id          BIGINT UNSIGNED NOT NULL,
  blocked_player_id  BIGINT UNSIGNED NOT NULL,
  created_at         BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (player_id, blocked_player_id),
  KEY idx_blocked_player (blocked_player_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS friend_op_idempotency (
  actor_player_id    BIGINT UNSIGNED NOT NULL,
  idempotency_key    VARCHAR(96) NOT NULL,
  op                 VARCHAR(32) NOT NULL,
  error_code         VARCHAR(64) NOT NULL,
  request_id         BIGINT UNSIGNED NOT NULL DEFAULT 0,
  peer_player_id     BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at         BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (actor_player_id, idempotency_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
