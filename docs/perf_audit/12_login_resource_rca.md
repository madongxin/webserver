# 12 登录资源根因（分组）

主窗：**登录成功率高**，资源瓶颈表现在 **移动/推送 hold 段** 与 **机器人行为**，而非登录失败。

## Gateway / 主机

| 观察 | 级别 | 说明 |
| --- | --- | --- |
| login_ok 累计 409+427，login_fail=0 | OBSERVED | 登录未因业务失败 |
| queue_overload=0，login_stage_reject_auth=0 | OBSERVED | Admission 未拒 |
| 本窗无 ERR_OVERLOADED 日志 | OBSERVED | 非限流主导 |
| 双 GW 8081/8083 均有流量 | OBSERVED | 非单 GW 打满（相对 12:22 前单边问题已改善） |
| 主机 4C/3.6G；压测时 CPU | NOT_OBSERVABLE | 无历史采样 |
| FD/重传/softirq | NOT_OBSERVABLE | |

**结论**：登录洪峰（~2–10/s）对当前 Gateway **未表现为失败型瓶颈**。`ERR_OVERLOADED` 本轮 **未** 作为保护触发。

## Auth / Session / Redis

| 观察 | 级别 |
| --- | --- |
| Acquire 409/409 | OBSERVED |
| Redis SLOWLOG LEN=20（事后） | OBSERVED 存在慢日志条目，**不能**证明压测窗 |
| redis_lua_errors 指标 | 需当时 scrape；现刻未作为失败主因 |
| 会话 TTL 机器人 300s | 前次分析；本窗 20min 在线以 Logout 结束，**非** 5min TTL 主杀 |

## GameLogic Bind / GameDB / MySQL

| 观察 | 级别 |
| --- | --- |
| Bind 409 成功 | OBSERVED |
| gamedb 日志有 Load/Ensure 活动 | OBSERVED 存在，无超时风暴模式 |
| MySQL 慢查询/锁等待 | **NOT_OBSERVABLE** → **不得**认定 MySQL 为登录根因 |
| Enter 后 transfer Freeze/Import 205 次（FlushBag transfer_finalize） | OBSERVED | 跨 Logic 进线正常路径 |

## 地图入场

| 观察 | 级别 |
| --- | --- |
| Enter ok=1，开出 5 线（87/95/96/97/98） | OBSERVED |
| soft 100 可超（102 等） | OBSERVED 配置 hard=400 |
| 无 ERR_MAP_NO_LINE / LINE_FULL 日志 | OBSERVED |

## 压测端

| 观察 | 级别 |
| --- | --- |
| 理论 40ms 间隔 vs 实测 ~2.3 login/s | DERIVED | 机器人或链路自限 |
| Hold 期持续 Hello 无 Login | OBSERVED | 半连接/重试环 |
| 机器人 CPU/NAT | NOT_OBSERVABLE | |

## 登录瓶颈 Top 3

1. **无法证明服务器登录层饱和（已证实：无失败/无限流）** — 置信度高。  
2. **机器人到达速率远低于标称 40ms（高度怀疑 bot/RTT）** — 证据：DERIVED RPS。  
3. **Hold 期 Hello 空转（已证实日志现象；根因在 bot 侧无法证明细节）** — OBSERVED hello≫login。
