# 公网错误码

客户端只判断 `GameResponse.error_code`（及子响应中的同名稳定码），不解析 `message` 文本。`message` 仅供诊断，且不得包含 MySQL/Redis/brpc 原文。

| error_code | 含义 | retryable | 重试前置 |
| --- | --- | --- | --- |
| `OK` | 成功 | 否 | — |
| `ERR_INVALID_ARGUMENT` | 参数/帧非法 | 否 | 修正请求 |
| `ERR_UNAUTHENTICATED` | 未 Hello 或未绑定身份 | 否 | 先 Hello 再 Register/Login/Reconnect |
| `ERR_FENCE_STALE` | Session fence 过期 | 否 | 用新 token Reconnect |
| `ERR_SESSION_EXPIRED` | 会话不存在或宽限期过 | 否 | 重新 Login |
| `ERR_PROTOCOL_VERSION` | 协议世代不兼容 | 否 | 升级客户端 |
| `ERR_SCHEMA_MISMATCH` | `schema_sha256` 不一致 | 否 | 导入服务器导出的 `game.proto` |
| `ERR_CLIENT_UPGRADE_REQUIRED` | 客户端版本过低 | 否 | 升级客户端 |
| `ERR_RATE_LIMITED` | 连接/帧/心跳/登录限流 | 是 | 等待后按 jitter 重试 |
| `ERR_OVERLOADED` | 队列过载或摘流 | 是 | 退避后重试；摘流则换入口 |
| `ERR_DEPENDENCY_UNAVAILABLE` | Redis/MySQL/brpc 不可用 | 是 | 短暂退避后重试 |
| `ERR_MAP_FULL` | 指定地图实例已满 | 否 | 不要换图；换模板或稍后 |
| `ERR_MAP_LINE_FULL` | 指定分线已满 | 否 | 换线或排队，禁止静默换线 |
| `ERR_MAP_NO_LINE` | 指定线号不存在 | 否 | 刷新线列表 |
| `ERR_MAP_LINE_LIMIT` | 已达最大线数且硬顶已满 | 否 | 换图或稍后 |
| `ERR_MAP_NOT_READY` | 线/实例 FROZEN/RECOVERING/CLOSED | 是 | 退避后重试或快照 |
| `ERR_MAP_DRAINING` | 线正在排空 | 否 | 换线或排队 |
| `ERR_QUEUE_NEEDED` | 需要先取排队票 | 否 | `EnqueueMap` |
| `ERR_QUEUE_INVALID` | 排队票无效或过期 | 否 | 重新 `EnqueueMap` |
| `ERR_QUEUE_NOT_READY` | 未到队首或线仍满 | 是 | 持票轮询后再 `EnterMap(queue_token)` |
| `ERR_DUNGEON_CREATE_FORBIDDEN` | 副本须先 CreateDungeon | 否 | 组队开本 |
| `ERR_DUNGEON_NOT_FOUND` | 副本不存在或已关闭 | 否 | 重新开本 |
| `ERR_DUNGEON_NOT_MEMBER` | 非该副本队员 | 否 | — |
| `ERR_PORTAL_UNKNOWN` | 传送门 ID 不存在或不属于当前图 | 否 | 用 Hello `maps[].portals` |
| `ERR_PORTAL_TOO_FAR` | 不在传送门触发半径内 | 否 | 走近后再 `InteractPortal` |
| `ERR_PORTAL_REQUIRED` | 保留码；2102 公网 `CreateDungeon` 已允许 | 否 | 联调用 `CreateDungeon` + `EnterMap(instance)` |
| `ERR_MAP_DATA_MISMATCH` | 地图静态数据 hash 不符 | 否 | 更新地图资源 |
| `ERR_NOT_ON_MAP` | 未进图 | 否 | EnterMap |
| `ERR_MAP_NOT_LOADED` | 尚未完成 EnterMap 就发送 Move | 否 | 先 EnterMap |
| `ERR_STALE_SEQ` | 客户端序号过旧 | 否 | 以服务器 seq 为准 |
| `ERR_MOVE_TOO_FAST` | 移动超速 | 否 | 拉回服务器位置 |
| `ERR_AOI_RESYNC_REQUIRED` | AOI 序号缺口 | 是 | 请求 `WorldSnapshotReq` 后从 baseline 继续 |
| `ERR_SNAPSHOT_TOO_LARGE` | 快照 AOI 超上限 | 否 | 缩小视野或稍后重试 |
| `ERR_PLAYER_DEAD` | HP=0 禁止移动等写命令 | 否 | `RespawnReq` |
| `ERR_MAIL_*` | 邮件子域错误 | 视子码 | 见邮件接口 |
| `ERR_COMMAND_FORBIDDEN` | 公网命令策略拒绝 | 否 | 不要重试该命令 |
| `ERR_INTERNAL` | 未分类内部错误 | 否 | 上报 trace_id |
| `ERR_BAD_CREDENTIAL` | 账号或密码错误 | 否 | 核对密码后重新 Login |
| `ERR_ACCOUNT_NOT_FOUND` | 账号未注册 | 否 | 先 Register |
| `ERR_BANNED` | 账号已封禁 | 否 | 不可登录 |
| `ERR_PLAYER_NOT_FOUND` | 目标玩家不存在 | 否 | 核对 PlayerID/角色名 |
| `ERR_CANNOT_ADD_SELF` | 不能添加自己 | 否 | — |
| `ERR_ALREADY_FRIEND` | 已经是好友 | 否 | 刷新好友列表 |
| `ERR_REQUEST_ALREADY_SENT` | 已有同向待处理申请 | 否 | 等待对方处理 |
| `ERR_INCOMING_REQUEST_EXISTS` | 对方已向自己发申请 | 否 | 走 Accept/Reject |
| `ERR_REQUEST_NOT_FOUND` | 申请不存在或不是待处理 | 否 | 刷新申请列表 |
| `ERR_REQUEST_EXPIRED` | 申请已过期 | 否 | 重新申请 |
| `ERR_FRIEND_LIMIT` | 自己好友已满 | 否 | 删除好友 |
| `ERR_TARGET_FRIEND_LIMIT` | 对方好友已满 | 否 | 稍后 |
| `ERR_PENDING_LIMIT` | 待处理申请或黑名单已满 | 否 | 清理申请/黑名单 |
| `ERR_ALREADY_BLOCKED` | 已拉黑该玩家 | 否 | Unblock 后再申请 |
| `ERR_NOT_FRIEND` | 当前不是好友 | 否 | 刷新列表 |
| `ERR_OPERATION_TOO_FREQUENT` | 好友申请过于频繁 | 是 | 等待后重试 |
| `ERR_RELATION_CONFLICT` | 关系并发冲突 | 是 | 刷新后重试 |
| `ERR_WRONG_ROUTE` | 好友命令打到了 GameLogic | 否 | 改走 World |

