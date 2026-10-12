# 邮件协议合同（V1）

供 Unity 客户端对接。协议以 `proto/game.proto` 为准，本文件不新增字段号。

- `schema_sha256`：`47e6fbf688af61b31ad4d5e2ecf88a17e9891521ad9950bb79076456a87a8d1a`（`sha256sum proto/game.proto`）
- 时间：`server_time_utc`、`sent_at`、`expire_at`、`read_at` 都是 Unix 秒。
- 身份：Gateway 用已认证连接覆盖 `player_id`。客户端填写的角色 ID 不是权威。
- 收件人：V1 只有 `ROLE`。附件只有 `ITEM`。分类 `SYSTEM`、`ACTIVITY` 可投递；`SOCIAL`、`TRADE` 只是预留展示。
- 关联：请求与响应的 `GameResponse.seq` 等于请求 `seq`。领取必须带稳定 `idempotency_key`。

## Gateway 消息号

`GameRequest` / `GameResponse` oneof 编号相同。

| 业务 | 字段 | 号 |
| --- | --- | --- |
| 摘要 | `mailbox_summary` | 30 |
| 列表 | `mail_list` | 31 |
| 详情 | `mail_get` | 32 |
| 已读 | `mail_read` | 33 |
| 单封领取 | `mail_claim` | 34 |
| 批量领取 | `mail_batch_claim` | 35 |
| 收藏 | `mail_favorite` | 36 |
| 批量已读 | `mail_batch_read` | 37 |
| 批量删除 | `mail_batch_delete` | 38 |
| 内部投递 | `mail_deliver` | 39 |
| 玩家寄信 | `player_mail_send` | 63 |
| 邮箱变更通知 | `mailbox_changed` | 64 |

`mail_deliver` 不是客户端命令。正式环境 Gateway 与邮件 brpc 都拒绝它。系统奖励走进程内 `MailService::Deliver` 或已登录的 GM HTTP `/api/gm/mail/deliver`。

`player_mail_send` 在正式环境（`GAMEMESH_FORMAL=1`）返回 `ERR_COMMAND_FORBIDDEN`。V1 不开放玩家寄信。

## 状态

- 阅读：`UNREAD` / `READ`
- 可见：`ACTIVE` / `SOFT_DELETED` / `EXPIRED` / `REVOKED`
- 附件：`NONE` / `UNCLAIMED` / `CLAIMING` / `CLAIMED` / `PARTIAL` / `RETURNED` / `DESTROYED`

按钮以 `MailGetRsp.mail.allowed_actions` 为准。未领附件的删除会进入 `failed_mail_ids`，不会删掉道具。

未领（`UNCLAIMED`）或领取中（`CLAIMING`）的邮件到期后仍保持 `ACTIVE`，玩家可以继续领取。无附件或已领完的邮件才会被标成 `EXPIRED`。退回和补偿流程还没有产品规则。

## 错误码

`OK`、`MAIL_NOT_FOUND`、`MAIL_EXPIRED`、`MAIL_REVOKED`、`ALREADY_CLAIMED`、`CLAIM_IN_PROGRESS`、`INVENTORY_FULL`、`CURRENCY_CAP`、`INVALID_ATTACHMENT`、`IDEMPOTENCY_CONFLICT`、`MAILBOX_FULL`、`RATE_LIMITED`、`ASSET_TRANSACTION_FAILED`、`PERMISSION_DENIED`、`INVALID_STATE`、`FAVORITE_LIMIT`、`HAS_UNCLAIMED_ATTACHMENT`、`INTERNAL_ERROR`、`INVALID_ARGUMENT`、`SERVER_STOPPING`、`STATE_SYNC_REQUIRED`、`SESSION_INVALID`、`ERR_COMMAND_FORBIDDEN`。

`STATE_SYNC_REQUIRED` 表示数据库可能已经入账，内存背包没对齐。客户端必须按权威背包重拉，不能把它当成“肯定没发奖”。同一 `idempotency_key` 重试不得生成第二份道具。

## 版本与推送

`mailbox_version` 存在 GameLogic 进程内存，重启后从 1 重新计。客户端以摘要和列表的服务端结果收敛，不要把版本倒退当成丢信。

在线时可能收到可靠推送 `mailbox.changed.v1`（`MailboxChangedNotify`：`player_id`、`mailbox_version`、`unread_count`）。推送丢失不影响数据库。登录、进大厅、打开邮箱、重连后都要再拉 `MailboxSummary`。

容量、过期天数、分页和收藏上限来自 `mail.cnf`，默认值见 `game/MailConfig.h`，不要写死在 UI。临期 72 小时是策划建议，用 `server_time_utc` 与 `expire_at` 比较。
