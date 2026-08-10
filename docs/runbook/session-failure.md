# Session 故障

## 现象

登录/重连失败；Auth/Session brpc 超时。

## 处置

1. 确认双 Session 中幸存者 HTTP `/api/version` 与 ready。
2. Gateway `session_addrs` 应为多地址 list://；杀一实例后应用层会按 peer 重试。
3. Session 无状态：权威数据在 Redis Lua；复活实例重新加入发现即可。
4. 勿把 MarkDisconnected 当 Logout。

演练：`scripts/test_session_failover.sh`。
