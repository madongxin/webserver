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
| `ERR_MAP_DATA_MISMATCH` | 地图静态数据 hash 不符 | 否 | 更新地图资源 |
| `ERR_NOT_ON_MAP` | 未进图 | 否 | EnterMap |
| `ERR_STALE_SEQ` | 客户端序号过旧 | 否 | 以服务器 seq 为准 |
| `ERR_MOVE_TOO_FAST` | 移动超速 | 否 | 拉回服务器位置 |
| `ERR_AOI_RESYNC_REQUIRED` | AOI 序号缺口 | 是 | 请求 `WorldSnapshotReq` 后从 baseline 继续 |
| `ERR_SNAPSHOT_TOO_LARGE` | 快照 AOI 超上限 | 否 | 缩小视野或稍后重试 |
| `ERR_PLAYER_DEAD` | HP=0 禁止移动等写命令 | 否 | `RespawnReq` |
| `ERR_MAIL_*` | 邮件子域错误 | 视子码 | 见邮件接口 |
| `ERR_COMMAND_FORBIDDEN` | 公网命令策略拒绝 | 否 | 不要重试该命令 |
| `ERR_INTERNAL` | 未分类内部错误 | 否 | 上报 trace_id |

Gateway 在 Hello/心跳/未登录拒绝路径直接填写顶层码。GameLogic/GameDB 回包由 `PromotePublicError` 提升子响应 `error_code` 并消毒 `message`。
