# AOI MOVE 降流量：从原因到方案（评估稿，2026-11-13）

状态：**仅文档，未开工。** 评估通过后再拆任务实施。  
关联实测：`egress_enforce_acceptance_20261113.md`、`client_bots_appear_frozen_20261113.md`、`egress_enforce_budget.md`、`extrapolate_audit.md`。

---

## 1. 要解决什么

### 1.1 业务现象

906 人 Sparse 压测时，客户端观感是「机器人不移动 / 周围人站桩」，自己移动也经常一顿一顿。

### 1.2 数据事实（两轮一致）

| 事实 | 基线（enforce 关） | Enforce 复测 | 含义 |
|------|-------------------|--------------|------|
| HTB `1:10` A→B | **2.60 Mbps 贴 ceil** | **2.61 Mbps 贴 ceil** | 出口容量打满 |
| wire 中 AOI `move` 占比 | **~74%** | **~74%** | 最大开销包种 |
| AOI move 合计 | ~2.92 Mbps 估算 / ~2514 帧/s | ~2.97 Mbps / ~2551 帧/s | 单帧 wire ~145 B |
| move_reply 占比 | ~22% / ~0.87 Mbps | ~22% / ~0.87 Mbps | 次大，但是自己走路回包 |
| 客户端 move timeout | 16.8% | 19.0% | 控制面也被挤 |
| stall `no_aoi` | （旁证文件）**94.8%**，sample_age p50 **46s** | — | 别人坐标断供 |
| `egress_admitted` / `budget_defer` | 0 / 0 | 0 / 0 | 强制预算未闭环 |
| GameLogic `move ok` | 大量 | 大量 | **不是逻辑没仿真** |

单帧成本（代码口径）：事件约 `kAoiMoveEventBytes=52` + 线上头 `kOnWireHeaderBytes=66`；单事件帧 wire ≈ 118–145 B（实测均摊 ~145）。多事件同帧可摊薄头税。

### 1.3 目标（建议验收口径）

在 **不抬默认 2.4/2.6 Mbps 预算**、**不恢复 5cm 过滤**、**不改 2s 移动超时 / 心跳 / 慢客户端 10/30/90** 的前提下：

| 目标 | 建议门槛（评估时可改） |
|------|------------------------|
| G1 出口 | 稳态 A→B HTB 吞吐 **明显低于 ceil**（例如 < 2.2 Mbps）或 AOI move wire 占比 **< 50%** |
| G2 AOI 帧率 | 两 gw 合计 AOI move 帧/s 相对本次基线 **下降 ≥ 40%** |
| G3 自己走路 | 客户端 timeout 率 **≤ 10%** 或至少较基线 **改善 ≥ 5 pp** |
| G4 别人可跟 | stall `sample_age` p50 **≤ 3.5s**（或 `no_aoi` 占比显著下降）；无 stall 时用「观察者可见实体位置更新间隔」替代 |
| G5 指标闭环 | enforce 打开时 `egress_admitted` 随写出增长；超预算时 `budget_defer` **> 0** 且 AOI 变稀 |

---

## 2. 原因分析（因果链）

```
容量根因
  HTB ceil 2.6 Mbps 共用 8081+8083
        │
        ▼
包种结构
  AOI move ≈ 74% wire（~2550 帧/s）
  move_reply ≈ 22%
  enter/leave ≈ 4%
        │
        ├──────────────────────────────┐
        ▼                              ▼
别人：AOI 排队/换帧/迟到              自己：MoveRsp 迟到
  sample_age 数十秒                    RPC p99 ≈ 1.9s → 触 2s timeout
  画面站桩                             巡逻停步 / resend / duplicate_target
        │                              │
        └────────── 观感：机器人不移动 ◄┘
```

### 2.1 根因 R1：出口容量被 AOI 快照刷爆

