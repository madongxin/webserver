# 邮件奖励服务端证据

基线 `3941f6d`。工作区在改这些文件之前是干净的，远端 `origin/main` 与该提交一致。没有重启正式集群，没有对线上角色发奖。

环境：本机 MySQL `metrics`，`gamedb_mail_claim_test` 直接连 GameDB 资产表。未启动 Gateway、未跑 Unity、未停 Redis、未杀进程。

命令：

```text
cmake --build build --target gamedb_mail_claim_test asset_dirty_consistency_test mail_unit_test -j 2
env -u GAMEMESH_FORMAL ./build/test/gamedb_mail_claim_test
./build/test/asset_dirty_consistency_test
./build/test/mail_unit_test
```

实际输出：`OK gamedb_mail_claim_test`、`OK asset_dirty_consistency_test`、`mail_unit_test OK`。日志里的 `ALREADY_CLAIMED` 重复键和 `STATE_SYNC_REQUIRED` 是用例故意打出的。

## 调用链

Gateway 用已认证会话覆盖 `player_id` → `MailService::ClaimOne` / 异步领取 → `AsyncMysqlGameDbRepository::DoClaimMail` → `GameDbAssetStore::GrantItemsOnConnection`。邮件状态、附件、幂等日志、outbox 和背包在同一个 MySQL 连接事务里提交。成功后 `ApplyClaimMemory` 按 `asset_version` 更新 GameLogic。版本不连续或 grant 为空时从 `player_asset_bag` 重载。`WorldSnapshotReq` 在内存标脏时先重载；重载失败则快照返回 `STATE_SYNC_REQUIRED`。

## REWARD

| ID | 结果 | 证据 |
| --- | --- | --- |
| REWARD-01 | NOT_RUN | 未通过 Gateway 给在线角色投递 |
| REWARD-02 | NOT_RUN | 未做离线收件后登录 |
| REWARD-03 | PASS | 用例 A：领取后 `player_asset_bag` +5，`asset_version` +1，outbox 一行 |
| REWARD-04 | PASS | 同一把钥匙再领，背包和版本不变，outbox 仍为一行 |
| REWARD-05 | PASS | 用例 G 换钥匙得到 `ALREADY_CLAIMED`；用例 H 四线程不同钥匙，背包只加一次 |
| REWARD-06 | PASS | 用例 D 八线程同一把钥匙，背包只加一次。没有客户端双击 |
| REWARD-07 | PASS | 用例 B/J：提交后不更新内存，再用原钥匙领取，内存与数据库对齐且不重复加 |
| REWARD-08 | PASS | 用例 F：内存应用和重载都失败时返回 `STATE_SYNC_REQUIRED`，背包已 +3；放开后同钥匙恢复且不二次入账 |
| REWARD-09 | PASS | 用例 K 背包满、用例 L 数量溢出，都回滚，邮件仍是 `UNCLAIMED`。未做提交中途杀进程 |
| REWARD-10 | NOT_RUN | 提交结果未知只在代码里按幂等日志回读，没有注入未知提交 |
| REWARD-11 | PASS | 同 K/L。非法附件在入账前拒绝 |
| REWARD-12 | NOT_RUN | 没有对一整批可领邮件做逐道具对账。用例 B2 只是连续单领 |
| REWARD-13 | PASS | 用例 M：手动确认邮件返回 `INVALID_STATE` / `manual confirm required`，另一封入账，背包只加可领的那一封 |
| REWARD-14 | NOT_RUN | 未停 Redis |
| REWARD-15 | NOT_RUN | 未暂停 MySQL 或重启进程 |
| REWARD-16 | NOT_RUN | 未与过期扫描并发 |
| REWARD-17 | NOT_RUN | 未做 Gateway 断线重连 |
| REWARD-18 | NOT_RUN | 用例 I 只证明服务端函数拒绝其它 `player_id`。未做双账号会话切换 |
| REWARD-19 | NOT_RUN | 未注入乱序资产事件 |
| REWARD-20 | NOT_RUN | 未起两个 GameLogic |
| REWARD-21 | PASS | 用例 I：其它角色领取返回 `PERMISSION_DENIED`，收件人背包不变。未从客户端伪造 Gateway 包 |
| REWARD-22 | NOT_RUN | outbox 与领取同事务写入。未做发布失败后的下游重复消费 |
| REWARD-23 | NOT_RUN | 本机没有 Unity |
| REWARD-24 | NOT_RUN | 上表有多项 NOT_RUN |

上线判断：不能写成奖励一致性或真实联调已完成。REWARD-10、REWARD-12、REWARD-14 至 REWARD-20、REWARD-22、REWARD-23 没有本轮证据。

统计：PASS 10，FAIL 0，BLOCKED 0，NOT_RUN 14。
