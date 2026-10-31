# GatewayConnRegistry 单锁基线

结论：这台机器上，1000 和 10000 条连接、4 线程、读写比 9:1 时，P99 从 72 微秒到 84 微秒，吞吐从每秒 33.9 万次到 30.7 万次。没有出现锁把延迟打到毫秒级的情况。按任务约定，**不改** `by_conn_` / `session_to_conn_` / `player_to_conn_` 的分片。

## 机器和构建

| 项 | 值 |
| --- | --- |
| CPU | Intel Xeon Platinum 8255C @ 2.50GHz，4 核 |
| 内存 | MemTotal 3782380 kB |
| 编译器 | g++ 11.2.1 |
| 构建 | Debug，`-O0`，无 ASan/TSan |
| 提交 | `ea20768`（基准程序本身在其后的提交里） |
| 程序 | `test/gateway_conn_bench.cpp` |

写路径用 `ApplyRoute`，`route_version` 固定为 `uint64_max`，避免版本回退打出 `LOG_WARN`。那条日志在锁里面，如果算进去，测到的是写日志，不是哈希表。

## 数字

4 个线程各跑 2 秒。每次操作约 90% 是 `FindByPlayer` 或 `FindBySession`，约 10% 是 `ApplyRoute`。P50/P99 是单次调用耗时，单位微秒。

| 连接数 | 操作次数 | 每秒操作 | P50 | P99 |
| --- | --- | --- | --- | --- |
| 1000 | 678686 | 339343 | 3 us | 72 us |
| 10000 | 613146 | 306573 | 4 us | 84 us |

P99 增加约 17%，吞吐下降约 10%。相对 Move 回包的毫秒级预算，这把锁不是当前瓶颈。

复现：

```bash
./scripts/build.sh Debug gateway_conn_bench
./build/test/gateway_conn_bench
```
