ALTER TABLE friend_request
  DROP INDEX uk_actor_idempotency,
  ADD UNIQUE KEY uk_idempotency (idempotency_key);
ALTER TABLE friend_op_idempotency DROP COLUMN subject_name;
