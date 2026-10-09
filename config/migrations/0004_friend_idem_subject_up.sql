-- 幂等行记住申请时提交的角色名。旧行 subject_name 为空，只能按已存的 peer_player_id 重放。
ALTER TABLE friend_op_idempotency
  ADD COLUMN subject_name VARCHAR(64) NOT NULL DEFAULT '' AFTER peer_player_id;

-- 申请幂等键按申请人隔离，不同玩家可以各自使用同一串 operation_id。
ALTER TABLE friend_request
  DROP INDEX uk_idempotency,
  ADD UNIQUE KEY uk_actor_idempotency (from_player_id, idempotency_key);
