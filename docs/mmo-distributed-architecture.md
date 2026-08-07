# GameMesh 分布式架构改造方案（正式基线）

> **地位：** 后续所有架构改造以本文为准。  
> **版本：** v1.0  
> **日期：** 2026-08-06  
> **输入材料：** `docs/archive/mmo_distributed_architecture_cpp17_gamelogic.md`（历史任务书，仅作参考）  
> **范围：** 本文只定义方案与阶段；**不要求立刻改代码**。实施时按阶段逐个垂直切片推进。

---

## 1. 文档目的与使用方式

1. 结合仓库真实能力，评估 ChatGPT 方案哪些可采纳、哪些需裁剪或延后。
2. 给出可落地的目标拓扑、服务边界、技术选型与分阶段路线。
3. 后续改架构、拆进程、加 RPC、做重连/分片时，**对照本文验收**，不以口头约定为准。
4. ChatGPT 原文中的「立即编码 / 工作包自动连跑 / 强制脚本交付」**不作为当前强制指令**；实施节奏由人工按阶段启动。

---

## 2. 仓库现状审计（事实）

### 2.1 进程与入口

| 项 | 现状 |
|----|------|
| 主进程 | 基本单进程：`test/http_server.cpp` 同时拉起 HTTP、可选 Game TCP、可选 brpc |
| 游戏长连接 | `GameTcpGateway`：独立线程 + `EventLoop` / `TcpServer`（master-slave Reactor） |
| 业务入口 | `OnMessage` → `ProtoFraming` → `GameService::HandleFrame` → `GameLogic::Handle` |
| 协议 | 外网：`uint32` 大端长度 + `game::GameRequest/GameResponse`（上限 4MB） |
| 会话 | `SessionStore` + Redis（token / online）；无 Redis 时部分路径放行 |
| 持久化 | MySQL `ConnectionPool`；道具异步队列 `PlayerItemPersistQueue`；邮件 `MailStore` |
| 内网 RPC | 可选 `ENABLE_BRPC`：`MailBrpcService` / `BrpcGameServer`（默认 OFF） |
| 本地 KV | 可选 RocksDB Demo（`ENABLE_ROCKSDB`） |
| 监控 | `/metrics` Prometheus 文本 + `LogicMetrics` 等 |

### 2.2 与 ChatGPT 假设的差异（必须纠正）

| ChatGPT 假设 | 仓库事实 | 对本方案影响 |
|--------------|----------|--------------|
| 工程已是 C++17 | 全局 `-std=c++14`；RocksDB/brpc 部分 target 单独开 C++17 | **渐进升到 C++17**，不一次性全仓强切 |
| 已有地图/AOI/Tick | **无** `map_template` / `map_instance` / AOI / 场景 Tick | Map 相关放中后期；前期用 **player 路由** |
| DB 以 PostgreSQL 为例 | **MySQL** + `ConnectionPool` | 保留 MySQL，不引入 PG |
| 需立刻上 etcd / NATS / OTel / K8s | 无 etcd、无 NATS；已有 Prometheus 风格 metrics | **延后**；早期用 Redis 注册 + 现有 metrics |
| Actor Runtime 已就绪 | 业务在 IO 线程同步 `Handle`，`GameLogic` 全局 `mutex` | 先拆线程/进程，再引入最小 Actor |
| 目录形如 `server/src/...` | 扁平：`tcp/` `game/` `db/` `redis/` `base/` | **不推倒重命名整仓**；新模块渐进引入 |
| GameLogic 已是通用地图池 | 当前是「道具/技能/登录/邮件」单例逻辑 | 先做成 **可多实例同构进程**，地图池随玩法再上 |

### 2.3 现有阻塞点（分布式前必须正视）

