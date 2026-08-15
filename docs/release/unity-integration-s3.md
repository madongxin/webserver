# Unity 联调批次（S3）

状态：本批服务器端以真实 TCP 双客户端门禁为准，不以“理论可用”收口。未按用户要求前不 commit / tag / push。

## 范围

- 公网：Gateway TCP + `game.proto`（不引入 gRPC、不改 Reactor、不另起登录进程）。
- 玩家资料、公共地图 1001、50 人容量、AOI Enter/Move/Leave、玩家邮件、断线宽限期 Reconnect。
- S3：公开资料 / 在线态 / 世界频道聊天；`FriendList` 仍为 `NOT_IMPLEMENTED`。
- 客户端工程 `luna` 不在本仓库修改。

## 标识

| 项 | 值 |
| --- | --- |
| 基线 | `145a64753aacd0d9e1dc7916edee81d15f183148`（S0–S3 均未提交） |
| 工作区 | dirty，未 commit / tag / push |
| 协议 namespace | `GameMesh.Protocol` |
| `game.proto` SHA-256 | `4c29a73aa7fbed19f122e122bc1832852e593f6bfaca0b7433249391e2ec643d` |
| 地图模板 | 1001 |
| 地图 SHA-256 | `ceef56586c5281dca4ce45340f511d0d577fd724b14131ae5a21d01ea7f41317` |
| 协议导出 | `./scripts/export_unity_protocol.sh` |

## 代码入口

| 能力 | 入口 |
| --- | --- |
| 资料加载 | `GameLogic::BindAuthenticatedPlayer` / `LoadProfileUnlocked`；Login 见 `GatewayLoginOrchestrator` |
| 公开资料 | `GameLogic::HandleGetPlayerBrief`；Formal 走 GameDB `LoadPlayerBrief` |
| 在线态 | `SessionStore::QueryPublicOnlineState`（ONLINE 集合 + session hash） |
| 世界聊天 | World `HandleChatSend` → 按 Gateway `PushBatch`（`chat.world.v1`，`reliable=false`） |
| 占位 50 | `PlacementStore::ReservePublicSlot`；Enter `map_instance_id=0` |
| AOI / Move | `MapRuntime`；推送 `GameLogic::EmitAoi` |
| 玩家邮件 | Gateway → World；Formal 下 GameDB `HandleGameFrame`；MailboxChanged 从 GameDB Push |
| 断线隐藏 | `GameLogicServiceImpl::UnbindPlayer` reason `tcp_disconnect` |
| 重连恢复 AOI | `BindPlayer` → `MapRuntime::Reconnect` |

## 协议

S3 仅追加字段/message，未改已发布编号。无新 SQL 迁移。详见 `docs/release/server-client-foundation-status.md`。

## 脚本

```bash
./scripts/client_ready_gate.sh
./scripts/test_s3_social.sh
./scripts/test_two_player_aoi.sh
./scripts/stable_gate.sh
./scripts/stable_gate.sh --with-e2e
```

## 回滚

- 停集群：`GAMEMESH_RUN_DIR=run/unity-e2e ./scripts/stop_formal.sh`
- 协议：继续使用已导出的 `game.proto` + manifest hash；客户端勿混用未匹配的 C# 生成物
- 不 `git reset --hard` 除非用户明确要求

## 验证（2026-08-15，本工作区）

| 命令 | 退出码 |
| --- | ---: |
| `GAMEMESH_JOBS=2 ./scripts/build.sh Debug` | 0 |
| `./scripts/test.sh unit` | 0 |
| `./scripts/test.sh integration` | 0 |
| `./scripts/client_ready_gate.sh` | 0 `CLIENT READY PASS` |
| `./scripts/test_s3_social.sh` | 0 |
| `./scripts/test_two_player_aoi.sh` | 0（`mailbox_changed=1` `mail_e2e_ok=1`） |
| `./scripts/stable_gate.sh` | 0 `DEV PASS` |
| `./scripts/stable_gate.sh --with-e2e` | 0 `CLIENT READY PASS` |
| `./scripts/stable_gate.sh --full` | **NOT RUN / STABLE BLOCKED**（未跑 30min load / 2h soak；工作区 dirty） |

报告：

- `run/client_ready/summary_1786792728.json`
- `run/stable_gate/summary_1786792900.json`（DEV）
- `run/stable_gate/summary_1786792945.json`（`--with-e2e`）

## 已知限制

- 单 MapInstance 单 GameLogic，非无缝大世界
- GameLogic 宕机需重新进图，不是实时地图迁移
- `FriendList` 仍为 `NOT_IMPLEMENTED`
- 世界聊天不落库、不可靠推送
- Unity `luna` 尚未导入本 SHA 的 `game.proto`
