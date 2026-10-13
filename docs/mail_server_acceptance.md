# 邮件 V1 服务端验收

V7 验收编号和旧文档里的 MAIL 编号不是同一套含义。下面矩阵用 2026-10-13 V7 编号。本轮起点 `d47e08c`，远端已 fetch，与本地一致。没有重启集群。

## 本次修改

| 问题 | 修改 |
| --- | --- |
| Redis 不可用时邮件接口放行 | 默认拒绝。只有非正式环境的 `GAMEMESH_MAIL_DEV_SKIP_SESSION=1` 可以跳过。正式环境忽略该开关 |
| 邮件 brpc 的 `MailDeliver` 用收件人会话鉴权 | 改为只校验 `GAMEMESH_MAIL_DELIVER_TOKEN`，并限制 `SYSTEM`/`ACTIVITY`。玩家 token 不能发奖。离线收件不依赖会话 |
| 已读/领取/收藏/批量删除的幂等键未绑定操作意图 | 同一 key 换角色、邮件或操作类型返回 `IDEMPOTENCY_CONFLICT`。投递重复键会比对附件、发送人和到期时间。查询失败返回 `INTERNAL_ERROR`，不会当成未命中再执行 |
| 领取事务把未领附件标成过期 | `DoClaimMail` 对 `UNCLAIMED`/`CLAIMING` 不再写成 `EXPIRED`，仍可领取 |
| 过期扫描把未领附件标成 `EXPIRED`，随后领取被拒绝 | `MarkExpiredBatch` 与 `TouchExpireIfNeeded` 跳过 `UNCLAIMED` / `CLAIMING`。`mail::PreserveAttachmentOnExpire` |
| 正式环境玩家寄信仍开放 | `HandlePlayerMailSend` 在正式环境返回 `ERR_COMMAND_FORBIDDEN`。代码保留，非正式测试仍可跑 |
| 详情附件查询失败仍返回成功 | `HandleMailGet` 在 `LoadAttachments` 失败时返回 `INTERNAL_ERROR`。邮件不存在与查询失败分开 |
| 批量领取把附件查询失败当成可一键领 | 查询失败写入该封 `INTERNAL_ERROR`，不进入领取 |
| 批量删除先记成功再删除 | 软删和操作日志同一事务，提交后才返回成功。回放带 `success_count` 和 `failed_mail_ids` |
| 收藏更新和日志不在同一事务 | 行锁、收藏计数、状态和日志一起提交。计数失败返回 `INTERNAL_ERROR` |
| `mailbox_version` 只在进程内存 | 保持字段，不把它当多实例版本。登录、重连、换角必须拉摘要和列表 |

未改 protobuf 字段号。容量清理本来就排除 `UNCLAIMED` / `CLAIMING`。批量删除未领附件本来就会失败并留下邮件。

## 功能矩阵（V7 编号）

| 编号 | 结果 | 证据 |
| --- | --- | --- |
| MAIL-01 在线离线投递 | 未执行 | 投递写 MySQL，未做重登 |
| MAIL-02 重复业务键 | 通过 | `player_mail_send_test` 同键改附件数量被拒绝 |
| MAIL-03 摘要分页 | 未执行 | |
| MAIL-04 详情查询失败 | 部分 | 不存在返回 `MAIL_NOT_FOUND`；有附件的详情能读到数量。查询失败路径未注入 |
| MAIL-05 已读 | 部分 | 同钥匙换邮件返回 `IDEMPOTENCY_CONFLICT` |
| MAIL-06 收藏切换 | 通过 | `player_mail_send_test` 收藏后详情动作为 `UNFAVORITE`，取消后为 `FAVORITE` |
| MAIL-07 批量删除 | 通过 | 未领附件进入 `failed_mail_ids` 且仍可打开；无附件删除 `success_count=1`，重试相同 |
| MAIL-08 未领不被删 | 通过 | 同上，未领邮件删除后详情仍是 `ACTIVE` |
| MAIL-09 单封领取入账 | 未执行 | 未跑 GameDB 领取集成 |
| MAIL-10 同键不重复发奖 | 未执行 | 代码有领取幂等，本轮未跑并发领取 |
| MAIL-11 批量部分成功 | 未执行 | |
| MAIL-12 背包满 | 未执行 | |
| MAIL-13 事务中断 | 未执行 | 收藏和删除已放进同一事务，未做杀进程 |
| MAIL-14 GameDB 超时 | 未执行 | |
| MAIL-15 过期与领取 | 未执行 | 未领不标 `EXPIRED` 已在代码中，扫描并发未跑 |
| MAIL-16 Redis 故障 | 未执行 | 正式环境默认拒绝，未停 Redis |
| MAIL-17 无令牌不能投递 | 未执行 | brpc 只认 `GAMEMESH_MAIL_DELIVER_TOKEN`，未跑负例 |
| MAIL-18 多实例版本 | 未执行 | `mailbox_version` 仍是进程内存，客户端必须全量拉列表 |
| MAIL-19 重连恢复 | 未执行 | 合同要求重连拉摘要和列表 |
| MAIL-20 双角色隔离 | 未执行 | |
| MAIL-21 推送乱序 | 未执行 | |
| MAIL-22 双端联调 | 未执行 | 协议字段号未改 |

上线判断：可联调。MAIL-09、MAIL-10、MAIL-12、MAIL-14、MAIL-16、MAIL-17、MAIL-20 没有本轮权威资产或故障证据，不能写成正式验收通过。

## 客户端还要做的

- 登录、进大厅、打开邮箱、重连时拉摘要和列表。`mailbox.changed.v1` 只是提示。
- `STATE_SYNC_REQUIRED` 后重拉背包，不要显示“未到账”。
- 正式环境不要调用 `player_mail_send` 或 `mail_deliver`。
- 未领邮件过期后仍可领取，直到策划给出退回规则。

## 测试

`mail_unit_test` 输出 `mail_unit_test OK`。`player_mail_send_test` 输出 `OK player_mail_send_test a=8320 b=8321 mail=28`。覆盖已读钥匙冲突、业务键改附件数量冲突、详情能读到附件、不存在的邮件返回 `MAIL_NOT_FOUND`、收藏后动作变为 `UNFAVORITE`、未领附件删除失败且仍是 `ACTIVE`、无附件删除 `success_count=1` 且重试相同。领取集成、双账号、停 Redis 未跑。