- 当前默认路径（外推关）：近处约 **200ms** 一轮坐标广播（`FarMoveIntervalMs` / coalesce 默认行为）。
- 906 连接 × 视野内多个运动实体 → 出站需求按应用层估算 **~4 Mbps**，硬帽 **2.6 Mbps** → 永久贴顶 + backlog。
- **权威仍在动**（logic `move ok` 大），冻住发生在 **下行送达**。

### 2.2 根因 R2：发太密且「单实体一帧」头税高

- 实测 avg payload ~79 B/事件，wire ~145 B → **头+成帧开销约占 45%**。
- `batch_wait` 在默认非 enforce 下经常凑不齐多人，一帧 1 个事件 → 每事件都付 66 B 头。
- 文档算术：满员每连接可持续位置事件约 **2.5/s（ceil 2600、85% 余量）**；现在系统在发远高于此的 AOI 总量。

### 2.3 根因 R3：预算「数字在、强制不在」

- `GAMEMESH_EGRESS_BUDGET_MBPS=2.4` 默认只作日志/统计语义时，**不挡住**超期实体发送（历史审计已写清）。
- D1 `EGRESS_ENFORCE=1` 验收：**admitted/defer 全程 0**，AOI 帧率与基线同量级 → **未把 R1 转成「AOI 让路给 move_reply」**。
- 文档解释口径：大量帧停放换帧可能从未 `NoteKernelWrite`；D1 本应补记，但本轮生产指标仍为 0 → **实施/接线仍有缺口，需单独查清后再依赖 enforce**。

### 2.4 根因 R4：外推开关现在不能省流量（H1）

- `GAMEMESH_AOI_EXTRAPOLATE` 默认关；打开后误差用**权威坐标**算、发出的是**呈现坐标** → 匀速/到点仍 `vx≠0` 时 **每 tick drifted**（`extrapolate_audit.md` H1，单测已钉）。
- 因此「开外推少发」在修 C1 **之前**不能当作方案。

### 2.5 放大项（非第一因）

| 项 | 数据 | 作用 |
|----|------|------|
| move timeout → resend | resend 15–17% | 加重上行与回包 |
| `duplicate_target` ~23% | 与超时重发同向 | 逻辑重复劳动 |
| 客户端 AOI 导出为空 | `aoi_move_events=0` | 观测缺口，不改根因 |

### 2.6 明确非目标 / 已排除

| 排除项 | 依据 |
|--------|------|
| 逻辑服没 Tick / 拒绝 MOVE | ok 量大，reject≈0 |
| 纯客户端渲染 bug 为主 | stall 中 `render_stall` 仅 0.8% |
| 恢复 5cm 位移过滤 | 项目硬约束：不做 |
| 未评估就抬高 2.4/2.6 Mbps | 硬约束：需 B1；且不解决「AOI 占比 74%」结构问题 |
| 默认打开 SENDBUF_V2 / READ_BACKPRESSURE / AOI_V2_* / TX_COALESCE / EXTRAPOLATE / ENFORCE | 均需单独验收；本文只作候选 |

---

## 3. 方案总表（供评估勾选）

图例：收益 = 对 AOI wire / 观感的预期；风险 = 语义或回归风险；依赖 = 必须先完成的事。

| ID | 方案 | 预期收益 | 风险 | 工程量 | 依赖 | 建议 |
|----|------|----------|------|--------|------|------|
| **P0** | 查清并修复 admitted / defer 闭环，使 `EGRESS_ENFORCE` 真限 AOI | 高：AOI 让路，move_reply 优先 | 中：限太死则远处更稀 | M | 无（先做） | **先做** |
| **P1** | C1 修外推误差口径 + ENTER 带速度；再开 `AOI_EXTRAPOLATE` | **很高**：匀速可少发一个数量级 | 中：需客户端外推正确 | M | 客户端确认 + 建议 P0 有对照 | **第二优先** |
| **P2** | `AOI_V2_LAYERS` 近密远疏 | 高：远实体降到 1s+ | 中：边界进出抖动 | M | 可见性/滞后策略评估 | 与 P1 可并行评估 |
| **P3** | 提高攒批 / 拉长 coalesce 间隔 / TX 合并 | 中：降帧率+头税 | 中：延迟增加 | S–M | 延迟预算 | 作 P0/P1 补强 |
| **P4** | 降兴趣人数上限 N 或压测参数（cluster/可见） | 高但偏「减负载」 | 高：玩法/压测可比性 | S | 产品同意 | 仅作对照实验 |
| **P5** | 抬 HTB / `EGRESS_BUDGET_MBPS` | 直接减压 | **违当前约束**；掩盖结构问题 | S | **B1 书面批准** | 默认不做 |
| **P6** | SENDBUF_V2 / READ_BACKPRESSURE | 改善排队形态，不保证少发 | 中 | M | 单独开关验收 | 排在 P0–P2 后 |
| **P7** | 恢复 5cm 过滤 | — | **禁止** | — | — | **不做** |

