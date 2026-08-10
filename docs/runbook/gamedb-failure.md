# GameDB 故障

## 现象

资产写超时/未知结果；Outbox 堆积。

## 处置

1. 写路径 `max_retry=0`；超时后先 `QueryOperationResult`，再决定是否换实例重试。
2. 幂等键保证最终资产只变一次。
3. kill gamedb-0 后 gamedb-1 应继续；共享 MySQL 事实源与幂等表。
4. Outbox publisher 需 claim/lock，避免双发。

演练：`scripts/test_gamedb_unknown_result_failover.sh`、`gamedb_unknown_result_test`。
