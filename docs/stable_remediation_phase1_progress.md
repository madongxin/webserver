# 稳定整改三阶段 — 阶段一进度

对照：`docs/GameMesh_Cursor_Server_Stable_Remediation_3_Phases.md`
基线：`d631c2f`；**阶段一完成后停止，等待用户确认再进入阶段二**。

| 项 | 内容 | 状态 |
|----|------|------|
| §2 | GrantItem/MailDeliver 公网封闭 + `CommandPolicy` | **已做** |
| §3 | Push ACK 原子 ahead/stale/duplicate + 指标 | **已做** |
| §4 | FullSnapshot 导出/写入失败不伪成功 | **已做** |
| §5 | SaveSnapshot 幂等 + request_hash 冲突 | **已做** |
| §6 | `QueryOperationResult` RPC + 存储查询 | **已做** |
| §7 | Formal Placement fence 必填 epoch/route | **已做** |
| §8 | Gateway Login/EnterMap `TryPost` + ERR_OVERLOAD | **已做** |

### 门禁证据

| 命令 | 结果 |
|------|------|
| `./scripts/check_deps.sh --full` | PASS |
| `./scripts/build.sh Debug` | PASS |
| `./scripts/test.sh unit` | PASS（含 command_policy / placement_formal_fence / gateway_overload） |
| `./scripts/test.sh integration` | PASS（push_ack / push_full_snapshot / gamedb snapshot+query） |

### 调用链变化（摘要）

- Client TCP → `AllowClientTcpPayload` → Formal 拒绝 GrantItem/MailDeliver（`ERR_COMMAND_FORBIDDEN`）
- PushAck → Redis Lua 校验 current_seq/last_ack → 指标分桶
- Reconnect FullSnapshot：Export 失败不 AppendReliable、不下发 ok 快照
- GameDB SaveSnapshot：先占幂等键 → 资产 CAS → Outbox → 完成；查询走 `QueryOperationResult`
- Formal EnterMap/MapPing：`require_complete_fence=true`

### 遗留（阶段二+）

- Channel 快照并发、Session×2 业务 HA、gl-2 动态发现 E2E、CI brpc 镜像、Release/ASan/20×E2E/soak
- 稳定版本判定仍为 **BLOCKED**（未完成阶段二/三门禁）

建议：用户确认后进入阶段二。