1. **IO 线程跑业务**：`GameLogic::Handle`、部分 MySQL/Redis 访问发生在连接所属 EventLoop 上。
2. **网关=逻辑**：`GameTcpGateway` 与 `GameLogic` 同机同进程，无法水平扩展逻辑。
3. **登录嵌在玩法**：`HandleLogin*` 与背包/邮件同处 `GameLogic`。
4. **连接≈会话**：断线即失绑；尚无 `fence_token` / 任意 Gateway 重连。
5. **无地图权威模型**：尚无 `map_instance_id → owner`，后续上大世界前必须补齐。

---

## 3. 对 ChatGPT 方案的可行性结论

### 3.1 建议采纳（核心原则，长期有效）

| 原则 | 说明 |
|------|------|
| 保留 Reactor 做客户端数据面 | 不推倒 `EventLoop`/`TcpServer`，Gateway 继续扛连接 |
| 内网统一 brpc，禁止再引入 gRPC | 复用已安装 brpc 与 `MailBrpcService` 集成方式 |
| GameLogic **同构通用实例池** | 同一二进制多实例；禁止按地图编译/永久绑定进程 |
| MapInstance **单一 Owner + owner_epoch** | 有地图后必须遵守；禁止双写同一运行副本 |
| 连接 ≠ 会话 | `connection_id` 本机有效；全局 `session_id + fence_token` |
| 资产以 DB 为事实源 | Redis 只做会话/Presence/缓存；写路径幂等 |
| 渐进拆分，小步可回滚 | 先单进程解耦 → 双进程 → Session/重连 → GameDB 强化 → 地图池 |
| I/O 线程禁止同步 DB/RPC/长锁 | 与现有「逻辑在 IO 上」冲突，必须分阶段改掉 |

### 3.2 建议裁剪或延后（对当前体量过重）

| 项 | 决策 | 理由 |
|----|------|------|
| 第一天拆 5+ 进程 + etcd + NATS + OTel + K8s | **延后** | 仓库尚无地图与 Actor，过早引入运维复杂度 |
| 全面 Actor + MapActor + AOI Tick | **中后期** | 现无场景系统；先保证 player 命令串行 |
| Envelope 立刻替换外网协议 | **内部先上，外网适配** | 保持现有 length-prefix + `game.proto` 兼容 |
| PostgreSQL / 提前分库 | **不做** | 继续 MySQL；单库瓶颈后再按 `player_id` 分片 |
| NATS JetStream Outbox | **GameDB 阶段再评估** | 可先用「DB 事务 + 本地 outbox 表 + 后台线程」 |
| OpenTelemetry 全家桶 | **延后** | 先扩展现有 `/metrics` 与结构化日志字段 |
| 整仓搬迁到 `server/src/...` | **不做大搬家** | 新代码可按模块落目录，旧路径逐步迁移 |
| ChatGPT「工作包自动连跑」 | **不自动执行** | 本文只作方案基线；每阶段人工启动 |

### 3.3 总体可行性一句话

> ChatGPT 文档的**架构方向正确、适合作为目标态参考**；但按仓库现状，必须把「地图池 / etcd / NATS / 完整 Actor / 生产化」后移，**前 3～4 个阶段聚焦：解耦 IO、拆 Gateway/GameLogic、brpc 转发、Session 重连**。  
> 在此约束下，方案**可行**，且与已有 Reactor + MySQL + Redis + brpc 高度契合。

---

## 4. 目标架构（分期可见）

### 4.1 中期目标拓扑（第一可运营形态）

```text
客户端
  │  现有 ProtoFraming + game.proto（兼容）
  ▼
L4 / 直连
  ▼
多组 Gateway（Reactor：连接、拆包、限流、转发、推送）
  │ brpc 异步 RPC（复用 Channel）
  ├──────────────┬────────────────┬────────────────┐
  ▼              ▼                ▼                ▼
Login/Session   多组 GameLogic    World（中控）     （GameDB 可嵌 Logic）
(可先合并)      （同构二进制池）   聊天/好友/邮件等
  │              │                不跑战斗 Tick
  ▼              ▼                │
Redis           MySQL ←───────────┘
```

### 4.1.1 World 服定义（已拍板）

本方案中的 **World = 区服中控 / 全局业务服**，不是跑大世界战斗的进程：

