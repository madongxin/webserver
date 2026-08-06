-- 回滚邮件表（危险：会删除全部邮件数据）
USE metrics;

DROP TABLE IF EXISTS mail_operation_log;
DROP TABLE IF EXISTS mail_attachment;
DROP TABLE IF EXISTS mail_instance;
