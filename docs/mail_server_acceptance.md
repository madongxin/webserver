# 邮件 V1 服务端验收

本地 `main` 在 `efeceb3` 之上有未提交修改。远端 HEAD 本次未 fetch。没有重启集群，没有推送。

## 本次修改

| 问题 | 修改 |
| --- | --- |
| 正式环境 Redis 不可用时，邮件 brpc 和 Gateway 邮件命令直接放行 | `MailBrpcServiceImpl::RequireToken` 与 `MailSessionOrFail`：`GAMEMESH_FORMAL=1` 时返回 `session unavailable`。非正式联调仍放行，避免拆掉无 Redis 的本地测试 |
| 邮件 brpc 的 `MailDeliver` 用收件人会话鉴权，玩家可以给自己投奖励 | 正式环境直接 `ERR_COMMAND_FORBIDDEN`。投递仍走 `MailService::Deliver` 和 GM HTTP |
| 过期扫描把未领附件标成 `EXPIRED`，随后领取被拒绝 | `MarkExpiredBatch` 与 `TouchExpireIfNeeded` 跳过 `UNCLAIMED` / `CLAIMING`。`mail::PreserveAttachmentOnExpire` |
| 正式环境玩家寄信仍开放 | `HandlePlayerMailSend` 在正式环境返回 `ERR_COMMAND_FORBIDDEN`。代码保留，非正式测试仍可跑 |

未改 protobuf 字段号。容量清理本来就排除 `UNCLAIMED` / `CLAIMING`。批量删除未领附件本来就会失败并留下邮件。

## 功能矩阵

| 编号 | 现状 | 测试 |
| --- | --- | --- |
| MAIL-01 系统投递 | 已有 `MailService::Deliver`，事务写邮件、附件和流水 | 本次未跑 MySQL 集成 |
| MAIL-02 离线可读 | 投递写 MySQL，不依赖在线会话 | 未跑双端 |
| MAIL-03 业务键幂等 | 已有：同键同内容命中，内容不同 `IDEMPOTENCY_CONFLICT` | 未跑 |
| MAIL-04 领取幂等 | 已有 GameDB `ClaimMailAttachments` | 未跑 |
| MAIL-05 提交后超时 | 已有 `STATE_SYNC_REQUIRED`，不二次发奖 | 未跑故障注入 |
| MAIL-06 依赖故障 | 正式环境会话故障不再放行邮件命令 | 见下方 `mail_unit_test`；Redis 故障注入未跑 |
| MAIL-07 背包满 | 已有 `INVENTORY_FULL`，未领邮件保留 | 未跑 |
| MAIL-08 批量部分成功 | 已有逐封 `MailClaimResult` | 未跑 |
| MAIL-09 未领删除 | 已有：未领进入 `failed_mail_ids` | 未跑 |
| MAIL-10 容量清理 | 已有：清理候选排除未领和领取中 | 未跑 |
| MAIL-11 过期与领取 | 本次改为未领/领取中不标 `EXPIRED` | `mail_unit_test` 只覆盖判定函数；扫描 SQL 未对库执行 |
| MAIL-12 切角色 | 查询按 `receiver_id` | 未跑双角色 |
| MAIL-13 分页与版本 | 已有 cursor；`mailbox_version` 在进程内存，重启不保持 | 未跑 |
| MAIL-14 Redis 失联 | 正式环境邮件接口失败关闭 | 未跑停 Redis |
| MAIL-15 收藏与批量 | 已有上限与失败列表 | 未跑 |
| MAIL-16 内部投递权限 | 正式环境拒绝玩家 brpc/TCP `MailDeliver` | 未跑 brpc 负例 |

## 客户端还要做的

- 登录、进大厅、打开邮箱、重连时拉摘要和列表。`mailbox.changed.v1` 只是提示。
- `STATE_SYNC_REQUIRED` 后重拉背包，不要显示“未到账”。
- 正式环境不要调用 `player_mail_send` 或 `mail_deliver`。
- 未领邮件过期后仍可领取，直到策划给出退回规则。

## 测试

`cmake --build build --target mail_unit_test gamelogic` 通过。`./build/test/mail_unit_test` 输出 `mail_unit_test OK`。`player_mail_send_test`、领取集成、双账号、停 Redis 本次未跑。