| 做 | 不做 |
|----|------|
| 跨玩家、跨地图的全局逻辑：聊天、好友、邮件（过渡期）、公会社交入口、区服公告等 | **不跑战斗 Tick / AOI / 移动同步** |
| 经 Gateway 或 brpc 收发；需要时通知 GameLogic（发奖、踢人提示等） | 不持有玩家战斗/场景权威状态 |
| 早期可一个进程兜住多类全局模块；压力大再拆 Chat/Friend/Mail | 不替代 Session（在线栅栏）与 MapScheduler（地图 Owner） |

与 GameLogic 的边界：

- **GameLogic**：场景内权威（进图后命令、战斗、地图 Owner、背包写权威等）。
- **World**：场景外 / 跨场景全局逻辑；与 Logic 解耦，避免把聊天好友塞进战斗进程。

### 4.2 远期目标拓扑（有地图与 HA 后）

采用控制面 / 数据面 / 全局业务面分离：

- **数据面：** Client → Gateway → 持有路由的 GameLogic Owner
- **控制面：** Session、MapScheduler（`map_instance_id → owner + epoch`）、服务发现
- **全局业务面（World 中控）：** 聊天、好友、邮件等；可水平扩展或按模块再拆
- **GameDB 面：** GameDB（事务、版本号、幂等）；异步事件总线按需引入
- **GameLogic：** 同构池，可同时承载多个 MapInstance；**禁止**按地图固定编译

远期组件（按需上线，不一次性全上）：

| 组件 | 职责 | 明确不做 |
|------|------|----------|
| Gateway | 连接、帧、限流、路由、推送 | 不改背包/货币，不加载完整玩家 |
| Login/Auth | 认证、短票据 | 不持有世界战斗状态 |
| Session/Router | 会话状态机、顶号、重连、玩家→Logic 绑定 | 不做资产库 |
| MapScheduler | MapInstance 放置与 epoch | 不跑 Tick |
| GameLogic 池 | Player/Map 权威逻辑、战斗 Tick | 不永久绑地图；不做聊天好友主逻辑 |
| **World（中控）** | **全局逻辑：聊天、好友、邮件等** | **不跑战斗 Tick；不拥有场景写权威** |
| **GameDB**（数据服） | 异步加载/提交、幂等 | 不跑每帧逻辑；进程/role 名 `gamedb` |
| Redis | 会话镜像、限流、Presence | 非资产事实源 |
| etcd（后期） | 服务发现/租约/路由版本 | 不写高频玩家状态 |
| 事件总线（后期） | 领域事件/审计 | 不进移动主路径 |

### 4.3 最小服务集合（实施时按此顺序出现）

**不要第一天拆十几个进程。** 进程出现顺序：

1. 仍单进程，但模块边界清晰（`role=all`）
2. `gateway` + `gamelogic` 双进程（同构 Logic 可起 2 实例）
3. `session`（可与 login 合并）独立
4. `gamedb` 独立或先作为 Logic 内异步模块
5. **`world` 中控进程**（先承接邮件/聊天/好友等全局模块；内部模块化，再按需拆）
6. etcd / 事件总线 / 完整 MapScheduler —— 有地图与多实例运维需求后再上

---

## 5. 技术选型（定稿）

| 领域 | 定稿 | 约束 |
|------|------|------|
| 语言 | 目标 C++17；过渡期允许 C++14 全局 + 局部 C++17 | 不引入协程强依赖 |
| 外网 | 现有 Reactor + ProtoFraming + `game.proto` | 不破坏客户端兼容；新字段向后兼容 |
| 内网 RPC | **brpc + Protobuf** | **禁止 gRPC**；`brpc::Channel` 启动期创建、运行期复用 |
| 会话/缓存 | Redis（已有 `SessionStore`） | 扩展 fence_token / online 路由，不做资产源 |
| DB | MySQL + `ConnectionPool` | 关键写事务 + 幂等键 + 行版本 |
| 服务发现 | 早期：配置文件 / Redis；后期：etcd | 不双真相源 |
| 异步事件 | 早期：DB outbox + 后台线程；后期评估 NATS | 消费者必须幂等 |
| 可观测 | 扩展现有 Prometheus `/metrics` + 日志字段 | OTel 后期可选 |
| 业务并发 | 近期：按 `player_id` 串行队列；中期：最小 Actor | IO 线程不跑重逻辑 |
| 部署 | 先裸机/手动多进程；compose 本地联调 | K8s 生产化阶段再定 |

