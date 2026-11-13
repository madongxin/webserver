# EGRESS_ENFORCE 验收压测总结（2026-11-13）

## 1. 结论（先看）

在 **905 bot / Sparse 巡逻 / 目标 5 Hz / duty 100%**、出口 HTB `1:10` **rate 2200Kbit / ceil 2600Kbit**、二进制含 D1/D2/D3（`ed2eae8` 一带）的前提下：

| 轮次 | 开关 | 稳态 A→B 出口 | 客户端 move timeout | `egress_admitted` / `budget_defer` |
|------|------|---------------|---------------------|-------------------------------------|
| 基线 | `EGRESS_ENFORCE=0`（默认） | **~2.60 Mbps**（贴 ceil） | **16.8%** | 全程 **0 / 0** |
| Enforce 复测 | `EGRESS_ENFORCE=1` + `SHARED_CLASS_KBIT=2600` | **~2.60 Mbps**（贴 ceil） | **19.0%** | 全程 **0 / 0** |

**实测效果：**

1. **没有把出口压到共享类 85% 预算以下。** 两轮稳态都贴 HTB ceil（~2.60 Mbps），与开不开 enforce 几乎无区分。
2. **客户端移动成功率没有改善。** Enforce 复测 ack 率 80.7%（基线 82.9%），timeout 率 19.0%（基线 16.8%），略差。
3. **预算强制在指标上未生效。** 两轮（含中间一次 enforce 空跑）`gamemesh_egress_admitted_bytes_total` 与 `gamemesh_aoi_move_budget_defer_total` 均为 **0**，无法用 admitted/defer 证明「按字节配额在限流」。
4. **有价值的部分是证据本身：** D1/D2 指标与 A/B/C + 客户端导出对齐完成；可判定「仅打开开关、不修 admitted 接线/计费」不足以改变 2.6 Mbps 贴顶现象。

中间还有一次 enforce 压测（22:39–22:49），服务端行为与复测一致（仍贴 ceil、admitted=0），但客户端移动统计当时未拿到非空导出，**主对比以基线 vs Enforce 复测为准。**

---

## 2. 环境与开关

| 项 | 值 |
|----|-----|
| 客户端入口 | `10.0.0.2:8081 / 8083`（VIP/双 Gateway） |
| 拓扑 | Gateway×2 + GameLogic×2 + Session×2 + GameDB×2 + GlobalService |
| 压测规模 | online≈**906**（A/B 点）；bot_count=**905** Sparse |
| 集群预算相关 | `GAMEMESH_EGRESS_BUDGET_MBPS=2.4`（start_formal 默认仍在） |
| 基线轮 | **不设** `GAMEMESH_EGRESS_ENFORCE`（默认关） |
| Enforce 轮 | `GAMEMESH_EGRESS_ENFORCE=1`，`GAMEMESH_EGRESS_SHARED_CLASS_KBIT=2600` |
| 其它实验开关 | `SENDBUF_V2=0`，`READ_BACKPRESSURE=0`，未开 `AOI_V2_*` / `TX_COALESCE` / `AOI_EXTRAPOLATE` |
| 启动日志（enforce） | `egress_enforce=1`，`egress_shared_class_kbit=2600`，`egress_headroom_percent=85`，`egress_gateway_instances=2`，`egress_bytes_per_sec=138125`（单实例） |
| 启动 WARN | legacy `EGRESS_BUDGET_MBPS` 折算 on-wire 300000 B/s **高于** shared class 85% 帽 276250 B/s —— 说明旧 Mbps 旋钮与新 shared-class 公式并存，enforce 应以 shared-class 为准 |

单实例字节预算推算（代码口径）：  
`2600 kbit × 85% ÷ 2 instances = 138125 B/s/gw`；再按连接数均分。若 admitted 真正按内核写出记账，稳态 AOI 出站应受该帽约束；**本次指标未看到约束痕迹。**

---

## 3. 轮次时间线与落盘

### 3.1 基线（enforce OFF）

| 点 | 本地时间 (CST) | online | 落盘 |
|----|----------------|--------|------|
| A | 22:16:42 | 906 | `~/tc_A_baseline.txt`，`~/metrics_A_baseline/` |
| B | 22:19:18 | 906 | `~/tc_B_baseline.txt`，`~/metrics_B_baseline/` |
| C | 22:21:19 | 1 | `~/tc_C_baseline.txt`，`~/metrics_C_baseline/` |
| 客户端 | run_id `client-20261113-221629` | — | `~/client_export_baseline1/`；亦见附件 / `docs/client-movement-20261113-224103/` 内同内容 metrics |

