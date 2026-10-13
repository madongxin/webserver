# 邮件领取凭证验收

基线 `09dc21f`。开工时远端 `origin/main` 与本地一致。测试在本轮源码上执行，提交前的 SHA 仍是 `09dc21f`。没有重启正式集群。本仓库没有 Unity 工程。

上一轮在当前代码里的状态：`MailClaimReceipt` 已挂在详情和领取结果上，首次 OK 日志保存事务号、版本和 grants，换钥匙不再用当前 `LoadMeta` 版本冒充。仍缺的是回包线格式往返、失同步时回执仍在、以及旧日志不得显示成已核对到账。这三项已补上并在下面的命令里跑过。

```text
cmake --build build --target gamedb_mail_claim_test mail_unit_test -j 2
env -u GAMEMESH_FORMAL ./build/test/gamedb_mail_claim_test
./build/test/mail_unit_test
```

输出：`OK gamedb_mail_claim_test`、`mail_unit_test OK`。环境是本机 MySQL。没有 Gateway 进程，没有 Unity。

| 编号 | 结果 | 证据 |
| --- | --- | --- |
| MAIL-E2E-01 | PASS | 字段号 5/6 与回执 1–5 被断言。`GameResponse` 单领、批领、详情序列化后能解析回同一 `claim_tx_id`。缺回执的旧字节 `verifiable=false`。Unity 目录不在本仓库，没有做另一份 proto 的 diff |
| MAIL-E2E-02 | NOT_RUN | 服务端入账在同一次命令里发生。没有 Unity 背包画面 |
| MAIL-E2E-03 | PASS | 领取后再发其它道具，`MailGet` 仍是首次 `claim_tx_id` 和 `asset_version_at_claim` |
| MAIL-E2E-04 | PASS | 两封批量领取的事务号不同，各自 OK 日志一行。含手动确认的批量仍是逐封结果 |
| MAIL-E2E-05 | PASS | 同一把钥匙再领，背包只加一次，OK 日志仍为一行 |
| MAIL-E2E-06 | PASS | 换钥匙返回 `ALREADY_CLAIMED`，回执与首次相同 |
| MAIL-E2E-07 | PASS | 未领邮件 `verifiable=false`。旧日志被改成没有 grants 后，详情是 `UNKNOWN` |
| MAIL-E2E-08 | PASS | 其它 `GRANT` 之后当前背包版本更大，该封回执不变 |
| MAIL-E2E-09 | NOT_RUN | 服务端会在 `STATE_SYNC_REQUIRED` 里带上已入账回执。没有客户端“待确认”界面 |
| MAIL-E2E-10 | PASS | 内存同步失败时响应仍带可解析回执；同钥匙恢复后 `MailGet` 的事务号相同，背包不二次增加 |
| MAIL-E2E-11 | PASS | 溢出和背包满回滚，邮件仍是 `UNCLAIMED`，没有可验证回执 |
| MAIL-E2E-12 | PASS | 同 MAIL-E2E-10 的恢复段：权威背包数量与数据库一致，回执不变 |
| MAIL-E2E-13 | NOT_RUN | 没有离线登录 |
| MAIL-E2E-14 | PASS | 其它角色领取和 `MailGet` 被拒绝，响应里没有 grants |
| MAIL-E2E-15 | NOT_RUN | 没有切角色或重连旧回包 |
| MAIL-E2E-16 | NOT_RUN | 未停 Redis，未重启 MySQL |
| MAIL-E2E-17 | NOT_RUN | 未起第二个 GameLogic |
| MAIL-E2E-18 | NOT_RUN | 领取事务里有一行 outbox。没有下游重复消费 |
| MAIL-E2E-19 | NOT_RUN | 这次没有跑玩家令牌投递或停 Redis |
| MAIL-E2E-20 | NOT_RUN | 没有 Unity 和 Gateway 双账号 |
| MAIL-E2E-21 | PASS | 删掉 grants 的旧 OK 日志查询为 `UNKNOWN`，换钥匙不再发奖 |
| MAIL-E2E-22 | NOT_RUN | 上表有多项 NOT_RUN |

功能已实现：领取、详情和批量回包都会带上首次入账回执；没有凭证时是 `UNKNOWN`。自动测试已通过：上面两条命令。真实双端联调未通过。

统计：PASS 12，NOT_RUN 10，FAIL 0，BLOCKED 0。不满足正式上线验收。阻断项包括 MAIL-E2E-02、09、13、15、16、17、20。
