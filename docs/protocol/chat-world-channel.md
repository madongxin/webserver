# 世界聊天（S3 最小闭环）

范围：**仅世界频道**。不实现私聊、好友、公会。`FriendList` 仍返回 `NOT_IMPLEMENTED`。

## 协议

- 请求：`GameRequest.chat_send = 50`（`ChatSendReq`：`player_id` 由 Gateway 覆盖）
- 应答：`ChatSendRsp` 追加 `message_id=4`、`server_time_ms=5`、`channel=6`
- 推送：`GameResponse.chat_notify = 73`（`ChatNotify`）
- Push `message_type`：`chat.world.v1`
- **不可靠推送**（`reliable=false`）。聊天不是资产事实源；客户端按 `message_id` 去重。

## 校验与限流

| 项 | 值 |
| --- | --- |
| 频道 | 空或 `world`；其它 → `ERR_CHANNEL_FORBIDDEN` |
| 文本 | UTF-8，1–200 码点，且 ≤800 字节；控制符拒绝（`ERR_TEXT_LENGTH` / `ERR_TEXT_CONTROL`） |
| 每玩家 | Redis INCR，默认 5 条 / 2 秒 → `ERR_RATE_LIMITED`（`GAMEMESH_CHAT_PER_PLAYER` / `GAMEMESH_CHAT_WINDOW_SEC`） |
| 每连接 | Gateway `CheckChatRate`：5 条 / 2 秒 |
| 环境 | `GAMEMESH_CHAT_MAX_CP`（默认 200）、`GAMEMESH_CHAT_MAX_BYTES`（默认 800） |

发送方身份来自连接绑定的 `player_id`，不信任客户端自报。

## 广播

GlobalService（`role=world`）按 `SessionStore` 的 ONLINE 集合列出目标，按 `gateway_instance_id` 批量 `PushBatch`，跳过空 Gateway。`MarkDisconnected` / `Logout` 的玩家不在广播集中。

## 非范围

- 私聊 / 好友 / 公会 / 聊天落库 / 敏感词库
- 把聊天当作可回放资产（ReplayStore）
