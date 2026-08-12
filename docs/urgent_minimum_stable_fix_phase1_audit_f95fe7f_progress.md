# 稳定评估阶段一进度（对照 `GameMesh_最新稳定版本评估_f95fe7f.md`）

基线评估：`docs/GameMesh_最新稳定版本评估_f95fe7f.md`（STABLE BLOCKED）。
本阶段只做 **数据与执行正确性**，不宣称 STABLE PASS。

## 已完成

### P0-2 PlayerSerialQueue 链式异步串行

- `async_inflight` 改为 `async_depth`：嵌套/链式 `MarkAsyncInFlight` 用深度计数。
- `CompleteAsyncInFlight`：先入队 completion，**depth 在 completion 执行后再减**；completion 内再 Mark 时 deferred 不会提前释放。
- 单测 `player_serial_async_test` 增加链式异步复现：同玩家 deferred 不得与第二段异步并发（`chain_serialization_violated`）。

### P0-3 邮件奖励 → 正式资产

- `GameDbAssetStore::GrantItemsOnConnection`：在已有 MySQL 事务上 GRANT 多道具并 bump `asset_version`。
- `AsyncMysqlGameDbRepository::DoClaimMail`：不再写 `player_item`；与邮件 CLAIMED / op log / outbox **同一事务**写入 `player_asset_bag` + `player_asset_meta`。
- `LoadInventory`：同一连接事务读 meta+bag；**移除**空 bag 时对 `player_item` 的运行期 fallback。
- `gamedb_mail_claim_test`：断言正式 bag/version；增加「已有正式背包 → 领取 → 模拟 kill/relogin 只从 bag 恢复」用例。

## 阶段门禁（本阶段）

| 项 | 状态 |
| --- | --- |
| 同玩家链式异步串行单测 | PASS `player_serial_async_test` |
| 邮件正式 bag + 幂等/崩溃重试单测 | PASS `gamedb_mail_claim_test` |
| 全量 `stable_gate` / STABLE PASS | **未做**（阶段二+） |

## 刻意不做（留给后续阶段）

- Placement 死 Owner 软续租 / epoch（阶段二）
- Gateway 断线异步 brpc（阶段二）
- Push ACK 连续性（P1）
- 全量 load/soak 与打 tag
