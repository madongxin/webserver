# 22 MapRuntime / Push 热点证据

## 锁模型（源码 OBSERVED）

- 全局 `MapRuntime::mu_`：仅注册表（找 map / player_map），Move 热路径短持有。
- **每实例** `InstanceState::mu`：`LockInst` 计量 `aoi_lock_wait_us_sum`（ServerStats 有，**OpsMetrics 未导出** → 等待分布 NOT_OBSERVABLE）。
- 多图不共享同一 instance 锁；同图内 Move 串行于该实例锁。

## 分段耗时

`Mailbox → mutex → validate → cell → neighbor → TopK → diff → PushGrouped`  
除 `gamemesh_logic_handle_peak_seconds`（全 Handle 峰值 0.17–0.20s）外，**分段 P99 均 NOT_OBSERVABLE**。

## AOI 计算

| 指标 | gl-0 | gl-1 | 备注 |
| --- | --- | --- | --- |
| aoi_candidate_sum | 24.1M | 8.0M | 无 count → 均值 NOT_OBSERVABLE |
| aoi_visible_query_us_sum | 8.0e6 µs | 2.8e6 µs | 无 query count 导出 |
| aoi_move_events | 1.70M | 0.95M | |
| aoi_sub_enter/leave | 66k/39k | 25k/13k | |

**INFERRED**：gl-0 线更密/移动更多，候选与 MOVE 事件更高。不能计算「每次 Move 平均候选」。

## 推送 Collapse（压测二进制）

11:47 代码：`DrainAoiQueue` 积压时 **CollapseMoves 只留最新** → 长时间合并成大跳（源码+文档）。  
这与客户端「低于 2–3Hz 一顿一顿」诉求一致，属 **已证实的推送合并策略问题**（性能/体验体验），不依赖无锁数据。

HEAD 已改为 ~100ms 抽样（≥10Hz 目标），**待重启验证**。

## PushBatch

| 项 | 值 | 级别 |
| --- | --- | --- |
| 本窗 reject 原因 | **100% `stale session/fence`**（2270 批） | OBSERVED |
| accept 消息和 | 79448（仅含打了 rejected= 的日志行样本集） | OBSERVED 偏日志 |
| metrics push_rejected | 1136+5069 累计 | OBSERVED 全日 |
| PushBatch RPC RTT | — | NOT_OBSERVABLE |
| 同步 Push 是否占满逻辑线程 | — | NOT_OBSERVABLE；有 offload，但无等待采样 |

**反证**：不能把 stale fence 拒绝当成「Push 性能根因」——多数是断线后正确拒收。

## AOI/移动瓶颈 Top 3

1. **出生点叠加 + view_radius=1 → 扇出与候选放大（高度怀疑密度驱动）** — 配置+事件量证据；缺 cell 密度时序。  
2. **积压 MOVE 合并成大跳（已证实策略，旧二进制）** — GameLogic CollapseMoves。  
3. **机器人 TOO_FAST/UNWALKABLE 刷拒绝（已证实累计）** — 非服务器算力根因，但放大无效负载。