好友终态不可重试：`ERR_ALREADY_FRIEND`、`ERR_NOT_FRIEND`、`ERR_REQUEST_EXPIRED`、`ERR_FRIEND_LIMIT`、`ERR_TARGET_FRIEND_LIMIT`、`ERR_WRONG_ROUTE` 的 `retryable` 为 false。`ERR_OPERATION_TOO_FREQUENT` 与 `ERR_RELATION_CONFLICT` 经 Gateway `PromotePublicError` 之后仍为 true。

好友上限、申请过期和申请频率由环境变量配置，默认好友 100、黑名单 100、待处理 50、过期 7 天、每分钟/小时/天 10/50/200：`GAMEMESH_FRIEND_MAX_COUNT`、`GAMEMESH_FRIEND_BLOCK_MAX`、`GAMEMESH_FRIEND_REQUEST_MAX_PENDING`、`GAMEMESH_FRIEND_REQUEST_EXPIRE_DAYS`、`GAMEMESH_FRIEND_RATE_PER_MINUTE`、`GAMEMESH_FRIEND_RATE_PER_HOUR`、`GAMEMESH_FRIEND_RATE_PER_DAY`。写操作的 `operation_id` 原样作为幂等键，最长 96 字节，空串会被拒绝。

Gateway 在 Hello/心跳/未登录拒绝路径直接填写顶层码。GameLogic/GameDB 回包由 `PromotePublicError` 提升子响应 `error_code` 并消毒 `message`。
