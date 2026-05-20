USE metrics;

CREATE TABLE IF NOT EXISTS player_item (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  player_id BIGINT NOT NULL,
  item_id BIGINT NOT NULL,
  count INT NOT NULL DEFAULT 1,
  create_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  expire_time DATETIME NULL,
  extra_data JSON,
  KEY idx_player_item (player_id, item_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