---

## 6. 关键设计决策（已拍板）

### D1. Gateway 有状态连接，会话可迁移

- TCP 连接落在某一 Gateway；推送按 `player → gateway` 路由。
- 断线后可重连**任意** Gateway；由 Session 完成 Rebind，旧 `fence_token` 失效。

### D2. GameLogic 同构池

- 同一 `gamelogic` 二进制；实例仅 `instance_id`/端口/容量不同。
- 有地图后：任意实例可承载任意 `map_template`；运行时 Placement 决定 Owner。
- **禁止** `gamelogic_1001` 这类按地图编译的产物。

### D3. 路由演进

| 阶段 | 路由键 |
|------|--------|
| 无地图期 | `player_id → gamelogic_instance_id`（一致性哈希或显式绑定） |
| 有地图期 | `realm_id + map_instance_id → owner + owner_epoch`；玩家跟地图走 |

### D4. 外网协议兼容策略

- 外网继续 `GameRequest/GameResponse`。
- 内网逐步引入带 `session_id/fence_token/route_version/...` 的 Envelope（可放 `proto/`）。
- Gateway 做 Legacy 适配：外网包 ↔ 内网 Envelope。

### D5. 数据一致性分级

| 数据 | 权威 | 策略 |
|------|------|------|
| 货币/背包/邮件领取/交易 | MySQL | 事务 + 幂等键；未提交不回成功 |
| 技能 CD、临时战斗态 | Logic 内存 | 可丢可重建 |
| 位置/场景态（未来） | Map Owner 内存 | 周期快照；可配置回滚窗口 |
| 在线/绑定 | Session + Redis 镜像 | TTL + fence；Logic 二次校验 |

### D6. World 中控与 Mail 路径

- **World = 中控**：暂放全局逻辑（聊天、好友、邮件等），**不跑战斗 Tick**。
- 短期：邮件等仍可由 Logic 调现有 `MailService` / brpc mail（兼容现状）。
- 中期：邮件迁入 **World**；Gateway 按消息类型路由到 World 或 GameLogic；Logic/Gateway 不直连邮件表。
- 远期：World 内聊天/好友/邮件可再拆独立进程，对外仍可保留 World 作为区服中控入口或旁路注册名。

### D7. World 与 GameLogic 交互

- 客户端全局类请求：Gateway → **World**（brpc）。
- 场景/战斗/进图类请求：Gateway → **GameLogic**。
- World 若需改背包等资产：经 GameDB 或向 Logic 发**有幂等键的指令**，禁止 World 直接改 Logic 内存。

---

## 7. 与现有模块映射

| 现有 | 目标归属 | 改造要点 |
|------|----------|----------|
| `tcp/*` Reactor | Gateway 数据面 | 保留；回调变薄 |
| `game/ProtoFraming*` | Gateway | 保留外网帧 |
| `game/GameTcpGateway*` | `gateway` | 去业务，加 brpc 转发/推送 |
| `game/GameService*` | Gateway 适配或内网编解码 | 不再直接调单例业务写库 |
| `game/GameLogic*` | `gamelogic` | 去单例进程耦合；命令串行；异步落库；剥离聊天好友邮件 |
| `game/SessionStore*` | Session/Login | 扩展状态机与 fence_token |
| `game/Mail*` + `game/brpc/*` | **`world`（中控，邮件模块）** | 已有 brpc 雏形优先复用；后续同进程挂聊天/好友 |
| `db/ConnectionPool*` 等 | GameDB / 各服数据访问 | IO 线程禁用同步取连接做重事务 |
| `db/PlayerItemPersistQueue*` | GameDB 雏形 | 强化幂等与版本号 |
| `redis/*` | 会话/路由镜像 | 扩展结构，不加资产语义 |
| `base/Prometheus*` | 各进程 metrics | 按 role 打 label |
| `test/http_server.cpp` | 过渡期 `role=all` 或拆为多 main | 最终多可执行文件 |
| `client/mail_brpc_client` | 内网客户端样例 | 扩展为 Gateway→Logic / Gateway→World |

