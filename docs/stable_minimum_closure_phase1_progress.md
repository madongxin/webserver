# 稳定最小闭环 — 阶段一进度

对照：`docs/GameMesh_Cursor_Server_Stable_Minimum_Closure_e8ab08f.md`
基线：`e8ab08f`；**阶段一完成，停止等待确认后再进阶段二。**

| 项 | 状态 |
|----|------|
| ApplyMutation 幂等身份（player/op/hash）+ IDEMPOTENCY_CONFLICT | **已做** |
| QueryOperationResult `status`（NOT_FOUND/IN_PROGRESS/SUCCEEDED/FAILED） | **已做** |
| `mysql_real_escape_string` + 幂等键长度/控制字符校验 | **已做** |
| FullSnapshot：ReserveSeq → baseline 编码 → AppendReserved | **已做** |
| MailBatchClaim 真异步状态机 + Dispatch 走 async | **已做** |
| CompleteAsyncInFlight 不受外部过载拒绝 | **已做** |
| 移除玩家链路 `future.get()`（ClaimOne 改为同步 RPC API） | **已做** |

### 幂等不变量 / schema

- 命中比较：`player_id` + `operation_type` + `request_hash`
- Mutation hash：`op|player|v|item|count`（FNV-1a hex）
- 占键：`ok=0,error_code=IN_PROGRESS` → 成功 finalize / 失败标记 FAILED
- 表字段沿用 `operation_type`/`request_hash`；`EnsureTables` 缺列 ALTER；proto 新增 `QueryOperationResultRsp.status=10`（兼容旧字段）

### FullSnapshot 序列唯一来源

`PushReplayStore::ReserveSeq` 分配的 `server_seq` = `FullSnapshot.baseline_server_seq` = Redis 条目 seq = 即时 `ServerPushEnvelope.server_seq`；Redis 与即时 payload 同一份已含 baseline 的编码。

### BatchClaim / 线程归属

`BeginHandleMailBatchClaimAsync`：逐封 `ClaimMailAttachmentsAsync` → `CompleteAsyncInFlight` 回投 → ApplyClaimMemory → 下一封；`GameLogicServiceImpl` 对 `mail_claim`/`mail_batch_claim` 均走 async。

### 门禁证据

| 命令 | 结果 |
|------|------|
| `./scripts/check_deps.sh --full` | PASS |
| `GAMEMESH_JOBS=2 ./scripts/build.sh Debug` | PASS |
| `./scripts/test.sh unit` | PASS |
| `./scripts/test.sh integration` | PASS（含 mutation idem + push reserve） |
| `./scripts/test_sanitizers.sh asan` | PASS |
| `./scripts/test_sanitizers.sh ubsan` | PASS |
| `rg future.get game runtime apps db` | 无命中 |

### 主要改动文件

- `db/Connection.*`、`db/GameDbAssetStore.*`、`db/BrpcGameDbRepository.*`
- `proto/gamedb.proto` + `runtime/brpc/gamedb.pb.*`、`GameDbServiceImpl.cpp`
- `game/PushReplayStore.*`、`runtime/brpc/GatewayLoginOrchestrator.cpp`
- `runtime/PlayerSerialQueue.cpp`、`game/MailService.*`、`GameLogicServiceImpl.cpp`
- `test/gamedb_mutation_idempotency_test.cpp`、`push_full_snapshot_test.cpp`、`player_serial_async_test.cpp`、`placement_recovery_test.cpp`

### 遗留（阶段二）

- 真实进程故障 E2E / stable_gate 顺序 / gl-2 真扩容等（见闭环文档阶段二）
- MailBrpc 同步 Claim 仍走同步 RPC（非玩家串行线程；可后续异步化）
- 未 commit / 未 push

**请确认后进入阶段二。**
