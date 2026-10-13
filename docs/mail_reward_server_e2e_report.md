# 邮件奖励服务端证据

基线 `e333103`。开工时工作区只有未跟踪的本地草稿，远端 `origin/main` 与该提交一致。本轮改动还没提交时，测试命令跑在这些源码上。没有重启正式集群。

## 命令

```text
cmake -S . -B build
cmake --build build --target gamedb_mail_claim_test mail_unit_test -j 2
env -u GAMEMESH_FORMAL ./build/test/gamedb_mail_claim_test
./build/test/mail_unit_test
```

配置阶段打印的协议摘要是 `49479bbdb5f8f1c5e5a0b649f21bb5c265122b01f4656b18638c87a8f6102792`。实际输出：`OK gamedb_mail_claim_test`、`mail_unit_test OK`。环境是本机 MySQL `metrics`。没有 Gateway，没有 Unity，没有停 Redis。

## 调用链

Gateway 覆盖 `player_id` → `MailService::ClaimOne` → `DoClaimMail`。背包、邮件 `CLAIMED`、附件、OK 操作日志和 outbox 在同一个 MySQL 事务里提交。OK 日志的 `after_state` 用 JSON 记下 `tx`、`asset_version` 和逐条 `grants`。换钥匙或再次查询都读最早一条 `result_code=OK` 的 CLAIM，不再调用 `LoadMeta` 把玩家当前版本当成这封邮件的回执。

## EDGE

| ID | 结果 | 证据 |
| --- | --- | --- |
| EDGE-01 | PASS | 同物品三份 `2000000000` 返回 `INVALID_ATTACHMENT`，背包不变，邮件仍 `UNCLAIMED` |
| EDGE-02 | PASS | 三份 `1431655765` 累加等于 `uint32` 上限，返回 `INVENTORY_FULL`，版本和 outbox 都不变 |
| EDGE-03 | PASS | 两种物品入账；同物品 4+5 合并为 9 且回执保留两条；数量 0 在投递时拒绝；余额 999998 再领 2 失败、再领 1 成功；单封 999999 成功 |
| EDGE-04 | PASS | 领取后再 `GRANT` 其它道具，`MailGet` 的 `claim_tx_id` 和 `asset_version_at_claim` 仍是领取当时的值，当前背包版本更大 |
| EDGE-05 | NOT_RUN | 没有 Unity，不能证明客户端会不会误显示 |
| EDGE-06 | PASS | 连续两封的 `claim_tx_id` 不同，其它发奖后各自回执不变 |
| EDGE-07 | PASS | 换钥匙返回 `ALREADY_CLAIMED`，回执事务号和版本与首次相同 |
| EDGE-08 | NOT_RUN | 同一次命令拒绝了其它角色的 `MailGet`。断线重连和重复 outbox 没有跑 |

## REWARD

| ID | 结果 | 证据 |
| --- | --- | --- |
| REWARD-01 | NOT_RUN | 未通过 Gateway 投递 |
| REWARD-02 | NOT_RUN | 未做离线登录 |
| REWARD-03 | NOT_RUN | 同一次命令核对了背包数量和回执。没有 Unity 画面 |
| REWARD-04 | PASS | 同一把钥匙再领，背包只加一次；回执来自首次 OK 日志 |
| REWARD-05 | PASS | 换钥匙和四线程不同钥匙都只入账一次，回执不变 |
| REWARD-06 | NOT_RUN | 服务端并发已在 REWARD-05 覆盖。没有 Unity 双击 |
| REWARD-07 | PASS | 提交后不改内存，再用原钥匙领取，内存与数据库对齐且回执不变 |
| REWARD-08 | PASS | 内存应用和重载都失败时 `STATE_SYNC_REQUIRED`，放开后同钥匙恢复且不二次入账 |
| REWARD-09 | PASS | 背包满和数量溢出回滚，邮件仍 `UNCLAIMED`。未做提交中途杀进程 |
| REWARD-10 | NOT_RUN | 没有注入未知提交 |
| REWARD-11 | PASS | 同 EDGE-01 至 EDGE-03，以及背包满 |
| REWARD-12 | NOT_RUN | 没有对一整批全部成功的邮件逐封核对 |
| REWARD-13 | PASS | 手动确认邮件返回 `INVALID_STATE` / `manual confirm required`，另一封入账 |
| REWARD-14 | NOT_RUN | 未停 Redis |
| REWARD-15 | NOT_RUN | 未暂停 MySQL 或重启进程 |
| REWARD-16 | NOT_RUN | 未与过期扫描并发 |
| REWARD-17 | NOT_RUN | 未做 Gateway 断线 |
| REWARD-18 | NOT_RUN | 其它角色领取和查询被拒绝。未做双账号会话切换 |
| REWARD-19 | NOT_RUN | 未注入乱序推送 |
| REWARD-20 | NOT_RUN | 未起两个 GameLogic |
| REWARD-21 | PASS | 其它 `player_id` 领取返回 `PERMISSION_DENIED`，查询也不带 grants |
| REWARD-22 | NOT_RUN | 未做 outbox 发布失败后的下游重复消费 |
| REWARD-23 | NOT_RUN | 没有 Unity |
| REWARD-24 | NOT_RUN | 上表有多项 NOT_RUN |

上线判断：不能写成邮件奖励一致性或真实联调已完成。REWARD-03、REWARD-10、REWARD-12、REWARD-17、REWARD-18、REWARD-22、REWARD-23 没有完整证据。

统计：EDGE PASS 6，EDGE NOT_RUN 2。REWARD PASS 8，REWARD NOT_RUN 16，FAIL 0，BLOCKED 0。
