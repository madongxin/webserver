-- 玩家最后安全位置（仅 GameDB 写 MySQL）
CREATE TABLE IF NOT EXISTS player_last_safe_position (
  player_id BIGINT NOT NULL PRIMARY KEY,
  realm_id INT NOT NULL DEFAULT 1,
  map_template_id BIGINT NOT NULL,
  last_safe_x FLOAT NOT NULL,
  last_safe_y FLOAT NOT NULL,
  last_safe_z FLOAT NOT NULL,
  last_safe_yaw FLOAT NOT NULL,
  position_version BIGINT NOT NULL DEFAULT 1,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