### 3.2 Enforce 第一次（有服务端，客户端导出缺失）

| 点 | 本地时间 (CST) | online | 落盘 |
|----|----------------|--------|------|
| 重启 | 22:31:53 | — | `egress_enforce=1` 写入 gw0/gw1 日志 |
| A | 22:39:59 | 906 | `~/tc_A_enforce.txt`，`~/metrics_A_enforce/` |
| B | 22:42:36 | 906 | `~/tc_B_enforce.txt`，`~/metrics_B_enforce/` |
| C | 22:49:05 | 1 | `~/tc_C_enforce.txt`，`~/metrics_C_enforce/` |
| 客户端 | — | — | 目录名 `224103` 内仍是 **221629 基线 metrics**；另有 `aoi-stall-20261113-223440.jsonl`（观察端卡顿） |

### 3.3 Enforce 复测（enforce ON + 非空客户端导出）——主对比轮

| 点 | 本地时间 (CST) | online | 落盘 |
|----|----------------|--------|------|
| A | 23:11:13 | 906 | `~/tc_A_enforce2.txt`，`~/metrics_A_enforce2/` |
| B | 23:12:43 | 906 | `~/tc_B_enforce2.txt`，`~/metrics_B_enforce2/` |
| C | 23:15:47 | 1 | `~/tc_C_enforce2.txt`，`~/metrics_C_enforce2/` |
| 客户端 | run_id `client-20261113-231200` | — | `~/client_export_enforce2/`，`docs/client-movement-20261113-231200/` |

说明：`230053` 曾导出过一份，但 `move_sent=0`（空包），已作废，不参与对比。

---

## 4. 服务端：出口与帧增量

### 4.1 HTB class `1:10`（sport 8081+8083 共用）

稳态对比用 **A→B**（压测仍在线）；B→C / A→C 含停测泄压，均值会偏低。

| 轮次 | A→B Δt | A→B 吞吐 | A→B drop Δ | A backlog | B backlog | C backlog |
|------|--------|----------|------------|-----------|-----------|-----------|
| 基线 | 156 s | **2.601 Mbps** | 0 | 262205 B / 1180 p | 186096 B / 992 p | 0 |
| Enforce-1 | 157 s | **2.593 Mbps** | 0 | 393437 B / 1666 p | 210223 B / 765 p | 0 |
| Enforce-2 | 90 s | **2.613 Mbps** | 0 | 176382 B / 821 p | 122593 B / 590 p | 0 |

| 轮次 | B→C Mbps | A→C Mbps |
|------|----------|----------|
| 基线 | 1.76（停测段） | 2.23 |
| Enforce-1 | 2.37（停测段较长） | 2.43 |
| Enforce-2 | 2.11 | 2.28 |

**解读：** 三轮稳态都在 **ceil 2.6 Mbps** 附近抖动（±0.02），看不出 enforce 把出站「让出」带宽。停测后 backlog 都能回到 0，说明不是永久卡死在 qdisc。

### 4.2 Gateway 帧增量（A→B 稳态窗）

| 轮次 | gw0 move_reply Δ | gw1 move_reply Δ | gw0 AOI move Δ | gw1 AOI move Δ | heartbeat Δ |
|------|------------------|------------------|----------------|----------------|-------------|
| 基线 (156s) | 60944 | 59118 | 199084 | 193127 | 0 |
| Enforce-1 (157s) | 61681 | 63893 | 200036 | 196847 | 0 |
| Enforce-2 (90s) | 34000 | 35396 | 115402 | 114184 | 0 |

折算粗速率（仅供量级，非精确 pps 拆分）：

| 轮次 | 两 gw move_reply/s（合计） | 两 gw AOI move/s（合计） |
|------|---------------------------|-------------------------|
| 基线 | ~(60944+59118)/156 ≈ **770 /s** | ~(199084+193127)/156 ≈ **2514 /s** |
| Enforce-1 | ~(61681+63893)/157 ≈ **800 /s** | ~(200036+196847)/157 ≈ **2528 /s** |
| Enforce-2 | ~(34000+35396)/90 ≈ **771 /s** | ~(115402+114184)/90 ≈ **2551 /s** |

**解读：** Enforce 打开后 AOI move / move_reply 发送速率与基线同量级，没有出现「 defer 导致 AOI 帧明显变稀」的服务端迹象。`heartbeat` 帧计数两轮均为 **0**（分类口径下无心跳帧入账，或本场景心跳不走该 frame 标签）。

