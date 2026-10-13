# 邮件奖励一致性合同

协议文件 `proto/game.proto` 的 SHA-256 是 `47e6fbf688af61b31ad4d5e2ecf88a17e9891521ad9950bb79076456a87a8d1a`。本轮没有改字段号，也没有新增消息。

## 权威资产

客户端确认到账用已有的 `WorldSnapshotReq`（`GameRequest.world_snapshot = 72`）。回包是 `GameResponse.full_snapshot = 54`：`player_id`、`asset_version`、`item_ids`、`item_counts`。Gateway 在 `GameService` 里用已认证会话覆盖请求里的 `player_id`，不一致则拒绝。

`asset_version` 来自 GameDB `player_asset_meta`。同版本重复应用不加道具。内存版本比已提交版本小，或中间缺版本时，服务端从 `player_asset_bag` 重载，不用增量相加。内存标脏且重载失败时，快照 `error_code` 为 `STATE_SYNC_REQUIRED`，不能把当时的内存数量当到账。

`mailbox_version` 只描述邮箱，不能当作资产版本。

登录时正式 GameLogic 通过 GameDB `LoadInventory` 装入背包和版本。重连后客户端发 `WorldSnapshotReq`。

## 领取

玩家领取走 `MailClaimReq`（34）或 `MailBatchClaimReq`（35）。`idempotency_key` 必填。真正入账在 GameDB 的一个 MySQL 事务里：锁邮件行和背包行，写 `player_asset_bag`、邮件 `CLAIMED`、附件 `CLAIMED`、`mail_operation_log`、`game_db_outbox`。任一步失败就回滚，邮件保持可领。

同一封邮件只入账一次。第二把钥匙看到 `CLAIMED` 后返回 `ALREADY_CLAIMED`，`attachment_state=CLAIMED`，`ok=true`。这表示已经入账，不是再发一份道具。钥匙换角色、换邮件或换操作类型返回 `IDEMPOTENCY_CONFLICT`。

`STATE_SYNC_REQUIRED` 表示数据库可能已经入账，内存没对齐。客户端重拉 `full_snapshot`，不要显示“未到账”，也不要换一把新钥匙再领。

`INVENTORY_FULL` 和数量溢出的 `INVALID_ATTACHMENT` 会回滚，邮件仍是 `UNCLAIMED`。背包软上限默认 999999，以服务端 `MailService` 传入 GameDB 的值为准。

一键领取里，附件 payload 含 `need_confirm` 或 `COD` 的邮件会在 `results` 里返回 `INVALID_STATE` / `manual confirm required`，不会被静默跳过。

## 示例

领取成功后，客户端用原 `idempotency_key` 重试，应看到 `error_code=OK` 或 `ALREADY_CLAIMED`，背包数量不变。然后发：

```text
WorldSnapshotReq { player_id 被 Gateway 覆盖 }
```

期望 `full_snapshot.ok=true`，`asset_version` 与领取后的 GameDB 版本一致，对应 `item_id` 的 `item_counts` 含本次数量。若 `error_code=STATE_SYNC_REQUIRED`，稍后再拉，不要本地加道具。
