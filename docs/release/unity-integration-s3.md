# Unity 联调批次（S3）

状态：本批服务器端以真实 TCP 双客户端门禁为准，不以“理论可用”收口。未按用户要求前不 commit / tag / push。

## 范围

- 公网：Gateway TCP + `game.proto`（不引入 gRPC、不改 Reactor、不另起登录进程）。
- 玩家资料、公共地图 1001、50 人容量、AOI Enter/Move/Leave、玩家邮件、断线宽限期 Reconnect。
- 客户端工程 `luna` 不在本仓库修改。

## 标识

| 项 | 值 |
| --- | --- |
| 基线 | `37b1977`（S1/S2/S3 均未提交） |
| 工作区 | dirty，未 commit / tag / push |
| 协议 namespace | `GameMesh.Protocol` |
| `game.proto` SHA-256 | `aed5c952a1aa817a13464af8ae05c14d14c19da0ceedd6b61663d2b39f255bcb` |
| descriptor set SHA-256 | `52482ea59d1a64688dbd64035c4e9b8c3a445dbfdfe5b006d5ceb83ac960fa11` |
| 地图模板 | 1001 |
| 地图 SHA-256 | `ceef56586c5281dca4ce45340f511d0d577fd724b14131ae5a21d01ea7f41317` |
| 协议导出 | `./scripts/export_unity_protocol.sh` |

## 代码入口

| 能力 | 入口 |
| --- | --- |
| 资料加载 | `GameLogic::BindAuthenticatedPlayer` / `LoadProfileUnlocked`；Login 见 `GatewayLoginOrchestrator` |
| 占位 50 | `PlacementStore::ReservePublicSlot`；Enter `map_instance_id=0` |
| 地图静态数据 | `MapCatalog` / `MapStaticData` |
| AOI / Move | `MapRuntime`；推送 `GameLogic::EmitAoi` |
| 玩家邮件 | Gateway → World；Formal 下 `GameDbService.HandleGameFrame` → `MailService::HandlePlayerMailSend` |
| 断线隐藏 | `GameLogicServiceImpl::UnbindPlayer` reason `tcp_disconnect` |
| 重连恢复 AOI | `BindPlayer` → `MapRuntime::Reconnect` |

## 协议

本批未改 protobuf 字段编号（S1 已冻结）。无新 SQL 迁移。

## 脚本

```bash
./scripts/run_unity_integration_server.sh
./scripts/test_unity_contract.sh
./scripts/test_two_player_aoi.sh
./scripts/test_map_capacity.sh
./scripts/test_player_mail_e2e.sh
./scripts/stop_unity_integration_server.sh
```

## 回滚

- 停 Unity 联调集群：`./scripts/stop_unity_integration_server.sh`
- 协议：继续使用已导出的 `game.proto` + manifest hash；客户端勿混用未匹配的 C# 生成物
- 地图：保留 sidecar hash；错误 hash 会被 `ERR_MAP_DATA_MISMATCH` 拒绝
- 不 `git reset --hard` 除非用户明确要求

## 验证（2026-08-15）

| 命令 | 退出码 |
| --- | --- |
| `GAMEMESH_JOBS=2 ./scripts/build.sh Debug` | 0 |
| `./scripts/test.sh unit` | 0 |
| `./scripts/test.sh integration` | 0 |
| `./scripts/test_unity_contract.sh` | 0 |
| `./scripts/test_two_player_aoi.sh` | 0 |
| `./scripts/test_map_capacity.sh` | 0（51 人 / 2 实例 / max=45） |
| `./scripts/test_player_mail_e2e.sh` | 0 |
| `./scripts/stable_gate.sh` | 0（`PASS e2e=0 full=0`；`--full` 未跑） |

## 已知限制

- 单 MapInstance 单 GameLogic，非无缝大世界
- GameLogic 宕机需重新进图，不是实时地图迁移
- Formal 下邮件落在 GameDB；`MailboxChanged` 推送在 GameDB 未配 Session 路由时可能缺省，客户端以 MailList 轮询兜底
- `stable_gate.sh --full`（20×E2E / 长 soak）未执行；默认门禁为 Debug 单元+集成
