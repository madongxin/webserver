# DECISIONS — 架构/实施决策日志

## D-M0-001（2026-08-07）阶段 0 范围

- 只建迁移文档与基线脚本，**不改变**客户端协议与业务语义。
- 指标缺口（连接数、QPS、p99 直方图）先登记，不强行改热路径埋点（避免阶段 0 引入行为风险）。

## D-M0-002（2026-08-07）构建入口

- 以仓库根目录 `CMakeLists.txt` 为唯一构建真相源。
- 新增 `scripts/check_deps.sh` / `scripts/build.sh`，不引入第二套构建系统。

## D-M0-003（2026-08-07）术语

- 继续沿用正式方案：`GameLogic` = 场景权威；`World` = 中控（聊天/好友/邮件），不跑 Tick。
- 归档稿 `docs/archive/mmo_distributed_architecture_cpp17_1.md` 中的 “World” 读作 GameLogic，不写入本决策的部署名。

## D-M0-004（2026-08-07）压测

- 阶段 0 压测入口：HTTP `/metrics` 采样 + 现有 `mail_brpc_client`（若 ENABLE_BRPC）说明。
- 专用游戏 TCP 长连接压测器列为缺口，不阻塞阶段 0。

## D-M1-001（2026-08-07）阶段 1 解耦形态

- IO 路径：拆帧 → `InProcessTransport` → `PlayerSerialQueue` → `HandleFrame` → `TcpReplySink`。
- 仍单进程；不引入 brpc 拆进程（留给阶段 2）。
- 串行键：请求体解析出的 `player_id`（`MailDeliver` 用 `receiver_id`）。
- Handler 不直接调用 `TcpConnection::Send`；必须经 `ReplySink` 回投 EventLoop。

## D-M2-001（2026-08-07）阶段 2 双进程

- 内网 `fwd.GameLogicForward.Forward`：payload=GameRequest 体，回包=带长度前缀的外网帧。
- `role=all`：InProcess；`role=gateway`：BrpcTransport；`role=gamelogic`：只听 Forward。
- 多 Logic：`logic_addrs` 逗号列表，`player_id % N` 静态路由；Channel 启动期 Init，`max_retry=0`。
- 本阶段不上 etcd / 地图迁移。

## D-TERM-001（2026-08-07）Persistence → GameDB

- 文档与进程角色统一称 **GameDB**（口语可叫「数据服」），不再使用 Persistence。
- 进程/目录/role 名：`gamedb`；接口名：`IGameDbRepository`（实施阶段 4 时采用）。
- 代码里已有类名如 `PlayerItemPersistQueue` 暂不强制重命名，语义上归属 GameDB。

## D-M7-001（2026-08-07）独立 Session/GameDB + 1B/2B

- **1B**：etcd / NATS / mTLS 均可选；空配置回退静态 `*.cnf` / 本地 outbox；附 kill/gray runbook。
- **2B**：`apps/<role>_main.cpp` → 零前缀多二进制（`gateway`/`gamelogic`/`world`/`session`/`gamedb`）；核心仍在 `runtime/`/`game/`/`db/`；`server role=all|…` 共用 `ServerBootstrap`。

## D-NAME-001（2026-08-07）工程更名 GameMesh

- 产品/框架名：**GameMesh**（原 CppWebServer）。
- 二进制：零前缀角色名；单体入口 `server`。
- API：`RunServer` / `LaunchOpts` / `ParseLaunchArgs`；路径助手 `GameMeshPaths`。
- Prometheus 指标前缀：`gamemesh_*`；etcd 默认 prefix：`gamemesh`。
- Session：进程内仅 Redis + `SessionStore`；Gateway/Logic 配 `session_addrs` 时 RPC，否则本地 Store。
- GameDB：包装 `AsyncMysqlGameDbRepository`；World/Logic 配 `gamedb_addrs` 时 `BrpcGameDbRepository`。
- 不做整仓 `services/<role>/`、不做强制编译依赖 etcd/NATS、不做 Chat/Friend 产品与 AOI/Tick/K8s。

## D-M6-001（2026-08-07）阶段 6 World 中控

- `role=world`：只跑 `WorldForward` + MailService/GameDB；**禁止**战斗 Tick / AOI。
- Gateway 按 `body_case`：`IsWorldBoundRequest`（邮件 30–39 + chat/friend）→ World；其余 → Logic。
- `role=all` 仍 InProcess（回滚路径）；Mail brpc 工具口仅 `all` 保留。
- Chat/Friend 仅模块边界 + `NOT_IMPLEMENTED`，不上产品逻辑。
- 生产化（etcd / mTLS / 杀服演练 runbook）延后至 D-M7。

## D-M5-001（2026-08-07）阶段 5 MapPlacement 骨架

- 不做 AOI/战斗 Tick/完整迁移 Saga；只证 Placement + epoch 写栅栏。
- Placement 进程内表（Gateway / role=all）；多 Gateway 共享 Redis 延后。
- 无地图请求仍 `player_id % N`；进图/MapPing/LeaveMap 按 `owner` → `ChannelForInstance`。
- Logic：`instance_id` 配置；`AcceptWrite(map, epoch)` 拒旧 epoch；EnterMap 走 Claim。
- 哑元场景命令：`MapPing`。

## D-M4-001（2026-08-07）阶段 4 GameDB 垂直切片

- 切片选 **邮件领取**（已有事务/幂等/row_version），不先改 grant/consume。
- 接口：`IGameDbRepository`；实现：`AsyncMysqlGameDbRepository`（worker 池 + 同步等待封装给 `HandleFrame`）。
- SQL 与 outbox 同事务；发布器只打日志并标 `published_at`，不上 NATS。
- `should_apply_memory`：仅首次 commit 成功改内存；幂等命中不二次加背包。
- 进程 role `gamedb` 本阶段不上线（仍嵌在 all/gamelogic）。

## D-M3-001（2026-08-07）阶段 3 会话栅栏

- `token` 即 `fence_token`；Login/Reconnect 后轮换。
- `session_id` 在宽限期内重连不变（同一次 Login 会话）；再 Login 会换新 session_id。
- 断线 → `DISCONNECTED` + `session_grace_sec`；业务 `ValidateToken` 仅接受 `ONLINE`。
- `MarkDisconnected` 必须匹配当前 token+generation，防止旧连接误杀新会话。
- 异设备同时在线：仍可用 `kick_other_device=false` 拒绝；默认客户端应 kick/顶号。

## D-AUTH-001（2026-08-07）登录边界：Gateway 编排 Auth+Session+BindPlayer

- **纠正** `GameLogic → Session.Login`：GameLogic `HandleLogin` 拒绝凭证路径。
- 正确：`Client → Gateway → AuthService.Login → SessionService.AcquireSession → GameLogicService.BindPlayer`。
- Auth 与 Session **同 `session` 二进制**、proto/service 分离（`auth.proto` / `session.proto`）。
- 外网仍用 `game.GameRequest.login`（兼容）；内部路由字段由 Gateway/Session 填写。
- 增加 `GatewayPushService.PushBatch`（内网 `game_port+100`）；Logic 只持 `gateway_instance_id`。
- `IServiceRegistry` + `StaticServiceRegistry` fallback；etcd 仍可选。
- World target 逻辑名 **GlobalService**（本轮不强制改二进制名）。
- 拓扑源：`docs/mmo-migration/topology-auth-session.md`。
