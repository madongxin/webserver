# 稳定评估阶段三进度（对照 `GameMesh_最新稳定版本评估_f95fe7f.md`）

基线：阶段一、二已完成。本阶段：**可靠 Push + 正式门禁**。

## 已完成

### P1-1 Push ACK 连续性

- `PushReplayStore` Lua ACK：校验 `lastAck+1 .. ack` 每一序号均已写入；缺口返回 `NEED_SNAPSHOT`。
- Gateway PushAck：Gap → `NEED_SNAPSHOT`。
- `push_full_snapshot_test`：中间空洞 ACK 3 拒绝 + ACK 1 仍可。

### P1-1 E2E server_seq

- `game_tcp_e2e_client`：解析 `server_push.server_seq`；回放帧无 envelope / seq 非法失败。
- `dual-gw`：`replay_n == 0 && !need_snapshot` → 失败（exit 16），不再 WARN。

### P1-2 行尾空格

- 清理 `docs/urgent_minimum_stable_fix_phase2_progress.md` 行尾空格（原 HEAD `git show --check` 失败原因）。

## 阶段门禁

| 项 | 状态 |
| --- | --- |
| ACK 连续性单测 | PASS `push_full_snapshot_test` / `push_ack_redis_test` / `push_replay_store_test` |
| E2E dual-gw 硬失败 | 代码已改；需集群验证 |
| `git show --check`（最终 commit） | 工作区 `git diff --check` PASS；HEAD 仍含旧空格直至提交 |
| 完整 `stable_gate` | 需最终 commit 后执行 |

## 说明

完整 `./scripts/stable_gate.sh --full` 含长负载/soak，须在**包含本阶段全部改动的最终 commit**上执行；门禁报告 commit 字段须对应该 commit。
