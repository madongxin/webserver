# 邮件奖励客户端接力

本机没有 Unity 工程。客户端按 `docs/mail_reward_integration_contract.md` 接。

## 要改的协议

`schema_sha256`：`49479bbdb5f8f1c5e5a0b649f21bb5c265122b01f4656b18638c87a8f6102792`。

用 `./scripts/export_unity_protocol.sh` 导出 `game.proto`，重新生成 C#。不要手改生成文件，也不要覆盖 `docs/protocol/published/v1`。

读取：

- `MailClaimRsp.result.receipt`（`MailClaimResult` 字段 6）
- `MailGetRsp.mail.receipt`（`MailDetail` 字段 5）
- `MailClaimReceipt.claim_tx_id`、`asset_version_at_claim`、`grants`、`claim_state`、`verifiable`

`WorldSnapshotReq`（72）的 `full_snapshot` 仍是背包数量和当前 `asset_version`。它不带 `mail_id`。

## 展示规则

`verifiable=true` 且 `claim_state=CLAIMED`，并且快照 `asset_version` 不早于 `asset_version_at_claim`，才把这封邮件标成已到账。其它来源把版本推高时，不要改这封的 `claim_tx_id`。证据不够就显示“已领取，到账确认中”。

`ALREADY_CLAIMED` 不要再加 `grants`。`STATE_SYNC_REQUIRED` 后拉快照，并用原 `idempotency_key` 重试。`INVALID_STATE` 且 message 为 `manual confirm required` 的批量结果不是成功。

## 服务端已能对上的数据

测试角色由 `gamedb_mail_claim_test` 在本地库 `metrics` 里使用 `9100xx` 段。道具样例 `item_id=2101`。正式集群和正式角色不要拿来发奖或故障注入。内部投递令牌不要放进客户端。
