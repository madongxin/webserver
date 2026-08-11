# Urgent Minimum Stable Fix — 阶段一进度与验收

基线：`857d963`
文档：`docs/GameMesh_Cursor_Urgent_Minimum_Stable_Fix_857d963.md`
状态：**阶段一验收通过；按文档停止，不进入阶段二**

## 修改文件列表

- `db/GameDbAssetStore.cpp`
- `game/PushReplayStore.cpp` / `.h`
- `runtime/PushReplayCache.cpp`
- `runtime/PlayerSerialQueue.cpp` / `.h`
- `runtime/brpc/GameLogicBrpcServer.cpp`
- `game/MailService.cpp` / `game/MailTypes.h`
- `test/gamedb_mutation_atomicity_test.cpp`（新）
- `test/gamedb_mutation_idempotency_test.cpp`（`_Exit` 防连接池挂起）
- `test/push_full_snapshot_test.cpp`
- `test/player_serial_async_test.cpp`
- `test/placement_recovery_test.cpp`（leader id 唯一化 + 轮询加长，消集成抖动）
- `scripts/test_integration.sh` / `CMakeLists.txt`

## 1. 资产事务原子性

不变量：`bag + asset_version + SUCCEEDED 幂等 + Outbox` 同一 MySQL 事务。

- 失败路径只 `ROLLBACK`，禁止在含资产修改的事务里 `COMMIT FAILED`。
- `COMMIT` 失败：归还连接后 `QueryOperationResult`；命中 `SUCCEEDED` 恢复成功，否则 `UNKNOWN_RESULT`。
- 已存 `FAILED` 直接返回原 `error_code`，不再 `IDEMPOTENCY_BUSY`。
- 空 `operation_type`/`request_hash` → `IDEMPOTENCY_CONFLICT`。
- Failpoint（env）：`AFTER_BAG` / `AFTER_VERSION` / `OUTBOX_FAIL` / `FINALIZE_FAIL` / `COMMIT_FAIL`。

## 2. Push gap 检测

`expected = last_server_seq + 1`：

1. `seq <= last`：忽略
2. `seq == expected`：回放，`expected++`
3. `seq > expected`：`NEED_SNAPSHOT`
4. 遍历后若 `cur >= expected`：尾部 Reserve 空洞 → `NEED_SNAPSHOT`（含 `last=0`）

另：`AppendReserved` 同 seq 幂等；`Ack` 未写入条目 → `ERR_ACK_GAP`。

## 3. PlayerSerialQueue 状态机

`RUNNING → DRAINING → STOPPED`

- DRAINING：拒绝新 `TryPost`；async completion 仍可入队
- STOPPED：`CompleteAsyncInFlight` 返回 false，禁止 inline
- Mail：STOPPED 时 `SERVER_STOPPING`，`done` 恰好一次
- `GameLogicBrpcServer::Stop`：先 `BeginDrain`，再停 brpc，最后 `Stop` 队列

## 4. 验收结果（真实执行）

| 命令 | 结果 |
|------|------|
| `./scripts/check_deps.sh --full` | PASS |
| `./scripts/build.sh Debug` | PASS |
| `./scripts/test.sh unit` | PASS |
| `./scripts/test.sh integration` | PASS（含 atomicity / full_snapshot） |
| `./scripts/test_sanitizers.sh asan` | PASS |
| `./scripts/test_sanitizers.sh ubsan` | PASS |
| `./scripts/test_sanitizers.sh tsan` | PASS |

关键单测：

| 测试 | 结果 |
|------|------|
| `gamedb_mutation_atomicity_test` | PASS |
| `gamedb_mutation_idempotency_test` | PASS |
| `push_full_snapshot_test` | PASS |
| `player_serial_async_test`（含 drain/stop） | PASS |

## 尚未解决（留给阶段二）

- GitHub CI lowlevel/full 假绿修复
- Session/GameDB failover 脚本假绿
- Soak 采样 / stable_gate 证据
- 正式 STABLE PASS / rc1 Tag（本阶段不创建、不 Push）
