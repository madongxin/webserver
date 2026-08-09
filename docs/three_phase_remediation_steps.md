# 三阶段整改：多步骤拆解与进度

> 对照：`docs/GameMesh_Cursor_Three_Phase_Remediation.md`  
> 规则：阶段一门禁通过前不开始阶段二；阶段二通过前不开始阶段三。默认不 git commit/push。

## 阶段一 — 分布式核心正确性（完成）

| 步骤 | 内容 | 状态 |
|------|------|------|
| 1.1–1.9 | 串行队列 / client_seq / 可信身份 / lease / Channel / Session 幂等 / 单测门禁 | **已做** |

## 阶段二 — 迁移 / 故障恢复 / 可靠 Push（完成）

| 步骤 | 内容 | 状态 |
|------|------|------|
| 2.1 | `PlayerTransferSnapshot` + Freeze→Export→Import→Cutover→Unbind（EnterMap 跨 Logic） | **已做** |
| 2.2 | `PlacementRecoveryScheduler`：SCAN 过期 lease→RECOVERING→Migrate + 审计 | **已做** |
| 2.3 | Push session 隔离、`ServerPushEnvelope`、ACK 校验、缺口全量快照、物品 Push | **已做** |
| 2.4 | `phase2_transfer_snapshot_test` / `placement_recovery_test` + unit/integration 门禁 | **已做** |

说明：

- 迁移状态机：`GatewayEnterMapOrchestrator`（Begin→Freeze→Export→PrepareBind→Import→Commit→Dispatch→Unbind）。
- 地图接管非无损实时恢复：自动 Migrate 后客户端需重新进图/安全点（日志明示）。
- `kill_logic_drill.sh AUTO_RECOVER=1` 可等待 Session 调度器自动接管。

## 阶段三 — 数据边界 / 发现 / 生产化（MVP 完成）

| 步骤 | 内容 | 状态 |
|------|------|------|
| 3.1 | GameDB 资产 RPC（Load/Inventory/Mutation/Snapshot/Flush）+ 版本 CAS/幂等/Outbox；Formal Logic 经 brpc GameDB；Auth Redis 限流；Outbox 双 Claim 测 | **已做** |
| 3.2 | RedisServiceRegistry（Lease/Discover/DRAINING）+ Gateway 轮询 ApplySnapshot（含 gl 热扩） | **已做**（etcd v3 Watch 可后续替换） |
| 3.3 | Docker `USER` 非 root；CI ShellCheck + ASan smoke | **部分**（完整 mTLS/UBSan/TSan/混沌 E2E 仍可加深） |

说明：

- 资产权威：`GameDbAssetStore` + `GameDbServiceImpl`；Formal 下 Logic 不再以本地 PersistQueue 为事实源。
- 发现：`RedisServiceRegistry` 与静态/cnf 降级并存；空快照不覆盖健康 Channel。
- 测试：`phase3_discovery_test`（integration）；`phase3_gamedb_asset_test`（需可达 MySQL，否则 timeout SKIP）。
- 门禁：`./scripts/build.sh Debug` + `./scripts/test.sh unit` + `./scripts/test.sh integration`。

---

验收命令：

```bash
./scripts/check_deps.sh
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
# 可选演练（需集群）：
# AUTO_RECOVER=1 ./scripts/kill_logic_drill.sh
# ./scripts/test_dual_gw_e2e.sh
```
