# 20 AOI / 地图密度（run_1339）

## 分线与 Logic

| 实例 | 线 | owner（日志/resolve） | Enter 落点次数（reply map=） |
| --- | --- | --- | --- |
| 87 | 1 | gl-0 | 102 |
| 95 | 2 | gl-1 | 104 |
| 96 | 3 | gl-1 | 113 |
| 97 | 4 | gl-0 | 102 |
| 98 | 5 | gl-1 | 7 |

面板峰值合计 occupancy≈407（含占位口径）。同线 **soft=100** 可显示 102/100。

## 空间分布

- `spawn_scatter_radius=0` → **出生点高度叠加**（OBSERVED 配置）。
- `aoi_view_radius_cells=1` → 同格/邻格极密，候选集易顶到 cap。
- 热点 Cell 精确人数时序：**NOT_OBSERVABLE**（无 cell 级指标）。

## 移动有效性

| 项 | 值 | 级别 |
| --- | --- | --- |
| Bot Move sent/s | — | NOT_OBSERVABLE |
| Server 接受/拒绝 Move/s（本窗） | — | NOT_OBSERVABLE（仅累计 error_code） |
| 进程累计 ERR_MOVE_TOO_FAST | ≈183627 (gl0+gl1) | OBSERVED 自 11:47 |
| 进程累计 ERR_UNWALKABLE | ≈685408 | OBSERVED |
| ERR_STALE_SEQ | 0 | OBSERVED |
| aoi_move_events | 1.70M + 0.95M | OBSERVED 累计 |
| push_accepted | 1.80M + 0.99M | OBSERVED 累计 |
| logic handle_peak | 0.201s / 0.169s | OBSERVED 进程峰值 |

**解读**：叠加出生 + 半径 1 → 移动时 AOI 扇出大；大量 UNWALKABLE/TOO_FAST 说明机器人路径/速度与权威不符（**BOT_INVALID_INPUT / EXPECTED_GUARD**）。无法从本窗证明「Move 吞吐先于推送崩溃」。

## 进图快照 vs 移动洪峰

Enter 集中在 13:39–13:40；PushBatch reject 日志从 ~13:40:30 升高。时间上 **先完成进图，再进入移动+推送噪音**（OBSERVED 顺序）。
