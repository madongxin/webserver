# 邮件奖励客户端接力

服务端仓库只改了这一侧。Unity 工程没有在本机运行。

## 客户端要接的接口

- 领取：`MailClaimReq` / `MailBatchClaimReq`。每封结果看 `MailClaimResult`，不要只看外层 `GameResponse.ok`。
- 到账确认：`WorldSnapshotReq` → `full_snapshot.asset_version` + `item_ids` / `item_counts`。
- 邮件状态：`MailGetReq`。未领是 `UNCLAIMED`，已领是 `CLAIMED`。
- 登录、重连、`STATE_SYNC_REQUIRED` 之后都拉一次世界快照。不要用 `mailbox_version` 对账道具。

## 错误码

| 码 | 客户端动作 |
| --- | --- |
| `OK` | 本封已按服务端结果处理。增量只在内存版本刚好落后 1 且服务端带回了本次 grant 时应用；客户端自己不要再加一遍 |
| `ALREADY_CLAIMED` | 已入账。保持当前背包，用快照对齐，禁止再加附件数量 |
| `STATE_SYNC_REQUIRED` | 数据库可能已入账。拉 `full_snapshot`，失败则稍后重试同一把钥匙 |
| `INVENTORY_FULL` | 没入账。邮件还在，清出背包后再用原钥匙领 |
| `INVALID_ATTACHMENT` | 没入账。不要重试出新奖励 |
| `INVALID_STATE` 且 message 为 `manual confirm required` | 这一封被一键领取显式跳过，不是成功 |
| `PERMISSION_DENIED` | 不是自己的邮件 |
| `CLAIM_IN_PROGRESS` | 稍后用原钥匙查询，不要换钥匙 |

## 服务端现状

正式环境玩家不能 `player_mail_send`，也不能走玩家口的 `mail_deliver`。系统邮件只走内部 `GAMEMESH_MAIL_DELIVER_TOKEN` 或进程内 `MailService::Deliver`。令牌不要写进客户端。

本轮没有改 proto，不需要重新导出协议。字段说明见 `docs/mail_reward_integration_contract.md`。

## 联调步骤

1. 用测试角色登录 Gateway。地址沿用现有 VIP `10.0.0.2:8081`。
2. 用内部投递给该角色一封固定 `item_id/count` 的 SYSTEM 邮件。
3. 领之前拉 `WorldSnapshotReq`，记下 `asset_version` 和该道具数量。
4. `MailClaimReq` 带稳定的 `idempotency_key`。
5. 再拉快照。数量应增加约定值，版本加 1。用同一把钥匙再领一次，数量不变。
6. 换一把钥匙再领，应得到 `ALREADY_CLAIMED`，数量仍不变。
7. 用另一个角色领同一封，应 `PERMISSION_DENIED`，两边背包都不变。

没有 Unity 日志之前，不要把双端联调写成通过。
