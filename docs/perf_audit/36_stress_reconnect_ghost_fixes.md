# 压测问题核对与修复（605 登录 / reconnect failed / 幽灵分线）

## 结论（这次压测是不是“错的”）

**服务端问题真实存在，客户端报错不全是误报。**

| 现象 | 服务端证据 | 判定 |
|------|------------|------|
| 客户端 reconnect failed ×数百 | Gateway `reconnect_ok` 很高、`reconnect_fail=0` | **半真**：Session 重连 RPC 成功，但地图态/后续 Move 失败，客户端记成 reconnect failed |
| 1–6 线满员但在线仅几十 | MarkDisconnected **不减** `map:occ`，宽限内占 softcap | **真**：幽灵占用 |
| 心跳后大批重连 | `close drops unsent`、迟到 Unbind 竞态 | **真**：连接不稳 + 重连后被旧 Unbind 再 Disconnect |
| AOI ERR_AOI_RESYNC | push_ack stale | 高压下存在，次要 |

## 已落地修复

1. **分线幽灵占用**：`MarkDisconnected` → `ParkForGrace`（从 `map:occ` 摘除、保留 `map:pres`）；重连 Bind → `ResumeParked`（**仅 on_map=true**）。
2. **Unbind/Reconnect 竞态**：异步 Unbind 在队列内用 session/fence/generation 再校验，旧 TCP 迟到 Unbind 不再抹掉新 Bind / 再 Disconnect。
3. **ReconnectRsp 明示进图**：新增 `map_instance_id` / `gamelogic_instance_id` / `need_enter_map` / `on_map`（append-only）。`need_enter_map = !on_map`（不再因 `map_instance_id==0` 误标）。
4. **重连风暴配额**：formal 启动默认抬高 `LOGIN_INFLIGHT` / `RECONNECT_RESERVE`。
5. **AOI LEAVE 必须下发**：`Disconnect` 恢复对邻居 LEAVE（静默断线会导致客户端幽灵模型）。`Reconnect` 补邻居 ENTER；reliable 推送用 `TrySendUrgent`（90% 水位）。
6. **重连回放限流**：超过 8 条 / 48KB 改 `need_full_snapshot`，避免补发包打满 send_buf。
7. **TrySend 50% / Urgent 90%**；`ConfirmSlot` 同步把 parked 加回 `map:occ`。
8. **occ 对账**：Session leader 定时 `ReconcileGhostMapOccupancy`，非 ONLINE 立刻 SREM softcap。
9. **断线 flush_bag**：带上 registry sticky；无 sticky 时跳过，消除 `missing sticky gamelogic_instance_id` 刷屏。

## 客户端必跟

- 拉取新 `schema_sha256`（`docs/protocol/export/`）：`edfd9fb92b1961dac7451d0b78be4e12ef9ee1424d9716d7e02dbcd5d237c6ae`
- `ReconnectRsp.ok && need_enter_map` → **立刻 EnterMap**，不要直接 Move/Heartbeat 当已进图。
- 心跳失败走 Reconnect；`need_enter_map` 后再进图；失败退避，勿立刻顶号 Login。

## 验证建议

再跑 605：观察页「分线人数」应贴近真实在线（断线后快速下降）；`close drops unsent` 应大幅下降；Gateway `reconnect_ok` 后若 `on_map=1` 可直接玩，若 `need_enter_map` 再进图。