---

## 4. 方案详述

### 4.1 P0 — 让字节预算强制真正作用于 AOI MOVE

**要解决的原因：** R1 + R3。

**机制（设计意图）：**

1. `GAMEMESH_EGRESS_ENFORCE=1` + `SHARED_CLASS_KBIT=2600`（或与现网 tc ceil 对齐）。
2. `Pick`：超连接配额则 **defer AOI**，取消 due-raise / batch_wait 抬额。
3. `move_reply` / 心跳等控制路径 **不按 AOI 配额卡死**（已有 control 分类方向，D2）。
4. `egress_admitted` 仅在内核确认写出后增加；`budget_defer` 在拒发/推迟时增加。

**本次验收缺口（必须先闭合）：**

- 生产抓取中 admitted/defer **恒 0**，同时 `tx_frames{move}` 与 wire 估算在涨。
- 需代码级核对：`SetEnforceByteBudget` 是否在每条连接 ledger 生效；`FinishMoveCommit` / `NoteKernelWrite` 是否走到；指标是否从同一进程暴露；park/换帧路径是否绕过 defer。

**预期效果（算术）：**

- 906 人、ceil 2600、85% 余量 → 每连接约 **290 B/s** 位置额度 ≈ **~2.5 事件/s**。
- 相对当前每连接摊到的 AOI 远高于此 → AOI 帧率应明显下降；HTB 仍可能接近满，但 **move_reply 占比应上升、timeout 应改善**（这是 P0 的核心价值，不只是「总 Mbps 下降」）。

**验收（P0 done）：**

1. enforce=1 时 A→B：`admitted` 单调增；超载时 `defer` > 0。  
2. AOI move 帧/s 较基线下降；move_reply 成功率 / 客户端 timeout 改善或持平偏好。  
3. 文档更新「admitted=0」根因（接线 bug vs 口径误解）。

**风险：** 远处/全体 AOI 更稀 → 需用 stall/`sample_age` 看是否从「完全断」变成「按配额慢更新」。若变成「控制面好了、AOI 仍分钟级断」，必须叠 P1/P2。

---

### 4.2 P1 — 外推少发（C1 + 客户端）

**要解决的原因：** R2（密快照）在匀速场景下的结构浪费。

**必须先修（C1，摘自 extrapolate_audit）：**

1. 漂移误差与 last 记录改为 **呈现坐标**（或「上次实际发出的坐标」），禁止用权威点算 pred 再和权威比。  
2. ENTER / 快照显式带 `vx/vz/move_start_ms`，否则客户端无法外推。  
3. 远层 keyframe：`GAMEMESH_AOI_FAR_KEYFRAME_MS` 默认 10000（评估值）。  
4. **客户端确认支持按速度外推之前，不默认打开。**  
5. 复压看 `gamemesh_aoi_move_emit_reason_total`：`interval`/`drifted` 应下降，`vel_changed`/`keyframe`/`arrival_stop` 成主导。

**预期效果：**

- Sparse 10–30s 匀速段：理论上可从 ~200ms 一发降到 **变速/关键帧才发**。  
- 对 wire：AOI 从 ~74% 有机会降到次要项（取决于视野内变速频率与 keyframe）。  
- 与 P0 叠加：额度内优先发「必须发」的 drifted/vel_changed，而不是均匀刷 interval。

