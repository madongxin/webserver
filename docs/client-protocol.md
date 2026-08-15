# 客户端协议（Unity / luna）

外网只连 Gateway TCP。本仓库 `proto/game.proto` 是唯一事实源。

## 导出

```bash
./scripts/export_unity_protocol.sh /path/to/output_dir
```

输出：

- `game.proto`
- `game.desc`（protobuf descriptor set）
- `protocol_manifest.json`（Git SHA、帧格式、schema SHA-256）
- `game.proto.sha256` / `game.desc.sha256`

C# namespace：`GameMesh.Protocol`（`package game` 不变）。不要把 Unity 工程路径写进服务器脚本。

## 帧格式

`4 字节 uint32 大端 payload 长度 + GameRequest / GameResponse protobuf`。

- 单帧最大 **4 MiB**
- 长度 0 或超限：立即断开
- 响应用外层 `GameResponse.seq` 关联请求
- Push 外层 `seq=0`；可靠顺序用 `ServerPushEnvelope.server_seq`

Push 外层永远是 `GameResponse.server_push`。`payload` 是**内层** `GameResponse` 的序列化结果。

固定 `message_type`：

| type | 内层 body |
| --- | --- |
| `aoi.delta.v1` | `AoiDelta` |
| `mailbox.changed.v1` | `MailboxChangedNotify` |
| `player.state.v1` | 预留（属性/HP 变化） |

## 登录快照

`LoginRsp.profile`（字段 9）是服务器权威资料。客户端不得在登录后覆盖名字或战斗属性。

刷新可用 `GetSelfProfile`（oneof 60）。

## 玩家邮件

公网用 `PlayerMailSend`（oneof 63）。`MailDeliver` 在正式模式仍禁止。

- 无附件
- `sender_player_id` 由 Gateway 覆盖
- 标题 1–64 Unicode，正文 1–1000 Unicode
- `operation_id` 幂等
- 在线收件人尽力推送 `mailbox.changed.v1`（仅 version/unread）；失败不回滚

## 地图数据（S2 才加载；请 Unity 按此导出）

服务器**不**读取 `.unity`、Prefab、FBX、NavMesh 二进制。需要 Editor 导出 JSON，例如 `maps/1001.grid.json`，并提供独立 SHA-256。

建议字段：

```json
{
  "schema_version": 1,
  "map_template_id": 1001,
  "scene_name": "MainScene",
  "data_version": 1,
  "bounds_min": [0, 0, 0],
  "bounds_max": [100, 20, 100],
  "aoi_cell_size": 12.0,
  "nav_sample_step": 1.0,
  "grid_width": 100,
  "grid_height": 100,
  "walkable_rle": [1, 20, 0, 3],
  "spawn_points": [{"id":"default","position":[1,2,3],"yaw":0}]
}
```

约定：

- Unity 水平面 **X/Z**，**Y 为高度**；服务器不交换轴
- `col = floor((x - min_x) / nav_sample_step)`
- `row = floor((z - min_z) / nav_sample_step)`
- `walkable_rle` 为 `[value, count, ...]`（0=不可走，1=可走）
- AOI 格大小与 walkable 采样步长是两个概念
- 至少一个合法出生点（在边界内且可走）

`EnterMap` 携带客户端地图 `data_version` / SHA-256（字段 5/6）；不匹配返回 `ERR_MAP_DATA_MISMATCH` 及服务器期望版本/哈希。`map_instance_id=0` 加入 50 人公共池；指定实例满员返回 `ERR_MAP_FULL`，不静默换图。`operation_id`（字段 7）用于占位幂等。

地图文件：`config/maps/map_1001.json` + `.sha256`。客户端必须发送同一 SHA-256。

## Move / AOI

协议字段已冻结（`MoveReq` 61，`AoiDelta` 62）。服务器权威位置：拒绝 NaN/Inf、越界、不可走、超速、旧 seq/epoch。AOI Push `aoi.delta.v1`（ENTER/LEAVE 可靠，MOVE 可合并）。

水平面为 Unity **X/Z**，**Y 为高度**。公共模板 **1001** 默认出生点 `(-28.5, -0.244, -7.25)`，yaw `76.022`。详见 `docs/map-data-v1.md`、`docs/aoi-v1.md`。

收到可靠 Push 后发送 `PushAck`（oneof 52），用 `server_seq` 裁剪 Gateway ReplayStore。外层响应用请求 `seq` 匹配；Push 外层 `seq=0`。

## Logout / Reconnect

- `LogoutReq` 必须带当前 `player_id` + fence `token`；Gateway 编排 Unbind(`logout`) + Session.Logout。Logout 释放地图占位并广播 AOI Leave。
- TCP 断线只 `MarkDisconnected` / Unbind(`tcp_disconnect`)：占位保留至宽限期（默认 45s），AOI 对他人隐藏。宽限期内另一 Gateway `Reconnect` 可恢复同一实例与 AOI，不产生重复实体。
- `MarkDisconnected ≠ Logout`。

## 联调入口

真实公网路径客户端：`build/test/game_tcp_e2e_client`。脚本：

```bash
./scripts/run_unity_integration_server.sh
./scripts/test_unity_contract.sh
./scripts/test_two_player_aoi.sh
./scripts/test_map_capacity.sh
./scripts/test_player_mail_e2e.sh
./scripts/stop_unity_integration_server.sh
```

跑法与端口：`docs/unity-integration-runbook.md`。