---

## 8. 分阶段实施路线（后续按此改架构）

> 每阶段结束必须：**可编译、有验收、可回滚到上一阶段行为**。  
> 未完成上一阶段验收，不启动下一阶段。

### 阶段 0 — 基线冻结（不改行为）

**目标：** 可重复压测与指标，便于回归。

- 记录连接数、EventLoop 延迟、QPS、p99、错误率（尽量用现有 metrics，缺口再补）。
- 明确构建方式：`CMake` + 现有 option（`ENABLE_MYSQL/REDIS/BRPC/...`）。
- 输出基线数字到 `docs/mmo-migration/`（实施该阶段时再建）。

**验收：** 功能与协议不变；压测可复现。

---

### 阶段 1 — 单进程内解耦（仍一个进程）

**目标：** 网络与业务边界清晰，为拆进程做准备。

- 引入抽象：`SessionHandle`、`ReplySink`、`ITransport`（先 `InProcessTransport`）。
- 业务 Handler **逐步**不再直接握 `TcpConnection*`。
- 把「解码后同步 Handle」改为「投递到按 `player_id` 分片的串行队列」（最小可运行版即可，不必完整 Actor 框架）。
- I/O 回调只做：读、校验、入队、回投写。

**验收：** 仍单进程；客户端无感；ASAN 回归通过；同一 `player_id` 命令有序。

---

### 阶段 2 — 拆 Gateway / GameLogic（brpc）

**目标：** 双进程跑通一条完整业务链（建议：`ValidateSession` 或 `ConsumeItem` / `GrantItem` 选一条）。

- 基于现有 brpc：`BrpcChannelManager`（长生命周期 Channel）+ 异步 Call。
- 可执行角色：`--role=all|gateway|gamelogic`（或分 target，但逻辑包相同）。
- Gateway：外网帧 → 内网 RPC → 回包；GameLogic：无监听外网玩家端口（或仅内网）。
- 资产写默认不做框架层盲目重试；依赖幂等键。

**验收：**

- `role=all` 与 `gateway+gamelogic` 行为一致。
- 可起 `gamelogic-1`、`gamelogic-2` 同构实例。
- RPC 超时/对端退出表现为明确失败，不静默丢包。
- **本阶段不上 etcd/NATS/地图迁移。**

---

### 阶段 3 — Session、顶号、断线重连

**目标：** 连接可丢，会话可恢复。

- Session 状态机：`AUTHENTICATED → LOADING → ONLINE → DISCONNECTED → CLOSING → OFFLINE`。
- `session_id` + `fence_token`；默认 `REPLACE_OLD` 顶号。
- Gateway→Logic 强制带 token；Logic 拒绝旧 token。
- 断线宽限期（建议先 30～60s 可配）；任意 Gateway Rebind。
- Redis 保存镜像；Logic 校验仍是最终栅栏。

**验收：** 杀 Gateway 后可重连；双端登录仅新 token 有效；旧定时器不能误杀新会话。

---

### 阶段 4 — GameDB 强化

**目标：** 一条关键资产链完全异步且幂等（优先：发奖/扣道具/邮件领取之一）。

- `IGameDbRepository` 异步接口；连接池线程与 Actor/串行队列隔离。
- `version` 乐观锁 + `idempotency_key` 唯一约束。
- Outbox：同事务写事件；发布器后台投递（先本地表，再评估 NATS）。

**验收：** 重复请求、进程在提交前后崩溃，均不重复发奖/扣款。

