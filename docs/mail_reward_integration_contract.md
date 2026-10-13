# 邮件奖励一致性合同

`proto/game.proto` 的 SHA-256 是 `49479bbdb5f8f1c5e5a0b649f21bb5c265122b01f4656b18638c87a8f6102792`。这一轮只追加字段，没有改旧字段号。

客户端需要重新导出协议：

```text
./scripts/export_unity_protocol.sh <输出目录>
```

不要覆盖 `docs/protocol/published/v1`。Hello 的 `schema_sha256` 必须换成上面的值，否则 `ERR_SCHEMA_MISMATCH`。

## 领取回执

`MailClaimResult.receipt` 的字段号是 6。`MailDetail.receipt` 的字段号是 5。类型是 `MailClaimReceipt`：

| 字段 | 编号 | 含义 |
| --- | --- | --- |
| `claim_tx_id` | 1 | 首次成功提交的资产事务号，形如 `mail:<mail_id>:<unix秒>` |
| `asset_version_at_claim` | 2 | 那一次入账提交后的 `asset_version` |
| `grants` | 3 | 原始附件清单，`asset_id` + `count`。相同物品不合并，背包里会合并 |
| `claim_state` | 4 | `UNCLAIMED`、`IN_PROGRESS`（库存里的 `CLAIMING`）、`CLAIMED`、`UNKNOWN` |
| `verifiable` | 5 | 首次 OK 日志里事务号、版本和 grants 都在。缺字段的旧邮件是 false |

查询入口是已有的 `MailGetReq`（32）。Gateway 用已认证会话覆盖 `player_id`。查别人的邮件返回 `PERMISSION_DENIED`，不带物品明细。

同一封邮件换 `idempotency_key` 得到 `ALREADY_CLAIMED` 时，`receipt` 仍是第一次 OK 入账的事务号和版本。内部 GameDB 的 `ClaimMailRsp` 同步增加了 `claim_tx_id=10`、`asset_version_at_claim=11`、`receipt_verifiable=12`。

旧日志只有后来补上的当前版本、没有 grants 时，`verifiable=false`。服务端不会用 `LoadMeta` 的当前版本填这个字段。

## 怎么算这封邮件到账

三件同时成立：

1. `claim_state=CLAIMED` 且 `verifiable=true`。
2. `claim_tx_id` 和 `asset_version_at_claim` 与首次领取一致。
3. 当前角色的 `full_snapshot` 成功，`asset_version` 不早于 `asset_version_at_claim`，并且内存没有标脏。

快照版本变大，可能是任务、商城或另一封邮件。不能单凭版本前进把某一封邮件标成已到账。后续消费会让余额变小，不要用“余额仍大于当时发放量”来证明来源。

`STATE_SYNC_REQUIRED` 表示数据库可能已经入账，内存没对齐。保留原钥匙重试，并拉 `WorldSnapshotReq`。回执仍以数据库里的首次 OK 日志为准。

`INVENTORY_FULL` 和 `INVALID_ATTACHMENT`（含数量加总超过 `uint32`）会整笔回滚。邮件保持 `UNCLAIMED`，没有回执。累加恰好等于 `uint32` 上限时，聚合不回绕；是否入账再由背包软上限 999999 决定。

## 示例

领取成功后读 `result.receipt`。再发一笔其它来源的发奖，然后 `MailGet` 同一 `mail_id`。`claim_tx_id` 和 `asset_version_at_claim` 不变，此时 `Bag` 的当前版本可以更大。换一把钥匙再领，回执仍是原来那一对。
