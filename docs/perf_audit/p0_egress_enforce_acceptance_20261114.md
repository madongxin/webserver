# P0 EGRESS_ENFORCE 验收（路由修复后，2026-11-14）

客户端导出：`docs/export-20261114-231805/`（`run_id=client-20261114-231805`）。  
对照：基线 `client-20261113-221629`、空转 enforce `client-20261113-231200`。  
服务端：P0 路由修复（coalescable MOVE → `send_frame`）+ `EGRESS_ENFORCE=1` + `SHARED_CLASS_KBIT=2600`。

---

## 1. 结论（先看）

**P0 阶段 A 通过。** 预算强制第一次在生产指标上闭环，客户端移动超时从 ~17–19% 降到 **~2.7%**，与「感觉好很多」一致。

| 目标 | 门槛 | 本次 | 结果 |
|------|------|------|------|
| G5 指标闭环 | admitted 增长；过载 defer>0 | admitted ~30MB/gw；defer ~200/s | **过** |
| G2 AOI 帧率 | 相对基线降 ≥40% | ~2514 → ~717 /s（约 **-71%**） | **过** |
| G3 自己走路 | timeout ≤10% 或改善 ≥5pp | **2.65%**（基线 16.8%） | **过** |
| G1 出口 | 明显低于 ceil 或 AOI wire&lt;50% | HTB 仍 ~2.6 Mbps；AOI 占 move+reply ~56% | **未过（结构改善）** |
| G4 别人可跟 | sample_age 等 | 本轮无 stall 文件；客户端 `aoi_move_events=631103` | **待补 stall** |

---

## 2. 客户端（硬指标）

同一 Sparse / 905 bot / target 5Hz / duty 100%：

| 指标 | 基线 | 空转 enforce2 | **P0 本轮** |
|------|------|---------------|-------------|
| timeout 率 | **16.8%** | **19.0%** | **2.65%** |
| ack 率 | 83.2% | 81.0% | **97.4%** |
| `move_timeout` 合计 | 50036 | 56046 | **11945** |
| `move_resend` | 45640 | 50565 | **9987** |
| 单 bot `actual_success_hz` p50 | 0.82 | 0.78 | **1.09** |
| success_hz &lt; 0.2 的 bot | 139 | 190 | **32** |
| success_hz == 0 | 10 | 18 | **3** |
| RPC p99 (ms) | 1884 | 1904 | **1839** |
| `aoi_move_events` | 0（导出空） | 0（导出空） | **631103**（~1427/s） |

解读：控制面（MoveRsp）明显止血；重发从 ~15–17% 量级降到 ~2%。p99 仍贴 2s 窗，说明出口仍紧，但多数请求已进窗内。

---

## 3. 服务端（稳态抽样）

路由闭合（累计，进程自 22:11）：

| 指标 | 基线/空转 enforce | P0 |
|------|-------------------|-----|
| `move_offer_total` | 0 | &gt;60 万 / gw |
| `move_flush_total` | 0 | ~17 万 / gw |
| `egress_admitted_bytes_total` | 0 | ~30 MB / gw |
| `aoi_move_budget_defer_total` | 0 | ~9.6 万 / gw |

约 60s A→B（22:55–22:56，online≈906）：

| 项 | 基线量级 | P0 |
|----|----------|-----|
| HTB 1:10 | ~2.60 Mbps 贴 ceil | **~2.60 Mbps 仍贴** |
| 两 gw AOI move 帧/s | ~2514 | **~717** |
| 两 gw move_reply 帧/s | ~770 | **~683** |
| AOI 占 move+reply wire | ~74% | **~56%** |

→ 「少发 AOI、让 MoveRsp」成立；总 Mbps 未降，符合「P0 核心价值是 timeout 而非总吞吐」的决议。

---

## 4. 与空转 enforce 的差别

2026-11-13 仅开开关：admitted/defer=0，timeout 略差。  
本次：先修 **Push 急送旁路**，再开 enforce → 指标与客户端同步改善。  
根因文档：`p0_egress_enforce_rootcause_20261114.md`。

---

## 5. 残留与下一刀

1. **出口仍贴 2.6 Mbps** → G1 未过；靠 P1 外推继续砍 AOI。  
2. RPC p99 仍 ~1.8s → 尾部 bot 仍可能顿挫。  
3. 建议补一份 `aoi-stall` 看 G4（别人 sample_age）。  
4. **默认是否常开 `EGRESS_ENFORCE`**：建议正式压测默认开，或写入 start_formal 可选；需单独评审（当前靠环境变量）。

---

## 6. 决策

| 项 | 决议 |
|----|------|
| 阶段 A / P0 | **通过** |
| 下一阶段 | 评估启动 **P1（C1 + 外推）** |
| 抬带宽 P5 | 仍否 |
