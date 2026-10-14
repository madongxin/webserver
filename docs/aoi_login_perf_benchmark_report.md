# AOI 与登录压测报告

日期：2026-10-14。机器：4 核，内存约 3.6GB（`free` 显示 3.6G，available 约 1.4G）。正式集群 pid 文件存在，本报告没有对它发登录或移动流量。

没有优化前的同口径微基准。下面的毫秒数只描述 **订阅修复之后** 的进程内循环，不能写成“提升了多少倍”。

## 进程内 AOI 微基准

命令：`./build/test/aoi_interest_bench`

每组把 N 个实体放进一张开图（格子 32，视野半径 1 格），再对 1 号实体做 200 次 `Move`（忽略速度校验）。事件数是这 200 次产生的 ENTER/MOVE/LEAVE 总数。

| 实例数 | 每实例人数 | 200 次移动的事件数 | 耗时 |
| --- | --- | --- | --- |
| 1 | 100 | 7856 | 1563.97 ms |
| 1 | 200 | 7918 | 4182.51 ms |
| 1 | 400 | 7758 | 12879.43 ms |
| 1 | 1000 | 7758 | 20411.80 ms |
| 4 | 100 | 7856 | 1899.47 ms |

事件数在 100 人之后不再随人数上涨，订阅上限生效。耗时仍随人数上涨，因为候选扫描和全局锁还在。4 实例那一行移动的是 1 号实例，耗时接近单实例 100 人，不能当成 8 核或多实例扩展结论。

没有 P50/P95/P99、没有网卡、没有 MySQL/Redis 延迟。这些字段是 NOT_RUN。

## 正确性命令

- `./build/test/map_runtime_aoi_test` 退出码 0。日志里有一条既有用例的 `ERR_STALE_SEQ` WARN，测试本身通过。
- `./build/test/aoi_subscription_invariant_test` 退出码 0，AOI-01..08 均为 PASS。
- `./build/test/login_admission_test` 退出码 0。预算 4、重连保留 1 时，新登录拿到 3 个名额，重连拿到最后 1 个。
- `cmake --build build --target gateway session gamelogic` 均已链接通过。正在运行的正式进程仍是旧二进制，需要重启后才会跑这版 AOI 和登录预算。本轮没有重启。

## 登录脚本

`scripts/load_game_login_stress.sh --users 2 --gateway-endpoints 127.0.0.1:8081` 因 `run/formal/pids` 存在而退出 2，状态 BLOCKED。没有发出 TCP 连接。

带 `124.222.244.169` 的地址同样退出 2。

## AOI-01 .. AOI-08

| 编号 | 结果 | 证据 |
| --- | --- | --- |
| AOI-01 | PASS | 60 人叠点，玩家 1 的快照为 50 且不含 60，也没有收到 60 的 ENTER |
| AOI-02 | PASS | 视野外的玩家收不到 MOVE |
| AOI-03 | PASS | 同格按距离保留更近的人，最远 id 不在快照里 |
| AOI-04 | PASS | 重复 Enter 只有一条实体 |
| AOI-05 | PASS | Leave 之后不再收到 MOVE |
| AOI-06 | PASS | 跨图后旧图无实体，旧邻居的 MOVE 不再送到新图 |
| AOI-07 | PASS | 断线 LEAVE，重连快照含邻居，之后 MOVE 恢复 |
| AOI-08 | PASS | 同距离按 player_id，两次快照一致，52 不在玩家 1 的 50 人里 |

## LOAD-01 .. LOAD-12

| 编号 | 结果 | 原因 |
| --- | --- | --- |
| LOAD-01 | NOT_RUN | 需要隔离集群上的小规模 TCP 登录。正式集群在跑，脚本拒绝执行 |
| LOAD-02 | NOT_RUN | 同上，没有 100 人进图+移动的隔离环境 |
| LOAD-03 | NOT_RUN | 3000 连接。本机 4 核 / 3.6GB，且正式进程占用同一端口 |
| LOAD-04 | NOT_RUN | 10000 连接 |
| LOAD-05 | NOT_RUN | 30000 连接 |
| LOAD-06 | NOT_RUN | 多 Gateway 分摊未测 |
| LOAD-07 | NOT_RUN | 依赖超时与补偿未在隔离环境注入 |
| LOAD-08 | NOT_RUN | Redis/MySQL 故障未注入 |
| LOAD-09 | NOT_RUN | Session/Bind/EnterMap 分段超时未测 |
| LOAD-10 | NOT_RUN | 乱序 Move 与断线重建只在 MapRuntime 单测覆盖了一部分，没有真实客户端长稳 |
| LOAD-11 | NOT_RUN | NAT 与恶意 IP 混合未测。每 IP 80/2s 限制仍在 |
| LOAD-12 | NOT_RUN | 长稳与重连风暴未测 |

没有测得的量：最大稳定 CCU、稳定登录 TPS、同图在线密度、排队等待、到达 RPS 与准入 RPS。进程内 1000 实体微基准不是这些数。
