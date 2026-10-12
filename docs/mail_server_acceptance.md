# 邮件 V1 服务端验收

复查起点是本地 `e472f84`。本轮没有重启集群。领取集成、双账号和停 Redis 未跑。

## 本次修改

| 问题 | 修改 |
| --- | --- |
| Redis 不可用时邮件接口放行 | 默认拒绝。只有非正式环境的 `GAMEMESH_MAIL_DEV_SKIP_SESSION=1` 可以跳过。正式环境忽略该开关 |
| 邮件 brpc 的 `MailDeliver` 用收件人会话鉴权 | 改为只校验 `GAMEMESH_MAIL_DELIVER_TOKEN`，并限制 `SYSTEM`/`ACTIVITY`。玩家 token 不能发奖。离线收件不依赖会话 |
| 已读/领取/收藏/批量删除的幂等键未绑定操作意图 | 同一 key 换角色、邮件或操作类型返回 `IDEMPOTENCY_CONFLICT`。投递重复键会比对附件、发送人和到期时间。查询失败返回 `INTERNAL_ERROR`，不会当成未命中再执行 |
| 领取事务把未领附件标成过期 | `DoClaimMail` 对 `UNCLAIMED`/`CLAIMING` 不再写成 `EXPIRED`，仍可领取 |
| 过期扫描把未领附件标成 `EXPIRED`，随后领取被拒绝 | `MarkExpiredBatch` 与 `TouchExpireIfNeeded` 跳过 `UNCLAIMED` / `CLAIMING`。`mail::PreserveAttachmentOnExpire` |
| 正式环境玩家寄信仍开放 | `HandlePlayerMailSend` 在正式环境返回 `ERR_COMMAND_FORBIDDEN`。代码保留，非正式测试仍可跑 |

未改 protobuf 字段号。容量清理本来就排除 `UNCLAIMED` / `CLAIMING`。批量删除未领附件本来就会失败并留下邮件。

## 功能矩阵

| 编号 | 现状 | 测试 |
| --- | --- | --- |
| MAIL-01 系统投递 | 已有 `MailService::Deliver`，事务写邮件、附件和流水 | 本次未跑 MySQL 集成 |
| MAIL-02 离线可读 | 投递写 MySQL，不依赖在线会话 | 未跑双端 |
| MAIL-03 业务键幂等 | 同键还比对附件、发送人、模板和到期时间；并发插入走同一比较 | 未跑 MySQL |
| MAIL-04 领取幂等 | GameDB 领取键绑定角色、邮件和 `CLAIM`。不一致返回 `IDEMPOTENCY_CONFLICT`，不发奖 | 未跑 |
| MAIL-05 提交后超时 | 已有 `STATE_SYNC_REQUIRED`，不二次发奖 | 未跑故障注入 |
| MAIL-06 依赖故障 | 正式环境会话故障不再放行邮件命令 | 见下方 `mail_unit_test`；Redis 故障注入未跑 |
| MAIL-07 背包满 | 已有 `INVENTORY_FULL`，未领邮件保留 | 未跑 |
| MAIL-08 批量部分成功 | 已有逐封 `MailClaimResult` | 未跑 |
| MAIL-09 未领删除 | 已有：未领进入 `failed_mail_ids` | 未跑 |
| MAIL-10 容量清理 | 已有：清理候选排除未领和领取中 | 未跑 |
| MAIL-11 过期与领取 | 本次改为未领/领取中不标 `EXPIRED` | `mail_unit_test` 只覆盖判定函数；扫描 SQL 未对库执行 |
| MAIL-12 切角色 | 查询按 `receiver_id` | 未跑双角色 |
| MAIL-13 分页与版本 | 已有 cursor；`mailbox_version` 在进程内存，重启不保持 | 未跑 |
| MAIL-14 Redis 失联 | 玩家邮件默认失败关闭。`GAMEMESH_MAIL_DEV_SKIP_SESSION` 只在非正式环境生效 | 未跑停 Redis |
| MAIL-15 收藏与批量 | 收藏键绑定操作；批量删除键绑定邮件 ID 列表 | 未跑 |
| MAIL-16 内部投递权限 | brpc 投递只认 `GAMEMESH_MAIL_DELIVER_TOKEN`。Gateway 仍拒绝玩家 `MailDeliver` | 未跑 brpc 负例 |
| MAIL-17 幂等键冲突 | 已读、领取、收藏、批量删除在 key 换目标时拒绝 | `mail_unit_test` 覆盖意图比较；数据库回放未跑 |
| MAIL-18 重启后版本 | `mailbox_version` 在进程内存，重启后不作为唯一对账依据 | 未跑多实例 |

## 客户端还要做的

- 登录、进大厅、打开邮箱、重连时拉摘要和列表。`mailbox.changed.v1` 只是提示。
- `STATE_SYNC_REQUIRED` 后重拉背包，不要显示“未到账”。
- 正式环境不要调用 `player_mail_send` 或 `mail_deliver`。
- 未领邮件过期后仍可领取，直到策划给出退回规则。

## 测试

`mail_unit_test` 输出 `mail_unit_test OK`。`player_mail_send_test` 输出 `OK player_mail_send_test`，覆盖同一已读钥匙换邮件被拒绝，以及同一业务键改附件数量被拒绝。领取集成、双账号、停 Redis 未跑。
