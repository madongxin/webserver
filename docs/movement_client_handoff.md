# 给 Unity 机器人的移动对接说明

服务器不实现五种走法。客户端自己发 Login、EnterMap、Move。不要往 Move 包里加 `run_id`。用测试时间窗对账。

协议没有为这次压测加字段。Hello 的 `schema_sha256` 必须等于当前服务器 `docs/protocol/export/protocol_manifest.json`，否则 `ERR_SCHEMA_MISMATCH`。

## 运行参数（客户端自己遵守）

```yaml
run_id: luna-move-v1
seed: 20261021
bot_count: 600
login_rate_per_sec: 20
movement_target_hz: 5
move_jitter_pct: 10
max_inflight_move_per_bot: 1
aoi_visible_cap_expected: 200
```

600 × 90% × 5 Hz = 2700 是客户端计划发送量，不是服务器承诺处理量。

## 地图 1002

| 项 | 值 |
| --- | --- |
| 模板 | 1002，LINE，场景 TerrainDemoScene |
| 实例 | 以 EnterMap / Reconnect 回包里的 `map_instance_id` 为准。模板相同或线号相同都不表示同一张图 |
| soft / hard / 线数 | 100 / 400 / 最多 8 |
| 选线 | `line_no=0` 由服务器选人少的未满软顶线。指定线满员是 `ERR_MAP_LINE_FULL`，不会自动换线 |
| 单线人数 | 最多 400。第 401 人被拒。600 人必然在多条线上，线与线之间没有 AOI |
| 总容量 | 8 × 400 = 3200。10000 人进不了 1002 |
| 出生点 | `(179.3, -18.687, 324.2)`，另有 `spawn_scatter_radius=4`，不要假设所有人叠在同一点 |
| 范围 | X/Z `[-1992, 1992]`，Y `[-38, 536]` |
| AOI | 格子 32 m，视野半径 1 格。每个观察者最多看见 **200 个其他玩家**，自己不算。看不见的人不是丢包 |
| 导航格 | 8 m。不超过速度的落点即使格子标成不可走也会被接受。不要为了贴格把步长缩到 0 |

## Move

- 每机器人最多 1 个未完成 Move。收到 `MoveRsp` 再发下一步。
- `move_speed` 按 10 m/s。一步长度不要超过 `10 * dt`。`dt` 用这次与上次的 `client_time_ms` 差，单位毫秒。
- 5 Hz 是目标。服务器不按 2/5/10 Hz 拒包。10 Hz 只要每步仍不超过 `speed * dt` 也会接受。
- 超速会被缩短。缩短后仍不能落到可走处才是 `ERR_MOVE_TOO_FAST`。
- `MoveRsp.ok=true`：权威坐标、yaw、`state_seq`、`server_time_ms` 已提交。`state_seq` 比上一份大。这 **不是** 「别人已经看见」。
- `MoveRsp.ok=false`：仍要采用回包里的 position / yaw / state_seq，不要用本地坐标重发。
- 常见拒绝：`ERR_INVALID_ARG`、`ERR_MAP_MISMATCH`、`ERR_STALE_EPOCH`、`ERR_MAP_NOT_LOADED`、`ERR_PLAYER_DEAD`、`ERR_NOT_ON_MAP`、`ERR_DISCONNECTED`、`ERR_STALE_SEQ`、`ERR_OUT_OF_BOUNDS`、`ERR_MOVE_TOO_FAST`。
- `client_seq` 必须比该玩家上一包更大，否则 `ERR_STALE_SEQ`。
- 没进图就 Move：`ERR_MAP_NOT_LOADED`。Reconnect 若 `need_enter_map=true`，先 EnterMap。

别人看见你的位置来自 AOI op=2，同一观察者、同一实体大约 200 ms 合并一次，窗内只留更大的 `state_seq`。所以远端最高大约 5 Hz，即使你自己的 MoveRsp 更快。合并帧带 `server_time_ms`。

## 出站

服务器每连接只保留一帧最新移动。内核里还有超过 8 KB 没送出时，新位置先不写进内核，避免 MoveRsp 排在旧坐标后面。客户端应持续读 TCP，不要等某一个序号时停止读其它帧。读得慢时，人会跳到最新点，而不是把积压的旧点慢慢播完。

## 对账指标

监控页 `http://127.0.0.1:8094/monitor`（内网）。按实例看，不要加总成一个 max。

- 计划发送 vs Gateway `rate(gamemesh_gateway_rx_frames_total)`：入站含心跳，大于纯 Move。
- 别人是否还在收到位置：`rate(gamemesh_move_flush_total)`。
- 是否在等内核排空：`rate(gamemesh_move_hold_total)` 与 `gamemesh_send_pending_max_bytes`。
- 是否暂时不再入队：`rate(gamemesh_move_drop_total)`。
- 在线：Session 的 `gamemesh_session_online_players`，Gateway 的 `gamemesh_gateway_tcp_connections`。分线占位可能高于在线，看 `gamemesh_ghost_occupancy`。

没有 Move 受理次数 counter，也没有 P95/P99。不要把 ACK 次数写成 AOI 完成次数。正式集群在重启之前，这些新计数不在正在跑的进程里。
