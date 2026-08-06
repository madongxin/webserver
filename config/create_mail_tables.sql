-- 邮件系统表（MMO 收件箱）
-- 库名与 config/mysql.cnf 中 dbname 一致（默认 metrics）
-- 回滚见 config/drop_mail_tables.sql
USE metrics;

CREATE TABLE IF NOT EXISTS mail_instance (
  mail_id              BIGINT AUTO_INCREMENT PRIMARY KEY,
  owner_scope          VARCHAR(16)  NOT NULL DEFAULT 'ROLE' COMMENT 'ROLE|ACCOUNT',
  receiver_id          BIGINT       NOT NULL,
  sender_type          VARCHAR(16)  NOT NULL DEFAULT 'SYSTEM',
  sender_id            BIGINT       NOT NULL DEFAULT 0,
  source_system        VARCHAR(64)  NOT NULL,
  business_key         VARCHAR(128) NOT NULL,
  template_id          VARCHAR(64)  NOT NULL DEFAULT '',
  template_version     INT          NOT NULL DEFAULT 1,
  category             VARCHAR(16)  NOT NULL DEFAULT 'SYSTEM',
  priority             INT          NOT NULL DEFAULT 0,
  sender_name_snapshot VARCHAR(64)  NOT NULL DEFAULT '',
  title_snapshot       VARCHAR(256) NOT NULL DEFAULT '',
  body_snapshot        TEXT         NOT NULL,
  read_state           VARCHAR(16)  NOT NULL DEFAULT 'UNREAD',
  read_at              BIGINT       NULL COMMENT 'UTC unix sec',
  visible_state        VARCHAR(16)  NOT NULL DEFAULT 'ACTIVE',
  has_attachment       TINYINT      NOT NULL DEFAULT 0,
  attachment_state     VARCHAR(16)  NOT NULL DEFAULT 'NONE',
  is_favorite          TINYINT      NOT NULL DEFAULT 0,
  sent_at              BIGINT       NOT NULL COMMENT 'UTC unix sec',
  expire_at            BIGINT       NOT NULL COMMENT 'UTC unix sec',
  deleted_at           BIGINT       NULL,
  row_version          BIGINT       NOT NULL DEFAULT 1,
  created_at           BIGINT       NOT NULL,
  updated_at           BIGINT       NOT NULL,
  UNIQUE KEY uk_mail_deliver (source_system, business_key, receiver_id),
  KEY idx_mail_receiver_vis_sent (receiver_id, visible_state, sent_at),
  KEY idx_mail_receiver_cat_read (receiver_id, category, read_state),
  KEY idx_mail_expire (expire_at),
  KEY idx_mail_attach_state (attachment_state),
  KEY idx_mail_favorite (receiver_id, is_favorite)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS mail_attachment (
  attachment_id         BIGINT AUTO_INCREMENT PRIMARY KEY,
  mail_id               BIGINT       NOT NULL,
  slot_index            INT          NOT NULL,
  asset_type            VARCHAR(16)  NOT NULL DEFAULT 'ITEM',
  asset_id              BIGINT       NOT NULL,
  count                 INT          NOT NULL DEFAULT 1,
  bind_type             VARCHAR(16)  NOT NULL DEFAULT 'NONE',
  payload               TEXT         NOT NULL,
  claim_state           VARCHAR(16)  NOT NULL DEFAULT 'UNCLAIMED',
  asset_transaction_id  VARCHAR(64)  NOT NULL DEFAULT '',
  claimed_at            BIGINT       NULL,
  created_at            BIGINT       NOT NULL,
  updated_at            BIGINT       NOT NULL,
  UNIQUE KEY uk_mail_slot (mail_id, slot_index),
  KEY idx_mail_attach_mail (mail_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS mail_operation_log (
  operation_id     BIGINT AUTO_INCREMENT PRIMARY KEY,
  mail_id          BIGINT       NOT NULL DEFAULT 0,
  actor_id         BIGINT       NOT NULL,
  operation_type   VARCHAR(32)  NOT NULL,
  idempotency_key  VARCHAR(128) NOT NULL,
  before_state     TEXT         NOT NULL,
  after_state      TEXT         NOT NULL,
  result_code      VARCHAR(64)  NOT NULL,
  trace_id         VARCHAR(64)  NOT NULL DEFAULT '',
  created_at       BIGINT       NOT NULL,
  UNIQUE KEY uk_mail_op_idem (idempotency_key),
  KEY idx_mail_op_mail (mail_id),
  KEY idx_mail_op_actor (actor_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
