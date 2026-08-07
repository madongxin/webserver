-- 创建监控库（表由 webserver 在 USE_MYSQL=1 启动时自动 CREATE）
CREATE DATABASE IF NOT EXISTS metrics DEFAULT CHARACTER SET utf8mb4;
USE metrics;

-- 验证是否有数据写入（需 webserver 正在运行且已编译 MySQL 模块）:
-- SELECT COUNT(*), FROM_UNIXTIME(MAX(ts_unix)) AS last_write FROM gamemesh_metrics;
-- SELECT * FROM gamemesh_metrics ORDER BY id DESC LIMIT 10;