### 4.3 GameLogic MOVE 结果（A→C 累计增量）

| 轮次 | logic0 ok Δ | logic1 ok Δ | logic0 duplicate_target Δ | logic1 duplicate_target Δ |
|------|-------------|-------------|---------------------------|---------------------------|
| 基线 | 118715 | 60356 | 38040 | 20208 |
| Enforce-1 | 257294 | 136458 | 82813 | 40595 |
| Enforce-2 | 128859 | 66301 | 38925 | 20318 |

Enforce-1 的 A→C 窗口更长（546s），绝对值更大属预期。Enforce-2 与基线窗口接近（274s vs 277s）时，ok / duplicate 量级接近。  
`duplicate_target` 比例在两轮都偏高（相对 ok 约 1/3～1/4 量级），这是移动请求侧现象，**不是本轮 enforce 开关能单独解释的改善点。**

### 4.4 预算相关计数（关键负结果）

| 指标 | 基线 A/C | Enforce-1 A/C | Enforce-2 A/C |
|------|----------|---------------|---------------|
| `gamemesh_egress_admitted_bytes_total` | 0 / 0 | 0 / 0 | 0 / 0 |
| `gamemesh_aoi_move_budget_defer_total` | 0 / 0 | 0 / 0 | 0 / 0 |

同时 AOI move 帧与 `egress_wire_bytes_estimate{frame="move"}` 在增长（例如 Enforce-2 A 点 gw0 move 帧已累计 144 万量级），说明 **有出站 MOVE，但 admitted 未记账**。按 D1 设计，admitted 应在 `FinishMoveCommit(true)` / `NoteKernelWrite` 路径累加；本次验收将其记为 **「强制预算未在生产指标上闭环」**，而不是「配额充足所以 defer=0」。

---

## 5. 客户端：基线 vs Enforce 复测

来源：

- 基线：`client-20261113-221629`（`movement-metrics-*.json`）
- Enforce 复测：`client-20261113-231200`

参数两边一致：`Sparse`，`bot_count=905`，`target_hz=5`，`move_duty_pct=100`，`segment 10–30s`，`aoi_bot_backoff=false`。

| 指标 | 基线 221629 | Enforce2 231200 | 相对变化 |
|------|-------------|-----------------|----------|
| move_sent | 297759 | 295266 | ≈持平 (−0.8%) |
| move_ack_ok | 246870 | 238257 | −3.5% |
| **ack / sent** | **82.91%** | **80.69%** | **−2.2 pp** |
| move_timeout | 50005 | 56035 | +12.1% |
| **timeout / sent** | **16.79%** | **18.98%** | **+2.2 pp** |
| move_resend | 45640 | 50565 | +10.8% |
| resend / sent | 15.3% | 17.1% | +1.8 pp |
| late_rsp | 27140 | 33425 | +23.2% |
| unknown_after_write | 47074 | 52274 | +11.0% |
| RPC p50 (ms) | 1071 | **785** | 中位数改善 |
| RPC p95 (ms) | 1475 | 1588 | 变差 |
| RPC p99 (ms) | 1884 | 1904 | 仍贴 2s 超时 |
| actual_send_hz（合计） | 696.4 | 631.6 | −9.3% |
| actual_success_hz（合计） | 577.4 | 509.6 | −11.7% |
| heartbeat_ok / fail | 9464 / 2351 | 10626 / 2772 | fail 仍约 20%+ 量级 |
| aoi_move_events / aoi_visible_peak | 0 / 0 | 0 / 0 | **无数据**（导出未填，不能当 0 人视野） |
| aoi-visible observers | 905，全空 | 905，全空 | 同 |

**解读：**

- 尾部延迟（p95/p99）与 timeout 仍由 **~2s 移动超时** 主导；enforce 未把 p99 拉离超时线。
- p50 在 enforce 轮更低，但与更高 timeout/resend/late_rsp 同时出现，**不能**解读为整体体验变好。
- 客户端 AOI 统计在两轮导出中均为空，**观察端卡顿需看 stall 文件，不能从 movement-metrics 推断。**

### 5.1 附：观察端 stall（非完整移动统计）

`docs/client-movement-20261113-224103/aoi-stall-20261113-223440.jsonl`（5283 事件帧，约 t=138–520s）：

