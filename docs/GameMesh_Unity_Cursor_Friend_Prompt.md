# Unity / Luna：MMO 好友系统 Cursor 提示词

把本文件整份贴进 **客户端（luna）仓库** 的 Cursor 对话，直接执行。不要改服务器仓库。不要 commit / push，除非用户明确要求。

服务器协议事实源：`webserver/docs/protocol/export/`（`game.proto` + `game.desc` + `protocol_manifest.json`）。客户端必须导入这一份，禁止用旧 schema 或反向覆盖服务器 proto。

---

## 你的角色

你在改 Unity 客户端。目标：登录进图后可用好友系统：搜索、发申请、同意/拒绝、列表、删除、黑名单；并能处理实时 Push。

## 不可违反

1. 外网只连 Gateway TCP VIP。不要连 Session / World / GameLogic / GameDB。不要引入 gRPC。
2. `package game`，C# namespace `GameMesh.Protocol`。Hello 必须带当前 `schema_sha256`，否则 `ERR_SCHEMA_MISMATCH`。
3. **不要在请求里自报操作人身份当权威。** 可以填 `player_id`，Gateway 会覆盖为 Session 绑定 id。填错会被拒 `ERR_UNAUTHENTICATED`。
4. 所有好友 RPC 都是 **World 路由**。进图、换线、进副本不影响好友关系。不要把好友状态绑在当前 `map_instance_id` / GameLogic 上。
5. 错误码只认 `GameResponse.error_code` 与子响应同名字段。不要解析 `message` 当 UI 文案。
6. `MarkDisconnected` ≠ 登出。好友在线在重连保护期内应继续显示在线，不要拿 TCP 是否断开当权威。

---

## 先同步协议

1. 从服务器拿到最新 `docs/protocol/export/`：`game.proto`、`game.desc`、`protocol_manifest.json` 及 sha256。
2. 重新生成 C# protobuf（`csharp_namespace = GameMesh.Protocol`）。
3. Hello 校验 `schema_sha256` 必须与 `ServerHelloRsp.schema_sha256` 一致。
4. 确认生成代码里有：

| 方向 | 消息 | GameRequest/Response field |
| --- | --- | --- |
| 列表 | `FriendListReq/Rsp` | req **51** / rsp **51** |
| 搜索 | `FriendSearchReq/Rsp` | req **81** / rsp **94** |
| 申请 | `FriendApplyReq/Rsp` | **82** / **82** |
| 同意 | `FriendAcceptReq/Rsp` | **83** / **83** |
| 拒绝 | `FriendRejectReq/Rsp` | **84** / **84** |
| 删除 | `FriendDeleteReq/Rsp` | **85** / **85** |
| 申请列表 | `FriendRequestListReq/Rsp` | **86** / **86** |
| 拉黑 | `FriendBlockReq/Rsp` | **87** / **87** |
| 解除 | `FriendUnblockReq/Rsp` | **88** / **88** |
| 黑名单 | `FriendBlockListReq/Rsp` | **89** / **89** |
| Push | `FriendRequestPush` | rsp **90** |
| Push | `FriendAddedPush` | rsp **91** |
| Push | `FriendRemovedPush` | rsp **92** |
| Push | `FriendPresencePush` | rsp **93** |

C# 字段名：`FriendAcceptRsp.Peer`、`FriendAddedPush.Peer`（C++ 关键字 `friend` 不能做字段名）。

---

## 数据结构

`FriendBrief`：`player_id, name, level, profession, avatar, online, last_online_time, map_name, remark`。

`remark` 只属于当前玩家视角，不要展示给对方。`map_name` 可能为空（隐私）。不要展示 `session_id` / `gateway_id` / `gamelogic_instance_id` / `fence_token`。

`FriendRelationState`：

- `NONE` 可申请
- `FRIEND` 已是好友
- `SENT_PENDING` 已发出申请
- `RECEIVED_PENDING` 对方已向我申请 → 引导到申请列表 Accept，不要再 Apply
- `BLOCKED_BY_SELF` 我拉黑了对方

**没有** “对方拉黑我” 状态。Apply 若目标拉黑自己，服务器仍可能返回成功（隐私）。不要用错误码探测谁拉黑了你。

上限（服务端默认）：好友 100、黑名单 100、待处理申请 50。申请约 7 天过期。

---

## 必须实现的 RPC 流程

登录成功（有 Session）后即可发好友命令，不强制先 EnterMap。

### 1. 打开好友面板 → `FriendList`

- `cursor` 首次空；`page_size` 可 100。MVP 一次即可。
- 客户端排序：在线优先；离线按 `last_online_time` 倒序。服务器不保证排序。
- `online==true` 或 Push 说在线即可显示在线。重连保护期服务器可能仍报在线。

### 2. 搜索 → `FriendSearch`

