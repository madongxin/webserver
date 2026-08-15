-- player_profile: 服务器权威玩家展示/战斗属性
CREATE TABLE IF NOT EXISTS player_profile (
  player_id BIGINT NOT NULL PRIMARY KEY,
  player_name VARCHAR(64) NOT NULL DEFAULT 'player',
  hp INT NOT NULL,
  max_hp INT NOT NULL,
  mp INT NOT NULL,
  max_mp INT NOT NULL,
  attack INT NOT NULL,
  spell_power INT NOT NULL,
  defense INT NOT NULL,
  magic_resistance INT NOT NULL,
  crit_chance FLOAT NOT NULL,
  crit_damage FLOAT NOT NULL,
  move_speed FLOAT NOT NULL,
  attack_speed FLOAT NOT NULL,
  stats_version BIGINT NOT NULL DEFAULT 1,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  KEY idx_player_profile_name (player_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