---

### 阶段 5 — 地图模型与通用 Logic 池

**目标：** 在有真实地图/副本需求时启用（当前仓库无地图，**可空窗等待玩法**）。

- 区分 `map_template_id` / `map_instance_id` / `gamelogic_instance_id`。
- MapPlacement：`owner + owner_epoch + route_version`。
- 同构池动态放置；支持单实例多地图、同模板多副本。
- 迁移/故障：冻结 → 快照 → 新 epoch Claim → 改路由；旧 epoch 拒绝写入。

**验收：** GL1 可同时承载多类型实例；迁移后旧 Owner 不能写；新增 Logic 实例无需改代码绑地图。

---

### 阶段 6 — World 中控与生产化

- 拉起 **`world` 进程（中控）**：先迁入邮件，预留聊天/好友模块边界；**确认无战斗 Tick**。
- 压力上来后再从 World 内拆 Chat/Friend/Mail 独立进程（可选）。
- 服务发现（etcd）、优雅排空、灰度、故障演练脚本。
- mTLS、容量告警、RPO/RTO 与 runbook。

**验收：** Gateway 能按消息类型把全局请求打到 World、场景请求打到 Logic；杀 Gateway/GameLogic/World 的演练有预期恢复时间；灰度可回滚。

---

## 9. 推荐实施顺序（工作切片，人工启动）

实施时建议按下列切片推进（对应上节阶段，可再拆 PR）：

| 序号 | 切片 | 对应阶段 |
|------|------|----------|
| W0 | 仓库审计归档 + 本方案确认 | 0 |
| W1 | 基线指标与压测入口 | 0 |
| W2 | InProcess 解耦 + player 串行队列 | 1 |
| W3 | 内网 proto（Forward/Session 元数据） | 2 准备 |
| W4 | brpc Gateway↔GameLogic 一条业务链 | 2 |
| W5 | 双 Logic 实例 + 静态路由 | 2 |
| W6 | Session 状态机 + fence_token | 3 |
| W7 | 断线重连 / 顶号 | 3 |
| W8 | 异步 GameDB 垂直样例 | 4 |
| W9 | MapPlacement（有玩法需求时） | 5 |
| W10 | World 中控（邮件迁入 + 聊天/好友骨架）与生产化 | 6 |

每一切片要求：实现 + 测试 + 文档状态更新（实施期维护 `docs/mmo-migration/STATUS.md`）。

---

## 10. 明确不做清单（防范围膨胀）

1. 不引入 gRPC 或第二套通用 RPC。
2. 不按地图编译/永久部署不同 GameLogic 二进制。
3. 不让两个 Logic 同时写同一 `map_instance_id`。
4. 不用 Redis 锁作为资产正确性的唯一保障。
5. 不在 Reactor IO 线程做同步 SQL / 同步 brpc / `future.get()`。
6. 不以 `player_id % 当前节点数` 作为长期路由（扩缩容会大搬家）。
7. 不断线立刻销毁角色且无宽限期（移动网络会打爆重载与竞态）。
8. 不把 Gateway 做成第二个逻辑服（保存唯一玩家权威状态）。
9. 不在无地图需求时强行实现完整 AOI/Tick/MapScheduler。
10. 不一次性拆十几个微服务再集成。
11. **不把战斗 Tick / AOI 放进 World**；World 只做中控与全局业务。
12. **不让 World 直接改 GameLogic 内存中的玩家战斗/背包权威状态**（须走幂等指令或 GameDB）。

---

## 11. 登录 / 下线 / 重连（目标语义摘要）

### 11.1 登录（目标态）

1. 客户端连任意 Gateway。  
2. Auth 发短票据（密码不进 GameLogic）。  
3. Session 串行占用 `player_id`，生成 `session_id/fence_token`。  
4. 解析目标 GameLogic（无地图：按玩家绑定；有地图：经 MapPlacement）。  
5. Logic `ClaimPlayer` → 异步加载 → Ready。  
6. Gateway 原子绑定 `conn → session → logic(+map epoch)`。  
7. 后续命令由 Gateway 填可信路由字段，**不信任客户端自报**。

