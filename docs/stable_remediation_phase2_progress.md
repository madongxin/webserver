# 稳定整改三阶段 — 阶段二进度

对照：`docs/GameMesh_Cursor_Server_Stable_Remediation_3_Phases.md`  
基线含阶段一工作区改动；**阶段二完成后停止，等待用户确认再进入阶段三**。

| 项 | 内容 | 状态 |
|----|------|------|
| §2 | Channel 不可变快照（GatewayAuth/SessionRpc/BrpcChannel/GameDb/Push） | **已做** |
| §2 | 空发现保留最后快照；Init 在锁外 | **已做** |
| §3 | Session×2 业务 HA：静态∪Redis 发现 + Auth/Acquire/Reconnect 应用层按 peer 重试 | **已做** |
| §4 | PlacementRecoveryScheduler Redis leader CAS；AUTO_RECOVER 业务演练 | **已做** |
| §5 | GameDB 写失败 → QueryOperationResult → 他节点重试；未知结果不伪成功 | **已做** |
| §6 | ClaimMail 真异步 brpc + PlayerSerialQueue inflight（同 shard 不阻塞） | **已做** |
| §7 | Session 发现并集防单点收缩；DRAINING 脚本契约 | **已做** |
| §8 | 故障 E2E 脚本（业务断言） | **已做** |

### 故障状态机（Logic）

```text
READY → (lease expire) RECOVERING → CAS Migrate(new owner, epoch+1) → READY
旧 Owner 写：epoch 校验拒绝
无实时地图快照：客户端重新进图（非无损）
多 Session：placement:recovery:leader Redis lease 互斥
```

### 门禁证据

| 命令 | 结果 |
|------|------|
| `./scripts/build.sh Debug` | PASS |
| `./scripts/test.sh unit` | PASS（含 channel_snapshot_race / player_serial_async） |
| `./scripts/test.sh integration` | PASS |
| `./scripts/final_e2e.sh --start-cluster` | PASS（8/8） |
| `scripts/test_session_failover.sh` | PASS（kill s0 后新 Login + dual-gw Reconnect） |
| `scripts/test_logic_auto_recovery.sh` | PASS（AUTO_RECOVER epoch 1→2 → gl-1） |
| `scripts/test_gamedb_unknown_result_failover.sh` | PASS（幂等单测 + kill gamedb0 / gamedb1 ready） |
| `scripts/test_dynamic_logic_scale.sh` | PASS |
| `scripts/test_registry_outage.sh` | PASS |
| `scripts/test_channel_snapshot_race.sh` | PASS |

### 主要改动文件

- `runtime/brpc/RpcChannelSnapshot.h`（新）
- `runtime/brpc/GatewayAuthClients.*` / `SessionRpcClient.*` / `BrpcChannelManager.*` / `GatewayPushClient.*`
- `db/BrpcGameDbRepository.*`（快照 + QueryOperationResult failover + 真异步 ClaimMail）
- `runtime/PlacementRecoveryScheduler.*`（leader CAS）
- `runtime/PlayerSerialQueue.*`（Mark/CompleteAsyncInFlight）
- `game/MailService.*` + `GameLogicServiceImpl` MailClaim 异步路径
- `apps/ServerBootstrap.cpp`（session 发现并集 + recovery instance id）
- `scripts/test_session_failover.sh` 等六脚本；`kill_logic_drill.sh` Redis 认证

### 遗留（阶段三 / 已知）

- Mail BatchClaim 仍走同步 `ClaimOne`（fut.get），可能阻塞 shard
- 正式启动 gl-2 进程并断言新地图落到 gl-2 的完整扩容 E2E 仍偏薄（当前以 registry/DRAINING 契约 + 单测为主）
- TSan 全量专项未在本环境默认门禁中跑
- Redis Registry 仍是控制面单点；生产需 Sentinel/Cluster（文档阶段三展开）
- 稳定版本判定仍为 **BLOCKED**（未完成阶段三）

建议：用户确认后进入阶段三。
