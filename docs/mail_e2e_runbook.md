# 邮件奖励隔离测试说明

正式集群 `scripts/start_formal.sh` 不是这轮的故障注入环境。不要对它停 Redis、停 MySQL、杀进程或发奖。

## 已执行的隔离命令

依赖本机 MySQL，连接信息在 `config/mysql.cnf`，库名 `metrics`。不要把密码写进提交。

```text
cmake -S . -B build
cmake --build build --target gamedb_mail_claim_test mail_unit_test -j 2
env -u GAMEMESH_FORMAL ./build/test/gamedb_mail_claim_test
./build/test/mail_unit_test
```

`GAMEMESH_FORMAL` 必须空着。测试角色使用 `910001` 起的固定数字，道具 `2101` 以及用例里的 `2210` 段。重复运行会在这些角色上累加测试道具，不碰正式玩家。

通过时输出 `OK gamedb_mail_claim_test` 和 `mail_unit_test OK`。日志里的 `ALREADY_CLAIMED` 重复键、`STATE_SYNC_REQUIRED` 是用例本身。

## 还没执行

Gateway、两个 GameLogic、Redis 停机、MySQL 暂停、进程重启、过期扫描并发、outbox 下游重复消费、Unity 双角色都没有跑。没有现成的隔离 compose 被拉起。这些项在报告里是 `NOT_RUN`，不能用“脚本已写”代替。

客户端导出见 `docs/mail_reward_client_handoff.md`。联调地址如果要上测试 Gateway，沿用部署文档里的测试 VIP，不要把 `GAMEMESH_MAIL_DELIVER_TOKEN` 发给客户端。
