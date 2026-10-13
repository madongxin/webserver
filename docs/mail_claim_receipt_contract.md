# 邮件领取凭证合同

`proto/game.proto` 的 SHA-256 是 `49479bbdb5f8f1c5e5a0b649f21bb5c265122b01f4656b18638c87a8f6102792`。本轮没有改字段号。

Gateway 在 `GameLogic::Handle` 里把 `mail_get=32`、`mail_claim=34`、`mail_batch_claim=35` 交给 `MailService`。回包是同一个 `GameResponse`，序列化后字段仍在。`player_id` 由已认证会话覆盖，回执结构里不再重复放角色 ID。

## 字段

`MailClaimReceipt`：

| 字段 | 编号 |
| --- | --- |
| `claim_tx_id` | 1 |
| `asset_version_at_claim` | 2 |
| `grants` | 3，元素是 `MailClaimGrant.asset_id=1`、`count=2` |
| `claim_state` | 4 |
| `verifiable` | 5 |

挂载位置：`MailDetail.receipt = 5`，`MailClaimResult.receipt = 6`。批领的每一封 `results` 各带自己的回执。

`claim_state`：`UNCLAIMED`、`IN_PROGRESS`（库存状态 `CLAIMING`）、`CLAIMED`、`UNKNOWN`。只有首次 OK 日志里同时有事务号、入账版本和 grants 时 `verifiable=true` 且状态为 `CLAIMED`。旧日志缺这些字段时状态是 `UNKNOWN`，`verifiable=false`，不填当前背包版本。

`mailbox_version` 只同步邮箱。`asset_version_at_claim` 是这封邮件入账当时的资产版本。之后的任务或商城不会改它。

## 客户端

用 `./scripts/export_unity_protocol.sh` 导出这份 proto，不要手改生成的 C#，不要覆盖 `docs/protocol/published/v1`。Hello 必须带上面的 `schema_sha256`。

到账要同时看到 `verifiable=true`、`claim_state=CLAIMED`，以及权威快照的 `asset_version` 不早于 `asset_version_at_claim`。`STATE_SYNC_REQUIRED` 的响应里如果已经带了可验证回执，表示数据库已入账，显示“待确认”，用原钥匙重试并拉 `WorldSnapshotReq`。不要因为别的奖励把版本推高，就把一封 `UNKNOWN` 或 `UNCLAIMED` 邮件标成已到账。

对账 SQL：`scripts/mail_claim_reconcile.sql`。一封邮件的 `result_code=OK` 领取日志应当只有一行。
