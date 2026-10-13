# 邮件领取凭证联调步骤

正式集群不要用来停 Redis、停 MySQL、杀进程或发奖。本轮跑的是本机测试库。

```text
cmake --build build --target gamedb_mail_claim_test mail_unit_test -j 2
env -u GAMEMESH_FORMAL ./build/test/gamedb_mail_claim_test
./build/test/mail_unit_test
```

`gamedb_mail_claim_test` 使用 `config/mysql.cnf` 指向的库。角色是 `9100xx` 段测试号，道具样例 `2101`。重复运行会在这些号上累加测试道具。

通过时输出 `OK gamedb_mail_claim_test` 和 `mail_unit_test OK`。日志里的 `STATE_SYNC_REQUIRED` 和 `ALREADY_CLAIMED` 重复键是用例本身。

单封对账把 `scripts/mail_claim_reconcile.sql` 里的 `@player_id`、`@mail_id` 换成这次测试的值。`ok_claim_count` 应为 1，`after_state` 里的 `tx` 和 `asset_version` 应与 `MailGet.receipt` 一致。

还没做的部分：Unity、Gateway 双账号、第二个 GameLogic、Redis 失联、MySQL 重启、outbox 下游重复消费。这些不能写成已通过。
