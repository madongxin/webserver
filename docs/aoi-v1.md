# AOI v1

兴趣范围是服务器权威的格子广播，不是客户端自行发现。

## 格子

- 水平面 X/Z；格子边长 `aoi_cell_size`（模板 1001 为 12m）
- 视野半径 `aoi_view_radius_cells`（默认 2，含自身格）
- 只向 **connected** 实体推送；断线隐藏，不删占位

## 事件

`AoiDelta`（`GameResponse` 字段 62）经可靠/可合并 Push 下发，`message_type=aoi.delta.v1`。

| op | 含义 | 可靠性 |
| --- | --- | --- |
| 1 | ENTER | 可靠 |
| 2 | MOVE | 可合并（coalescable） |
| 3 | LEAVE | 可靠 |

`EntitySnapshot`：`player_id`、`player_name`、`position`、`yaw`、`hp`、`max_hp`、`state_seq`。MOVE 必须带正确 `player_id`、递增 `state_seq` 和新坐标。

进图响应另有 `self` 与 `aoi_snapshot`（当前可见他人，不含自己）。

## Move

`MoveReq` 字段 61。服务器拒绝 NaN/Inf、越界、不可走、超速（`move_speed * 1.25 + 0.5m`）、旧 client seq / stale epoch。确认坐标以 `MoveRsp` 为准。

## 断线与重连

- TCP 断线：`MapRuntime.Disconnect` → 他人 LEAVE；Redis 占位保留至宽限期。
- 宽限期内 `Reconnect` + `BindPlayer`：`MapRuntime.Reconnect` 对可见者再发 ENTER，不创建第二个实体。
- Logout / 宽限期到期：LeaveAll + `ReleaseByPlayer`，他人 LEAVE，槽位释放。

## 限制

一个 `map_instance_id` 只有一个 GameLogic Owner。不是跨 Cell 无缝大世界；不要假设邻接实例连续推送。
