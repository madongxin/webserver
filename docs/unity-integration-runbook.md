# Unity 联调跑法

对接客户端：Unity 2022.3.62f3c1（仓库 `luna`，本仓库不修改）。外网只连 Gateway TCP。

## 版本组合

| 项 | 值 |
| --- | --- |
| 本批 | Unity 联调切片 S3 |
| 协议 | `proto/game.proto`，`package game`，C# `GameMesh.Protocol` |
| 帧 | `uint32` 大端长度 + protobuf，最大 4 MiB |
| 地图模板 | `1001` / `config/maps/map_1001.json` |
| 容量 | 公共实例 50 人；第 51 人新实例 |
| 宽限期 | Session `session_grace_sec` 默认 45s（断线仍占位） |

导出协议（把输出目录交给 Unity 生成 C#）：

```bash
./scripts/export_unity_protocol.sh /path/to/output_dir
```

`protocol_manifest.json` 含 `game_proto_sha256`。地图 sidecar：`config/maps/map_1001.json.sha256`。

## 启动顺序

1. MySQL、Redis 已按 `scripts/bootstrap_local_config.sh` 配置。
2. `./scripts/check_deps.sh` 与 `./scripts/build.sh Debug`。
3. `./scripts/run_unity_integration_server.sh`  
   默认 `GAMEMESH_RUN_DIR=$ROOT/run/unity-e2e`，拓扑与 `run_e2e_cluster.sh` 相同：Gateway×2、Session/Auth×2、GameLogic×2、World、GameDB×2。`GAMEMESH_FORMAL=1`。
4. 客户端连 **Game TCP**：`E2E_GW0_GAME`（默认 19081）与 `E2E_GW1_GAME`（19083）。HTTP / Push 仅内网。
5. 停止：`./scripts/stop_unity_integration_server.sh`（只杀 PID 文件中的进程）。

本地 formal 默认游戏口仍是 8081/8083（`scripts/start_formal.sh`）。不要把 Unity 工程路径写进服务器脚本。

## 时序

1. **Register** → Auth → GameDB 建号。
2. **Login** → Auth 校验 → Session.AcquireSession → GameLogic.BindPlayer。`LoginRsp.profile` 为权威资料。
3. **GetSelfProfile** 刷新，不得用客户端默认值覆盖。
4. **EnterMap** `map_template_id=1001`，`map_instance_id=0` 进公共池。带 `map_data_version` + `map_data_sha256`；错误哈希返回 `ERR_MAP_DATA_MISMATCH` 及服务器期望值。空哈希兼容旧客户端。
5. **Move** 服务器权威；AOI `aoi.delta.v1`：ENTER=1 MOVE=2 LEAVE=3。
6. **PlayerMailSend** 无附件；在线收件人尽力 `mailbox.changed.v1`，失败则 **MailList / MailGet**。
7. **PushAck** 确认可靠 `server_seq`。
8. **Reconnect**（宽限期内换 Gateway）恢复同一 `map_instance_id` 与 AOI。
9. **Logout** 释放 Session 与地图占位；他人收到 LEAVE。

## 门禁

```bash
./scripts/test_unity_contract.sh
./scripts/test_two_player_aoi.sh
./scripts/test_map_capacity.sh
./scripts/test_player_mail_e2e.sh
```

未执行、超时、缺关键 Push/字段时非零。不要用 `grep PASS || true`。

## 已知限制

- 单个 MapInstance 只有一个 GameLogic Owner，不是跨 Cell 无缝大世界。
- GameLogic 宕机后不是实时无损迁移；客户端按新 Owner/epoch 重新进图。
- Redis / MySQL 仍是基础设施单点。
- 断线宽限期内占位不释放；Logout 才释放。