**验收（P1 done）：**

1. H1 单测 + 「20s 匀速呈现推进」事件数测试通过。  
2. `AOI_EXTRAPOLATE=1` 压测：AOI 帧/s、wire 占比下降；`emit_reason` 分布符合预期。  
3. 客户端：匀速他人平滑移动；急转不穿模（抽检 + stall 指标）。

**风险：** 客户端外推不一致 → 鬼畜/拉回；需客户端同学签字。服务端默认仍保持关，用 env 灰度。

---

### 4.3 P2 — 近/远分层（AOI_V2_LAYERS）

**要解决的原因：** R1 下「远近一视同仁 200ms」。

**机制：**

- `GAMEMESH_AOI_V2_LAYERS=1`
- 近：`AOI_V2_NEAR_METERS`（默认 32）保持较高频
- 远：`AOI_V2_FAR_INTERVAL_MS`（默认 1000，可评估 2000–5000）
- 可与外推 keyframe、静默上限一起调

**算术对照（文档旧估算）：** 若远周期拉到数十秒才能单靠分层塞进 2.4 Mbps；故 **P2 单独往往不够**，应与 P0/P1 组合。

**验收：** 近处 timeout/跟手不明显变差；远处更新间隔可控；进出近圈无震荡（必要时 `AOI_V2_HYSTERESIS`）。

**风险：** 与可见集 / leave 语义耦合；需回归 ENTER/LEAVE。默认仍关。

---

### 4.4 P3 — 攒批与间隔（降帧、降头税）

**要解决的原因：** R2 头税与「一实体一帧」。

**候选旋钮（均需列进变更说明，禁止默默改默认）：**

| 旋钮 | 作用 | 副作用 |
|------|------|--------|
| 提高 `batch_min` / 修复 batch_wait 条件 | 多实体同帧 | 单实体延迟上升 |
| 拉长 coalesce / far interval | 直接降频 | 跟手变差 |
| `TX_COALESCE` 类合并 | 降 syscall/帧数 | 需单独验收 |

**预期：** wire 降幅通常 **小于** P1，但对 P0 额度「同一 B/s 打更多实体」有帮助。

**验收：** 同预算下每连接事件/s 上升或同事件/s 下帧/s 下降；延迟直方图可接受。

---

### 4.5 P4 — 减视野 / 改压测密度（对照用）

- 降 AOI 人数上限、加大 `cluster_spacing`、降 bot 数。  
- **能立刻降流量**，但改变的是负载而非发送策略效率。  
- 建议只作为 **对照基线**（回答「是策略问题还是绝对人数问题」），不作为正式服默认手段，除非产品改兴趣范围。

---

### 4.6 P5 — 抬带宽（默认否决）

- 抬 tc ceil 或 `EGRESS_BUDGET_MBPS` 只能推迟打满时刻。  
- 本次结构上 AOI 已占 74%；人数或密度再升仍会打满。  
- **无 B1 批准不做。**

---

### 4.7 P6 — 发送缓冲 / 读反压（后置）

- `SENDBUF_V2`：位置可覆盖、与回包分段 → 改善「过期 AOI 占坑」，**不减少逻辑层 emit 次数**。  
- `READ_BACKPRESSURE`：保护事件循环，间接影响，不直接砍 AOI 产量。  
- 排在 P0–P2 之后，避免一次变更过多开关。

---

## 5. 推荐实施顺序（评估用路线图）

```
阶段 0  证据闭合（已完成大半）
        └ 基线 vs enforce 复测 + 客户端 + stall 文档

阶段 A  P0  admitted/defer 修复 + enforce 复压（短 A→B ≥120s）
        └ 门禁：G5 + timeout 有改善趋势

阶段 B  P1  C1 外推修复（默认仍关）+ 客户端联调
        └ 门禁：单测 H1/呈现推进；灰度 EXTRAPOLATE=1 达 G1/G2

阶段 C  P2/P3  分层与攒批微调（在 A+B 数据上选型）
        └ 门禁：G3/G4；远近体验可接受

阶段 D  （可选）P6 缓冲类；P4 仅对照；P5 仅 B1
```