| 项 | 实测 |
|----|------|
| 原因占比 | `no_aoi` **94.8%**，`held_at_sample` 4.4%，`render_stall` 0.8% |
| 涉及 bot | 主要 4 个（Bot131/291/122/294） |
| still_sec | p50 / p90 / p99 ≈ **43 / 131 / 165 s** |
| sample_age | p50 / p99 ≈ **47 / 168 s** |

说明在出口贴顶场景下，观察者侧会出现**长时间收不到新 AOI 坐标**的卡顿；这与服务端仍在大量发送 AOI move 帧、但链路/预算/可见性未改善的现象并存。该 stall 文件时间落在第一次 enforce 窗口附近，**不宜单独当作基线对照样本**，只作质性旁证。

---

## 6. 「两次压测起到了什么效果」——对照表

这里的「两次」指：**基线（关 enforce）** 与 **Enforce 复测（开 enforce，且客户端非空）**。

| 期望效果（验收意图） | 实测结果 | 是否达成 |
|----------------------|----------|----------|
| 打开 D1 强制字节预算后，出站低于 shared-class 85% 帽 | A→B 仍 **~2.60 Mbps 贴 ceil** | **否** |
| `egress_admitted` 随内核写出增长 | 两轮均为 **0** | **否** |
| 超预算时 `budget_defer` 上升、AOI 变稀 | defer **0**，AOI move/s 与基线同量级 | **否** |
| 客户端 move timeout / ack 改善 | timeout **16.8%→19.0%**，ack **82.9%→80.7%** | **否（略差）** |
| 部署 D1/D2 指标并完成 A/B/C + 客户端对照 | 指标暴露、抓取、客户端对齐完成 | **是（证据目标）** |
| 证明「只开开关即可改善 906 人贴顶」 | 数据否定该假设 | **是（否定结论）** |

**一句话：**  
这两次压测的效果，不是「enforce 治好了出口」，而是 **用对齐的服务端 + 客户端数据证明：当前实现下打开 `EGRESS_ENFORCE` 对 2.6 Mbps 贴顶与移动超时没有正面收益，且 admitted/defer 指标未闭环。**

---

## 7. 与代码意图的差距（供下一刀）

设计口径（`docs/perf_audit/egress_enforce_budget.md` / D1）：

- enforce 时取消 due-raise 与 batch_wait；
- 超配额 defer AOI；
- admitted 仅在真实写入确认时累加。

本次验收观察到：

1. 进程环境与启动日志确认 **`egress_enforce=1`**；
2. 出站与 defer/admitted 行为却像 **未在记账/未在限流**；
3. 因此问题优先级应是 **admitted/defer 生产路径为何恒 0**（接线、ledger 实例、FinishMoveCommit 条件、指标 scrape 是否同源），而不是继续同参数空转压测。

未在本轮验证（明确标为未测）：

- C1 外推修复；
- 调高 2.4 Mbps / 改 HTB；
- 打开 `SENDBUF_V2` / `AOI_V2_*` / `TX_COALESCE` 等其它开关。

---

## 8. 原始数据索引

| 类别 | 路径 |
|------|------|
| 基线 tc/metrics | `~/tc_{A,B,C}_baseline.txt`，`~/metrics_{A,B,C}_baseline/` |
| Enforce-1 tc/metrics | `~/tc_{A,B,C}_enforce.txt`，`~/metrics_{A,B,C}_enforce/` |
| Enforce-2 tc/metrics | `~/tc_{A,B,C}_enforce2.txt`，`~/metrics_{A,B,C}_enforce2/` |
| 基线客户端 | `~/client_export_baseline1/`，`docs/client-movement-20261113-224103/`（metrics 实为 221629） |
| Enforce 客户端 | `~/client_export_enforce2/`，`docs/client-movement-20261113-231200/` |
| Stall 旁证 | `docs/client-movement-20261113-224103/aoi-stall-20261113-223440.jsonl` |
| 启动日志 | `run/formal/logs/gw0.log` / `gw1.log`（`egress_enforce=1` 行） |

---

## 9. 建议下一步（不在本文实施）

1. **先修/先查** `egress_admitted` 与 `budget_defer` 为何在 enforce=1 且 AOI 大量发送时仍为 0；修好后再做一轮短 A→B（≥120s）对比 Mbps 与客户端 timeout。  
2. 客户端导出流程固定为：**停 bot 前**导出，并校验 `move_sent>0`。  
3. 在 admitted 闭环前，**不要**把「开 enforce」当作已交付的出口治理能力。
