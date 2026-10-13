-- 按一封邮件核对首次入账，不用背包期末余额代替流水。
-- 把下面两个数字换成测试角色和 mail_id。不要对正式角色执行。
-- 期望：ok_claim_count = 1，附件 claim_state = CLAIMED，
-- after_state 里的 tx / asset_version / grants 与领取响应一致。

SET @player_id = 0;
SET @mail_id = 0;

SELECT COUNT(*) AS ok_claim_count
FROM mail_operation_log
WHERE mail_id = @mail_id
  AND actor_id = @player_id
  AND operation_type = 'CLAIM'
  AND result_code = 'OK';

SELECT operation_id, idempotency_key, result_code, after_state
FROM mail_operation_log
WHERE mail_id = @mail_id
  AND actor_id = @player_id
  AND operation_type = 'CLAIM'
ORDER BY operation_id ASC;

SELECT slot_index, asset_id, count, claim_state, asset_transaction_id
FROM mail_attachment
WHERE mail_id = @mail_id
ORDER BY slot_index ASC;

SELECT mail_id, receiver_id, attachment_state, visible_state
FROM mail_instance
WHERE mail_id = @mail_id
  AND receiver_id = @player_id;
