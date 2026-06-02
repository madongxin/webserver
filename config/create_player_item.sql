-- 玩家道具实例表（每次发放/获得一条记录，非背包聚合表）
-- 库名与 config/mysql.cnf 中 dbname 一致（默认 metrics）
USE metrics;

CREATE TABLE IF NOT EXISTS player_item (
  id              BIGINT AUTO_INCREMENT PRIMARY KEY COMMENT '唯一实例ID',
  player_id       BIGINT NOT NULL                COMMENT '玩家ID',
  item_id         BIGINT NOT NULL                COMMENT '道具配置ID',
  count           INT NOT NULL DEFAULT 1         COMMENT '本次获得数量',
  create_time     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '获得时间',
  expire_time     DATETIME NULL                  COMMENT '过期时间，NULL 表示不过期',
  extra_data      JSON                           COMMENT '强化/洗练/附魔等扩展 JSON',
  KEY idx_player_item (player_id, item_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 验证：SELECT * FROM player_item ORDER BY id DESC LIMIT 10;
