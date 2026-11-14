# P0：admitted/defer 恒 0 根因与修复（2026-11-14）

状态：**代码已修路由，待 enforce=1 短复压验收。**  
关联：`aoi_move_reduction_plan_20261113.md` 阶段 A、`egress_enforce_acceptance_20261113.md`。

---

## 1. 结论

**不是**「帧一直停放所以 NoteKernelWrite 从不触发」为第一因（那是次要口径）。  
**第一因：** `GatewayPushServer` 把所有 `aoi.delta.v1`（含 GameLogic 标了 `coalescable=true` 的纯 MOVE）都送进 `send_frame_urgent` → **合并槽 / Pick / EGRESS_ENFORCE 整条旁路从未接到生产流量**。

现场证据（905 人压测与当前进程一致）：

| 指标 | 值 | 含义 |
|------|-----|------|
| `tx_frames_by_frame{move}` | 数十万～百万 | 有出站 MOVE（急送 TrySend） |
| `move_offer_total` | **0** | 从未进入 coalesce Offer |
| `move_flush_total` | **0** | FlushMoveSlot 未交卷 |
| `aoi_move_commit_callbacks_total` | **0** | 无内核确认回调 |
| `egress_admitted_bytes_total` | **0** | 无 NoteKernelWrite 记账 |
| `aoi_move_budget_defer_total` | **0** | Pick 未跑，谈不上 defer |

Ledger 单测里 enforce 行为是对的；**生产接线把流量绕开了 ledger**，所以开不开 `EGRESS_ENFORCE` 出口一样贴顶。

---

## 2. 因果

```
GameLogic PublishAoiBatch
  纯 MOVE → reliable=false, coalescable=true
  ENTER/LEAVE → reliable=true, coalescable=false
        │
        ▼
GatewayPushServer env.send   ← 旧：凡 aoi.delta.v1 → send_frame_urgent
        │
        ├─ 急送 TrySend / AoiNoteFrameWrite  → tx_frames{move}↑
        └─ 不经 OfferCoalescedMove           → offer/flush/admitted/defer = 0
```

设计本意（`GameTcpGateway` 已具备）：

- `send_frame` → `OfferCoalescedMove` → `Pick`（可 defer）→ `TrySendLatestMove` → `FinishMoveCommit` → `NoteKernelWrite`（admitted）
- `send_frame_urgent` → ENTER/LEAVE 急送

---

## 3. 修复

1. `runtime/AoiPushSendRoute.h`：`AoiDeltaUsesCoalescePath(message_type, coalescable)`  
2. `GatewayPushServer.cpp`：`coalescable` 的 `aoi.delta.v1` → `bind.send_frame`；否则仍 urgent。  
3. `egress_enforce_test` 增用例 h。  
4. **默认仍不打开** `GAMEMESH_EGRESS_ENFORCE`（需单独复压验收）。

---

## 4. 验收（阶段 A 复压）

重启 Gateway 后压测，`EGRESS_ENFORCE=1` + `SHARED_CLASS_KBIT=2600`：

1. `move_offer_total` / `move_flush_total` **> 0**（路由闭合）  
2. `egress_admitted_bytes_total` 随时间增长  
3. 过载时 `aoi_move_budget_defer_total` **> 0**  
4. 相对基线：AOI move 帧/s 下降趋势；客户端 move timeout 改善或持平偏好  

客户端：**无需改协议**；照常导出即可。

---

## 5. 决策记录

| # | 决议 |
|---|------|
| 1 | 批准阶段 A / P0 |
| 2 | P0 更看重 timeout 降 |
| 7 | 不抬带宽（P5） |