### 11.2 主动下线

停止新命令 → 最终落盘成功 → 条件释放 Session → Ack → 关连接。落盘失败保持 `CLOSING` 并有界重试。

### 11.3 异常断线

进入 `DISCONNECTED`，保留 Actor/玩家上下文至宽限期；超时走下线闭环；宽限内任意 Gateway Rebind。

---

## 12. 故障预期（目标态）

| 故障 | 预期 |
|------|------|
| Gateway 崩溃 | 重连任意 Gateway；Logic 宽限内保留；旧 token 失效 |
| GameLogic 崩溃 | 无地图期：玩家重登/迁移到健康实例；有地图期：新 Owner + epoch 恢复 |
| Redis 短暂不可用 | 已在线尽量继续；无法证明会话唯一则暂停新登录 |
| MySQL 不可用 | 关键写失败，绝不先回成功 |
| 服务发现不可用 | 使用最后有效路由快照；禁止不安全切主 |

---

## 13. 容量与可观测（最低要求）

逐步具备（优先复用现有指标名并扩展 label）：

- Reactor：连接数、循环延迟、收发字节、发送队列。
- Gateway：帧错误、限流、路由失败、慢客户端断开。
- Logic：按实例负载、处理耗时（已有 `LogicMetrics` 可扩展）。
- Session：ONLINE/DISCONNECTED、重连成功、旧 token 拒绝。
- RPC：每方法 QPS、延迟、错误码。
- GameDB：事务延迟、版本冲突、幂等命中、outbox 积压。

日志关键字段：`request_id`、`player_id`（可脱敏）、`session_id` 哈希、`gamelogic_instance_id`、`message_type`、`error_code`；有地图后再加 `map_instance_id` / `owner_epoch`。

---

## 14. 待产品确认的参数（不阻塞阶段 0～2）

实施中后期需要产品/策划确认，但不改变本文主体设计：

- 游戏形态：开放世界 / 房间 / 副本；是否固定 Tick。
- 目标 CCU、单 Gateway 连接上限、单图人数、可接受延迟。
- 顶号策略、断线托管时长、跨区服是否需要。
- 资产 RPO、世界状态允许回滚窗口、服务 RTO。
- 部署环境：裸机 / VM / Docker / K8s。

---

## 15. 参考文档与修订

| 文档 | 角色 |
|------|------|
| `docs/mmo-distributed-architecture.md`（本文） | **正式改造基线** |
| `docs/archive/mmo_distributed_architecture_cpp17_gamelogic.md` | 归档任务书；原则可参考，节奏与组件以本文裁剪为准 |
| `docs/mmo-migration/*` | 实施期再创建（审计、STATUS、DECISIONS） |

### 修订记录

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.0 | 2026-08-06 | 综合 ChatGPT 方案与仓库审计，形成可执行分阶段基线；本期仅落文档不改代码 |
| v1.1 | 2026-08-06 | 明确 World=中控：聊天/好友/邮件等全局逻辑，不跑战斗 Tick；与 GameLogic 边界写入 D6/D7 |
| v1.2 | 2026-08-07 | 阶段 0 启动：`docs/mmo-migration/`、构建/基线脚本、空载 metrics 样本 |

---

## 16. 下一行动（人工触发）

1. ~~阶段 0–6~~ 已完成 — 见 `docs/mmo-migration/STATUS.md`。  
2. ~~独立 Session/GameDB + 薄生产化 + 多二进制（1B/2B）~~ — 已完成：零前缀多二进制、etcd/NATS/SSL 可选、kill/gray runbook、`run_midterm_local.sh`。  
3. **完成标准（本切片）：** 五进程可启；Gateway→Session Login；邮件 Gateway→World→GameDB；无可选组件时行为与现网一致。  
4. 后续可选：Chat/Friend 产品逻辑、AOI/Tick、K8s 清单、etcd 热更（无需重启 Gateway）。