**原则：**

1. **先让「少发/让路」的机制在指标上可见**（P0），再追求「聪明少发」（P1）。  
2. 每一阶段 **单独提交、单独开关、单独 A/B/C + 客户端导出**。  
3. 不默认打开任何实验旗；正式默认值变更需单独评审。

---

## 6. 每阶段共用复测清单

1. 环境：`GAMEMESH_ADVERTISE_HOST=10.0.0.2`；记录全部相关 `GAMEMESH_*`。  
2. 服务端：A（稳态）→B（≥120s）→C（停测）；`tc 1:10` + gw/logic `/metrics`。  
3. 必看指标：  
   - `tx_frames_by_frame{move|move_reply}`  
   - `egress_wire_bytes_estimate`  
   - `egress_admitted_bytes_total`、`aoi_move_budget_defer_total`  
   - `aoi_move_emit_reason_total`（P1 后）  
   - logic `move_requests_total`  
4. 客户端：停 bot **前**导出；校验 `move_sent>0`；保留 bots csv。  
5. 可选：`aoi-stall-*.jsonl` 看 `no_aoi` / `sample_age`。  
6. 禁止：本机改 tc/sysctl（由约定操作者执行）；禁止恢复 5cm。

---

## 7. 决策清单（评审填写）

| # | 问题 | 选项 | 决议（空） |
|---|------|------|------------|
| 1 | 是否批准阶段 A（P0 admitted/defer）为下一刀？ | 是 / 否 | |
| 2 | P0 目标更看重「timeout 降」还是「总 Mbps 降」？ | timeout / Mbps / 两者 | |
| 3 | 客户端外推是否已具备联调条件？ | 已具备 / 排期中 / 不具备 | |
| 4 | 是否允许阶段 B 合并 C1 + 灰度 EXTRAPOLATE？ | 是 / 否 / 仅 C1 不开开关 | |
| 5 | P2 远间隔初始值？ | 1000 / 2000 / 5000 / 其它 | |
| 6 | 是否允许压测对照降 N / 改 cluster？ | 是 / 否 | |
| 7 | 带宽上调（P5）？ | 否 / 走 B1 | |
| 8 | 文档与提交作者/分支约定是否沿用现网流程？ | 是 / 说明 | |

---

## 8. 成功时系统应呈现的形态（概念）

| 层级 | 期望行为 |
|------|----------|
| 逻辑 | 仍按玩法接受 MOVE；duplicate 因超时下降而下降 |
| 发送策略 | 匀速少发（外推）；远疏近密；超配额 defer AOI |
| 出口 | AOI 不再长期占 74%；move_reply 及时 |
| 客户端 | 自己走路超时明显下降；他人靠外推滑动 + 低频校正，而不是数十秒冻帧 |

---

## 9. 附录：关键数字速查

| 项 | 值 |
|----|-----|
| 实测 AOI move wire 占比 | ~74% |
| 实测 AOI 帧/s（两 gw） | ~2510–2550 |
| 单帧 wire | ~145 B |
| move_reply 占比 | ~22% |
| HTB ceil | 2600 Kbit |
| 预算 85% 整机 | 276250 B/s |
| 每 gw @2 实例 | 138125 B/s |
| 每连接 @906 | ~305 B/s（×0.95≈290） |
| 约可支撑位置事件/连接/s | ~2.5 |
| 客户端 timeout 基线 / enforce2 | 16.8% / 19.0% |
| stall no_aoi | 94.8% |
| sample_age p50 | 46.5 s |

---

## 10. 文档维护

- 评估结论写入本节「决策清单」或评审纪要后，再开实施任务。  
- 实施时每完成一阶段，回链更新 `egress_enforce_acceptance_*.md` 式复盘，避免口口相传。
