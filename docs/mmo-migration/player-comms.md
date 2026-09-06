# 跨 GameLogic / 跨线玩家通讯

## 铁律

- **AOI 永不跨 `map_instance_id`。** 同模板不同线 = 不同实例，彼此看不见、收不到 MOVE。
- **一条线只有一个 Owner GameLogic。** 同线玩家不会拆到两台 Logic；不存在「同线跨 Logic 的场景同步」。
- **社交走 World（GlobalService），按 `player_id`，不按线、不按 Logic。**

## 不同 GameLogic 上的玩家怎么通讯

同线不可能分属两台 Logic。跨图 / 未进图 / 登录临时 Bind 不同 Logic 时：

| 能力 | 路径 | 说明 |
|------|------|------|
| 世界聊天 | Client → Gateway → **World** `ChatSend` → `PushBatch` 按 gw | `channel=world` |
| 私聊 | 同上，`channel=whisper\|private` + `target_player_id` | 只推目标 Gateway |
| 邮件 | World → GameDB | 已有 `IsMailBoundRequest` |
| 在线 / 所在线 | `QueryOnlineState`（World） | 回 `state` + `map_instance_id` / `line_no` / `kind` / `gamelogic_instance_id` |
| 查名 | `GetPlayerBrief`（World） | 不含坐标 |

不要在 GameLogic 之间广播 AOI 或 Tick。

## 不同线上的玩家怎么通讯

1. **聊天 / 邮件 / 在线查询**：与上表相同，World 不看线号。
2. **见面 / 同屏**：先 `QueryOnlineState` / `QueryMapLines` / 进图回包 `lines` / Push `map.lines.v1` 得到线号与人数，再发 **`SwitchLine`**（指定线；满员 `ERR_MAP_LINE_FULL`，可 `EnqueueMap`）。
3. **换线成功后** 才进入对方 `map_instance_id` 的 AOI。禁止静默换线。

## 热迁之后

`LiveMigrateMap` 换 Owner 后，旧 Logic 卸图。客户端下一次 Move 若 `NOT_CLAIMED` / 错 Owner，应再发 **`EnterMap`（已占位幂等）** 或 `WorldSnapshot`。社交不受影响。
