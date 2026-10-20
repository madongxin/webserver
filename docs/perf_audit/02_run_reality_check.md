# 02 压测真实性核对（主窗 run_1339）

## 机器人配置 vs 服务器观测

| 声称/面板 | 服务器 OBSERVED | 判定 |
| --- | --- | --- |
| 登录人数 405 + 1 真人 | unique `Gateway login orchestrated` **409**；Acquire/Bind/Enter 同为 **409** | 基本达成；+4 可能为重登/额外号（需机器人侧核对，**NOT_OBSERVABLE**） |
| 登录间隔 40ms → 理论 ~25 login/s | 13:39–13:41 约 **418** 次 login 编排 / 180s ≈ **2.3 login/s**；峰值 1s=**15** | **DERIVED**：实际远低于 25/s（排队、RTT、或客户端门控） |
| Gateway 8081/8083 分流 | 进程累计 login_ok gw0=409 gw1=427（含全日） | 双 GW 有流量；本窗精确分摊见 timeline，未再拆唯一玩家 |
| 在线 20 分钟 | 首批登录 ~13:39:20，集体 Logout **14:01:20** ≈ **22 分钟** | OBSERVED 相符 |
| 编号 0–98 进当前线，99+ 服务器分线 | `EnterMap resolve` **req_line：1→100 次，0→328 次** | OBSERVED：多数已发 `line_no=0`（相对 12:22 轮的 2/3/4 硬指定有改进） |
| 机器人 Move Hz | 无 bot 日志 | **NOT_OBSERVABLE** |
| `scripts/load_game_login_stress.sh` | `login_report.json` status=**BLOCKED** | **未**用该脚本打 EIP |

## 漏斗（人次 / 独立玩家）

来源：`scripts/perf_audit/parse_logs.py` → `data/run_1339/summary.json`（OBSERVED）

| 阶段 | 独立 player_id |
| --- | --- |
| Auth Login ok / AcquireSession | 409 |
| BindPlayer ok | 409 |
| EnterMap reply | 409 |
| EnterMap reply ok=1 次数 | **428**（含重复进图） |
| 窗口内曾 disconnect | 176 |
| 窗口内 logout orchestrated | 250 |

`login_fail_total`（两 Gateway 累计）= **0**；`login_stage_reject_*=0`；窗口内日志 **无** `ERR_OVERLOADED`。

## AOI 实际上限

| 证据 | 值 |
| --- | --- |
| `config/maps/map_manifest.json` 模板 1002 | `aoi_max_visible=200`，`aoi_view_radius_cells=1` | OBSERVED |
| 运行二进制 | 11:47 启动，晚于仓库 AOI200 合入；`(deleted)` 无法 `strings` 反证 50 | **INFERRED 高置信：按 200 配置加载** |
| 运行时日志打印 cap | 无逐次 VisibleIds 上限日志 | 无法用日志再证 |

**不得声称「已用隔离实验验收 AOI=200 延迟」**——仅配置/代码路径推断 cap=200。

## 分线占用（面板曾显示合计 407）

Redis/metrics（压测中后期截图）：5 线合计 occupancy **407**，Gateway/Session ONLINE **404**。  
原因：**分线人数 = `map:occ` 占位**；断线后 Session 先下线，占位宽限期后才释放（见前次核对）。属 **口径差**，不是多登录了 3 个真人。事后复核曾回到 406。

## spawn / 移动有效性（服务器侧）

- 1002 `spawn_scatter_radius=0` → 出生点叠加（配置 OBSERVED）。
- 进程累计（11:47 起，含两次压测）：`ERR_MOVE_TOO_FAST` gl0+gl1≈**183627**；`ERR_UNWALKABLE`≈**685408**；`move_err_stale_seq=0`。  
  → 大量拒绝来自路径/速度，属 **BOT_INVALID_INPUT / EXPECTED_GUARD**，不能直接当 AOI CPU 根因。
- 单窗 Move 接受/拒绝次数：**NOT_OBSERVABLE**（无按时间刮取的 error_code 时序）。

## 是否够找根因？

- **登录路径**：数据足够证明「登录/进图基本成功，瓶颈不在 Auth 失败」。
- **移动/AOI**：有累计扇出与 handle_peak，**缺**锁等待时序、Move P99、bot 发送 Hz → 性能根因只能到 **高度怀疑** 级。
- **机器人端**：无日志 → 重试风暴/Hello 空转只能从 GW 日志侧面看。