- 精确 `target_player_id` **或** 精确 `exact_name`（二选一，id 优先）。
- 用返回的 `relation` 决定按钮：申请 / 同意 / 已是好友 / 已拉黑。

### 3. 发申请 → `FriendApply`

- 填 `target_player_id`（搜索结果）。可选 `exact_name`。
- **必须带 `operation_id`**（例如 `apply:{playerId}:{targetId}:{uuid}`），超时重试用同一个，禁止换新 id 连点。
- 成功看 `request_id`。`request_id==0` 且 `ok` 也可能是隐私成功，UI 当“已提交”。
- `ERR_INCOMING_REQUEST_EXISTS`：打开申请列表让玩家 Accept。
- `ERR_OPERATION_TOO_FREQUENT`：可重试，做冷却。

### 4. 申请列表 → `FriendRequestList`

- 默认 `page_size=20`，用 `next_cursor` 翻页。
- 离线期间的申请也能在这里看到。进游戏时拉一次。

### 5. 同意 / 拒绝

- `FriendAcceptReq.request_id` / `FriendRejectReq.request_id` 来自列表或 `FriendRequestPush`。
- 同样带稳定 `operation_id`。
- Accept 成功用 `FriendAcceptRsp.peer` 立刻插入好友列表。对方走 Push。

### 6. 删除 → `FriendDelete`

- `friend_player_id`。双方关系同时解除。默认不要弹“你被删了”强提醒；收到 `FriendRemovedPush` 只从 UI 移除。

### 7. 黑名单 P2（一起做完）

- `FriendBlock`：会删好友并关掉相关申请。单向。
- `FriendUnblock` 后才能再申请。
- `FriendBlockList` 展示我拉黑的人。

---

## Push（必须接）

好友 Push 走现有可靠/非可靠信封：`ServerPushEnvelope` 或内层 `GameResponse`。`payload` 是内层 `GameResponse`。

| message_type | 可靠 | 内层 body | 客户端 |
| --- | --- | --- | --- |
| `friend.request.v1` | 是 | `FriendRequestPush` | 红点 + 申请列表插入；重连会补发 |
| `friend.added.v1` | 是 | `FriendAddedPush` | 插入好友列表 |
| `friend.removed.v1` | 是 | `FriendRemovedPush` | 从列表移除 |
| `friend.presence.v1` | **否** | `FriendPresencePush` | 只更新当前在线；**不要 replay 历史上下线** |

重连后：

1. 处理可靠 Push 补发（申请/加好友/删除）。
2. **重新拉 `FriendList`** 校正在线状态。不要把 presence 历史当队列回放。

解析顺序与聊天相同：外层 `has_friend_request_push` 或 `InnerFromPush` 后再看 body。

---

## 错误码（本地化表，服务端不给中文 UI）

`ERR_PLAYER_NOT_FOUND` `ERR_CANNOT_ADD_SELF` `ERR_ALREADY_FRIEND` `ERR_REQUEST_ALREADY_SENT` `ERR_INCOMING_REQUEST_EXISTS` `ERR_REQUEST_NOT_FOUND` `ERR_REQUEST_EXPIRED` `ERR_FRIEND_LIMIT` `ERR_TARGET_FRIEND_LIMIT` `ERR_PENDING_LIMIT` `ERR_ALREADY_BLOCKED` `ERR_NOT_FRIEND` `ERR_OPERATION_TOO_FREQUENT` `ERR_RELATION_CONFLICT` `ERR_DEPENDENCY_UNAVAILABLE` `ERR_UNAUTHENTICATED`

`ERR_RELATION_CONFLICT` / `ERR_DEPENDENCY_UNAVAILABLE` / `ERR_OPERATION_TOO_FREQUENT` 可重试；先刷新列表。

---

## UI 建议（MVP）

1. 好友面板：列表 + 搜索框（角色名或数字 PlayerID）+ 申请页签 + 黑名单页签。
2. 申请页签显示 `applicant.name`、时间、同意/拒绝。
3. 在线绿点；离线显示最近在线时间（`last_online_time` unix 秒，0 则显示“未知”）。
4. 不要做亲密度、分组、礼物、推荐。

## 联调

1. 两个账号登录同一 VIP。
2. A Search B → Apply；B 在线应立刻收到 `friend.request.v1`。
3. B 离线时 A Apply；B 登录后 `FriendRequestList` 能看到。
4. B Accept；A 收到 `friend.added.v1`，双方 `FriendList` 都有对方。
5. 同一 `operation_id` 重试 Apply/Accept 不得出现双申请/双关系。
6. Delete 后双方列表都没对方。
7. A Block B 后再 Apply 应失败或表现为已拉黑（A 视角）；B 再向 A Apply 时 B 可能仍看到成功。
8. 换图/换线后好友列表与在线状态仍正确。
