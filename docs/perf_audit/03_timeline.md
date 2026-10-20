# 03 时间线（run_1339，本地墙钟）

数据：`docs/perf_audit/data/run_1339/timeline_10s.csv`（OBSERVED 流式分桶）。

## 阶段标注

| 阶段 | 时间 | 特征 |
| --- | --- | --- |
| warmup | 13:38 | 零星 logout/disc（真人 819 等） |
| ramp | 13:39:20–13:40:30 | login/enter 洪峰；10s 桶峰值 login≈96、enter≈95 |
| peak login | 13:39:30–13:40:20 | 多数角色完成 Acquire→Enter |
| hold | 13:40:30–14:01:00 | 几乎无新 login；持续 `ClientHello`；间断 disconnect；`PushBatch` reject 日志 |
| cooldown | 14:01:20–14:03 | **249 logout + 189 disconnect / 10s**；随后扫尾 disc |

## 关键先后（因果候选）

1. **13:39:20+** 登录与进图同步上升，enter≈login（无进图积压证据）。
2. **13:40:30+** 登录结束；`PushBatch … (stale session/fence)` 日志开始升高（13:41 分钟 reject 消息数峰值 624）。
3. Hold 全程 **Hello 持续**（约 10–60/10s）但 **login=0**：机器人在建连/Hello 却未完成登录 → 侧面 **BOT** 行为或半开连接。
4. **14:01:20** 集体 Logout（与「在线约 20 分钟」一致）→ 非会话 Redis TTL=300 的主因（否则应约 13:44 集体掉）。
5. 窗口内 **idle_timeout 日志极少（3）**；Gateway 累计 idle_timeout 计数含全日，不能直接等同本窗。

## 与 run_1222 对比（同日早盘）

| | 12:22 | 13:39 |
| --- | --- | --- |
| 进图 | 406 全成功 | 409 全成功 |
| 提前断线 | ~180 人在进图后 1–3 分钟 | hold 内缓慢流失 + 14:01 登出 |
| req_line | 大量硬指定 2/3/4 | 多为 0 + 100 次 line=1 |
| 会话 TTL 嫌疑 | 12:28 波次与 5min 重叠 | 本窗集体下线对齐 20min 配置 |

## 时钟

单机日志，无跨主机 NTP 对齐问题。异步日志可能 ±1s，10s 桶足够。
