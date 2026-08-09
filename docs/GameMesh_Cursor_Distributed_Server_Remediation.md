# GameMesh 分布式游戏服务器后续实现提示词

> 适用仓库：`https://github.com/madongxin/webserver`
>
> 审查基线：`main@1d7d0e0`
>
> 目标：在现有 C++17、Reactor、ProtoFraming、brpc 和多进程骨架上，逐步实现路由正确、可扩缩、可故障隔离、可恢复、可运维的分布式游戏服务器。

## 使用方式

把本文件放入仓库，例如：

```text
docs/GameMesh_Cursor_Distributed_Server_Remediation.md
```

每次只向 Cursor 下达一个阶段：

```text
阅读 docs/GameMesh_Cursor_Distributed_Server_Remediation.md，执行阶段 N。
只执行该阶段；完成测试和阶段门禁后停止，等待我确认。
```

不要一次跨越多个阶段。前一阶段的硬性门禁未全部通过时，不得进入下一阶段。

---

# 可直接交给 Cursor 的总提示词

你正在修改 GameMesh 项目。请基于当前工作区的真实代码实施，不要只输出架构建议，也不要盲信本文记录的历史问题。

如果某项已经实现，必须给出以下证据后才能标记完成：

1. 实现文件和核心符号。
2. 覆盖该语义的真实测试。
3. 实际执行命令、退出码和结果。
4. 涉及多进程的能力必须通过多进程或容器级测试，不能只用 mock 证明。

## 固定架构边界

```text
Client
  ⇄ Reactor TCP + ProtoFraming + Protobuf
Gateway
  ⇄ 异步 brpc
Auth / Session / GameLogic / GlobalService / GameDB
```

必须遵守：

- 使用 C++17，不引入 gRPC。
- 客户端只连接 Gateway，不直连 GameLogic，不使用 brpc。
- 保留现有 Reactor，不推倒重写。
- AuthService 与 SessionService 可同进程部署，但职责和接口分离。
- GameLogic 是同构通用节点，每个实例能承载任意 MapTemplate。
- 一个具体 MapInstance 同一时刻只能有一个可写 Owner。
- 玩家绑定后按 Session 路由定向调用指定 GameLogic，不能逐请求 round-robin。
- Disconnect 不等于 Logout，保留重连宽限期。
- Redis 保存会话、路由和短期协调状态，不是玩家资产事实源。
- 正式模式下账号和资产通过 GameDB 访问 MySQL，不能隐式绕过 GameDB。
- 状态修改 RPC 默认禁止框架自动重试；业务重试必须有 idempotency key。
- Reactor I/O 线程不得同步调用 brpc、Redis 或 MySQL。

## 每阶段开始前必须执行

1. 完整阅读仓库内 `AGENTS.md` 及目标目录适用的约束。
2. 执行：

   ```bash
   git status --short
   git branch --show-current
   git rev-parse HEAD
   ```

3. 保留用户已有修改，不回滚、不覆盖、不格式化无关文件。
4. 阅读当前阶段涉及的 proto、服务实现、配置、脚本和测试。
5. 先用代码和测试确认问题仍存在，再修改。
6. 在 `docs/distributed_remediation_progress.md` 记录 HEAD、阶段、任务、测试、阻塞和遗留风险。
7. 直接实现当前阶段，不要检查完后停在报告阶段。

## 全局禁止项

- 不执行破坏性 Git 操作，不提交、不推送，除非用户另行明确要求。
- 不删除失败测试，不通过空实现、硬编码成功、永真断言或扩大 `SKIP` 让 CI 变绿。
- 不把依赖缺失、服务未启动或测试二进制不存在记作通过。
- 不在依赖失败时 fail-open。
- 不在业务代码硬编码 `127.0.0.1`、`0.0.0.0`、固定实例 ID 或服务列表首地址。
- 不逐请求创建 `brpc::Channel`。
- 不混用 access token、refresh token 和 session fence token。
- 不记录密码、完整 Token、私钥或线上连接串。
- 当前阶段门禁未通过时，不得更新文档声称该能力已完成。

## 每阶段完成时统一输出

1. 修改文件列表。
2. 关键行为变化和新增/调整的 RPC。
3. 实际执行的构建、测试和演练命令。
4. 每条命令的真实退出码与摘要。
5. 阶段门禁逐项结果。
6. 阻塞项、遗留风险和安全回滚方式。

如果有门禁未通过，明确输出 `阶段 N 未完成` 并停止。

---

# 阶段 0：干净构建、可信测试与容器基线

## 目标

建立可信开发基线。全新 clone 必须能明确完成依赖检查、Debug/Release 构建、单元测试和多角色容器启动；任何失败都要真实暴露。

## 审查线索

`main@1d7d0e0` 曾出现：`Buffer.cpp` 的 `pessimizing-move`、`TimeStamp.h` 缺 `<ctime>`、CI 未准备 brpc 却默认全量构建、Docker 固定 Gateway 入口导致其他角色启动错误。先确认当前代码，不得盲改。

## 实现任务

### 0.1 编译与依赖

- 统一 `CMAKE_CXX_STANDARD 17` 和 `CMAKE_CXX_STANDARD_REQUIRED ON`。
- 修复 clean build 的真实错误，不通过全局关闭告警掩盖项目问题。
- 区分“完整分布式构建”和“不依赖 brpc 的低层单测构建”。
- 完整 CI 必须安装或使用包含兼容 brpc、protobuf、protoc、hiredis、MySQL Client 的工具链镜像。
- 固定并检测 brpc、protobuf、protoc 的兼容版本。

### 0.2 脚本

复用并增强现有脚本，缺失时补齐等价入口：

```text
scripts/check_deps.sh
scripts/build.sh
scripts/test.sh
scripts/run_cluster_local.sh
scripts/stop_cluster_local.sh
```

要求：使用 `set -euo pipefail`；不依赖调用目录；不自动 sudo；必需依赖或测试缺失时返回非零；PID 精确到实例；停止脚本不能按模糊进程名误杀；配置生成不得覆盖已有配置。

### 0.3 Docker 与 Compose

- 一个镜像可正确启动 gateway、session、gamelogic、world/global、gamedb。
- 不允许固定 `ENTRYPOINT gateway` 后把其他角色名错误追加为 Gateway 参数。
- 使用 entrypoint dispatcher，或由 Compose 指定完整可执行文件。
- 每个角色使用独立 instance_id、listen_addr、advertise_addr、配置和健康检查。
- 验证容器内实际进程角色，不只检查容器为 running。

### 0.4 测试可信度

- 删除或重写只验证局部变量、永真断言、只检查文件存在的伪测试。
- 测试脚本区分 PASS/FAIL/SKIP；必需测试不得 SKIP。
- 集成测试先验证 MySQL、Redis、etcd 和内部服务 ready。
- 将 Reactor、PasswordHash、PushReplayCache 的有效低层测试纳入统一入口。

## 必测场景

1. 全新构建目录 Debug 和 Release 构建。
2. 低层模式只构建明确允许的 target。
3. 完整模式缺 brpc 时明确失败。
4. Compose 中每个服务运行正确二进制角色。
5. 各角色健康检查能区分实际角色。
6. Shell 脚本语法和失败码传播。

## 阶段 0 门禁

```bash
./scripts/check_deps.sh
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/build.sh Release
docker compose config
docker compose up -d
./scripts/smoke_test.sh
docker compose down
```

本机无 Docker 时可在 CI 执行容器门禁，但必须给出真实 CI 结果。

---

# 阶段 1：Gateway 身份、连接索引、登录与重连竞态

## 目标

保证玩家身份、Gateway 身份和连接绑定在登录、顶号、断线、跨 Gateway 重连及迟到回调下始终正确。

## 审查线索

审查基线曾发现：GameTCP 与 Push 注册使用不同 Gateway ID；旧连接断开时可能无条件删除已经指向新连接的 player/session 索引；Reconnect 与迟到 Disconnect 缺少竞态测试。

## 实现任务

### 1.1 统一 GatewayInstanceId

- 定义配置化且稳定的 `gateway_instance_id`，例如 `gw-0`，不得从 `0.0.0.0:port` 拼接。
- GameTCP、Session 绑定、GameLogic 绑定、GatewayPush 注册、日志和指标使用同一个 ID。
- `listen_addr` 与 `advertise_addr` 分离；实例身份不等于监听地址。
- 启动时检测空值和重复 instance_id，发现后 fail-fast。

### 1.2 条件删除连接索引

删除连接时必须满足：

```text
player_to_conn[player_id] 仍等于待删除 connection_id 才删除
session_to_conn[session_id] 仍等于待删除 connection_id 才删除
```

- `by_conn_`、player 索引和 session 索引在同一临界区保持一致。
- 新连接接管后，旧连接关闭不能破坏新索引。
- 连接关闭后清理流缓冲、绑定、发送队列和定时器。

### 1.3 异步 Login/Reconnect 状态机

```text
Auth.Login
→ Session.AcquireSession
→ 指定 GameLogic.BindPlayer
→ Gateway 原子绑定连接
→ Client LoginSuccess
```

```text
Session.ReconnectV2
→ 返回新 fence/generation 与完整路由
→ 指定 GameLogic Rebind/Bind
→ Gateway 原子替换旧连接索引
→ Client ReconnectSuccess
```

要求：

- 同一连接同时只有一个 Login/Reconnect 流程。
- callback 返回时验证连接仍存在、流程 generation 仍匹配。
- 新 fence 生效后，旧 Gateway 的 Dispatch、Disconnect、Logout 不得影响新会话。
- Bind/Rebind 失败时执行幂等补偿，不留下错误 ONLINE 状态。
- 普通命令以连接绑定为可信来源，不信任客户端自报 player/route。
- Reactor I/O 线程不得同步等待 brpc。

### 1.4 Session 原子性

- Acquire、Disconnect、Reconnect、Logout 使用 Redis Lua 或等价原子 CAS。
- 原子比较 `session_id + fence_token + generation`。
- Redis 客户端必须线程安全，不能用一个非线程安全 hiredis context 并发服务多个请求。

## 必测场景

1. 新连接接管后旧连接断开，新连接索引仍存在。
2. 旧 Gateway 的 MarkDisconnected 不会把新 Session 改为 DISCONNECTED。
3. 旧 fence/generation 的 Dispatch 和 Logout 被拒绝。
4. Login 任一步失败都不产生半完成 Session。
5. Bind 成功但客户端已断开时正确补偿。
6. gw0 断线后通过 gw1 Reconnect 成功。
7. Reconnect 后 Push 目标更新为新 Gateway ID。
8. 并发 Reconnect 只有一个有效 fence。
9. GameTCP 与 GatewayPush 使用相同 gateway_instance_id。
10. 后端 RPC 超时时 Reactor 仍处理其他连接。

## 阶段 1 门禁

- 单元、集成、双 Gateway 重连测试全部通过。
- TSAN 或等价并发验证不报告注册表数据竞争。
- 日志可用 request_id 关联流程，但不输出完整 Token。

---

# 阶段 2：跨 GameLogic 进图与权威玩家路由

## 目标

让 GameLogic 真正成为通用节点池。玩家从一个 GameLogic 进入另一个 GameLogic 所有的 MapInstance 后，Session、Gateway、本地 Actor 和后续 Dispatch 必须全部切换到新 Owner，且不存在双写窗口。

## 审查线索

审查基线曾发现：Gateway 直接把 EnterMap Dispatch 到 Placement 返回的新 Owner，但没有先在新 Owner Bind/Rebind；新 Owner 会以 `player not bound` 拒绝；即使 Session Route 更新，Gateway 本地 sticky route 仍可能指向旧 Logic。

## 实现任务

### 2.1 单一权威路由模型

统一路由对象，至少包含：

```text
player_id
session_id
fence_token
generation
gateway_instance_id
gamelogic_instance_id
map_instance_id
map_owner_epoch
route_version
route_state
```

- Session/Placement 的 Redis 状态是权威路由。
- GatewayConnRegistry 保存带 route_version 的缓存。
- GameLogic 保存当前玩家绑定和 map owner epoch。
- 每次路由变化都增加 route_version。
- Gateway 不得维护另一套不可比较版本的 sticky route。

### 2.2 跨 Logic Transfer 状态机

根据现有接口适配，可新增最小必要 RPC，但必须形成等价流程：

```text
1. ResolveOrAllocateMap，得到 target_logic、map_instance、owner_epoch
2. Session.BeginPlayerTransfer(CAS old route_version)，生成 transfer_id
3. 旧 Logic FreezePlayer，停止接收新的状态写
4. 旧 Logic 导出最小可转移状态或持久化检查点
5. 新 Logic PreparePlayer/BindPlayer，导入状态并校验 owner_epoch
6. Session.CommitPlayerTransfer，原子更新 Logic/Map/epoch/route_version
7. Gateway 原子更新本连接的路由缓存
8. 新 Logic 幂等执行 EnterMap
9. 旧 Logic FinalizeUnbind
```

要求：

- `transfer_id` 和每一步均幂等。
- Freeze 后旧 Logic 拒绝普通写命令，只允许查询转移状态和补偿。
- Commit 只能在新 Logic Ready 后执行。
- Commit 后所有 Dispatch 定向到新 Logic。
- 旧 Logic 的迟到请求因 route_version/fence/transfer state 被拒绝。
- 失败时明确 Abort/Rollback，不允许两个 Logic 同时可写。
- Gateway 崩溃后，Session/Logic 能按 transfer_id 恢复或回滚。
- 不使用 brpc 自动重试状态修改步骤。

### 2.3 Dispatch 路由刷新

- Dispatch 使用连接缓存中的具体 `gamelogic_instance_id`。
- 收到 `ERR_ROUTE_STALE` 或 `ERR_MAP_OWNER_STALE` 时刷新 Session 权威路由。
- 只有幂等或可确认尚未执行的请求允许有限业务重试。
- GameLogic 校验 bind、session、fence、route_version、map_instance 和 owner_epoch。
- 未知 GameLogic ID 必须 fail-closed，不能退化到地址列表第一个实例。

## 必测场景

1. 玩家先绑定 gl-0，再进入 gl-1 拥有的地图。
2. 新 Owner 未 Prepare/Bind 时不能 Dispatch EnterMap。
3. Commit 后 Gateway 后续命令全部发往 gl-1。
4. Commit 后 gl-0 拒绝旧 route_version 写请求。
5. Prepare 失败时旧 Logic 恢复处理且 Session 不切路由。
6. Commit 成功后 FinalizeUnbind 超时仍不双写。
7. Gateway 在 Transfer 中崩溃后能恢复或回滚。
8. 重复 transfer_id 不重复创建 Actor、不重复进图。
9. 一个 Logic 同时承载多个 MapTemplate 的实例。
10. 两个 Gateway 查询同一玩家得到相同权威路由。

## 阶段 2 门禁

- 至少两个 Gateway 和两个 GameLogic 的真实多进程测试通过。
- 跨 Logic 进图、失败回滚、迟到旧命令全部通过。
- 日志能证明任意时刻最多一个 Player/Map Owner 接受写命令。

---

# 阶段 3：动态服务发现与 Session 真正高可用

## 目标

让多个 Session 实例真正分担请求并可接管；所有内部服务支持动态注册、续租、发现、摘流和地址变化，不再只在启动时读取静态首地址。

## 审查线索

审查基线曾发现：Session 启动两个进程但 Gateway 固定连接首地址；etcd 使用 v2 HTTP、只在启动时查询、TTL 无 keepalive、无 watch、无注销；注册记录缺稳定 instance ID 或使用不可路由地址；Facade 最终仍可能只返回静态 Registry。

## 实现任务

### 3.1 统一 IServiceRegistry

Gateway GameTCP、GatewayPush、Auth/Session、GameLogic、GlobalService 和 GameDB 全部通过统一接口注册/发现。记录至少包含：

```text
service_name
instance_id
advertise_addr
protocol
version
capacity
status
lease_id
metadata
```

- `listen_addr` 可为 `0.0.0.0`，`advertise_addr` 必须可路由。
- 同一 instance_id 的地址变化可被 Watch 感知并更新 Channel。
- 优雅停服先标记 DRAINING，再停止接收新流量，最后注销。

### 3.2 etcd v3 或 brpc NamingService

删除生产路径的 etcd v2 原始 HTTP。选择并完整实现一种：

1. etcd v3 Lease + KeepAlive + Watch；或
2. 满足同等 lease/watch/metadata 语义的 brpc NamingService。

必须支持 Lease 续租、Watch 增删改、revision 恢复、优雅注销、ChannelManager 动态更新和 Channel 长期复用。

### 3.3 Session 负载与接管

- Gateway 的 Auth/Session 客户端不得截取第一个地址。
- 多 Session 通过 NamingService 和明确负载策略分担请求。
- Session 业务无状态，真实状态在 Redis 原子存储中。
- 任一 Session 崩溃后，其他实例继续 Login、Reconnect、Disconnect、Logout。
- 状态修改 RPC 幂等；超时后先查询结果，再决定是否重试。
- readiness 不通过的 Session 不进入节点池。

### 3.4 静态地址降级

- 开发环境保留 `*_addrs`。
- 注册中心不可用时使用最后有效节点和配置化静态节点。
- 空发现结果不得覆盖健康 Channel。
- 静态项也必须包含 instance_id，保留定向路由能力。
- 降级状态必须有指标和告警。

## 必测场景

1. 两个 Session 都收到真实请求。
2. kill 当前 Session 后另一个实例继续登录和重连。
3. Lease 存活超过三倍 TTL。
4. 服务崩溃后注册项在 TTL 后消失。
5. 新增 GameLogic 后 Gateway 无需重启即可发现。
6. GameLogic 地址变化后 Channel 动态更新。
7. 注册中心中断时最后有效节点继续工作并产生降级指标。
8. 拒绝 `0.0.0.0` 作为 advertise_addr。
9. DRAINING 实例不接收新玩家，但可完成迁移。
10. GatewayPush 可按 gateway_instance_id 发现跨主机地址。

## 阶段 3 门禁

- 两个 Session 真实承载流量并通过杀进程接管测试。
- KeepAlive、Watch 和动态 Channel 测试通过。
- 生产路径不再使用 etcd v2 单次查询。

---

# 阶段 4：MapInstance 所有权租约、迁移与故障恢复

## 目标

实现真正的 MapInstance 单写所有权。GameLogic 崩溃、网络隔离、优雅迁移和旧节点恢复时不产生双 Owner；无法无损恢复时必须执行明确的降级语义。

## 审查线索

审查基线曾发现：Placement 的 recovering、epoch、route version 主要是字段；没有 owner lease/续租扫描；迁移缺 Freeze/Snapshot/Claim/Ready/Cutover；所谓杀服脚本可能只修改 Redis，未真正 kill 进程。

## 实现任务

### 4.1 Placement 权威记录

至少持久化：

```text
realm_id
map_template_id
map_instance_id
owner_gamelogic_instance_id
owner_epoch
owner_lease_deadline
route_version
state
transfer_id
snapshot_version
updated_at
```

推荐状态：`PREPARING / READY / FREEZING / FROZEN / TRANSFERRING / RECOVERING / RELEASING`。

- 创建、Claim、续租、冻结、迁移、恢复、释放使用 Redis Lua/CAS 或等价事务。
- map_instance_id 全局唯一。
- Owner 每次变化增加 owner_epoch 和 route_version。
- 非法状态跳转拒绝并记录。

### 4.2 防止网络分区双写

仅增加 epoch 不足以阻止仍持有旧本地 epoch 的旧进程写入，必须实现 Owner Lease：

- GameLogic 定期续租自己拥有的 MapInstance。
- 只有本地可证明 lease 有效时才接受地图写命令。
- 续租失败超过安全窗口后旧 Owner fail-closed。
- 新 Owner 只能在旧 lease 过期，或收到旧 Owner FreezeAck 后 CAS Claim 更高 epoch。
- 恢复的旧 Owner 必须重读权威 Placement，不得凭内存状态恢复写入。

### 4.3 优雅迁移

```text
Source Freeze
→ Drain in-flight commands
→ Create snapshot/checkpoint
→ Target Prepare + Load snapshot
→ Target Claim higher epoch
→ Target Ready
→ Session/Gateway route cutover
→ Source Release
```

- 每一步使用 migration_id/idempotency key。
- 超时和重启后可以查询状态并继续或回滚。
- Snapshot 带版本和校验，包含恢复 Map/玩家连续性的最小状态。
- Target Ready 前不能切客户端路由。

### 4.4 崩溃恢复

- Logic lease 失效后 Placement 进入 RECOVERING。
- 调度器按容量和版本选择新 Owner。
- 有快照时加载；无快照时明确让玩家重新进图或从持久化状态恢复。
- 不得静默声称恢复了已经丢失的实时战斗状态。
- 旧 Owner 恢复后拒绝旧 epoch 请求。

## 必测场景

1. 两个 Gateway 并发创建地图不产生重复 ID。
2. 两个调度者并发 Claim 只有一个成功。
3. Freeze 后旧 Owner 不再接受写入。
4. Target Ready 前不切 Session 路由。
5. kill Owner 后 lease 过期，新 Owner 获得更高 epoch。
6. 旧 Owner 恢复后不能处理旧 epoch 写请求。
7. 网络隔离导致续租失败时旧 Owner 自动停止写入。
8. 迁移每一步重复执行都幂等。
9. 无快照恢复走明确的重新进图流程。
10. 故障演练真实 kill GameLogic，不能只改 Redis 字段。

## 阶段 4 门禁

- `scripts/kill_logic_drill.sh` 或等价脚本真实终止 Owner 并验证恢复。
- 故障、迁移、网络隔离测试中均无双写。
- 路由、epoch、lease 和状态机有指标与结构化日志。

---

# 阶段 5：可靠 Push 与跨 Gateway 回放

## 目标

让 Push 不再只是接口或进程内 deque。至少一个真实业务事件由 GameLogic 推送到正确 Gateway；可靠消息在跨 Gateway 重连后能够回放，缓存不足时触发全量状态同步。

## 实现任务

### 5.1 真实 Push 调用链

- 选择一个低风险真实功能接入，例如玩家状态、背包变更通知或进图结果。
- GameLogic 只保存 `player_id + session_id + gateway_instance_id`，不保存 Gateway connection_id。
- 按 gateway_instance_id 聚合 PushBatch，禁止广播所有 Gateway。
- GatewayPush 地址通过阶段 3 的注册中心发现，禁止写死环回地址和端口。
- Gateway 校验目标 instance_id、本地 player/session、fence/generation。
- Push 必须回投连接所属 Reactor EventLoop 后发送 TCP。

### 5.2 消息分类与背压

```text
coalescable：移动、朝向、普通 AOI，可覆盖旧状态
reliable：资产结果、地图切换、关键通知，需要 seq/ACK/重放
```

- 限制 PushBatch、连接发送队列的消息数和字节数。
- 慢客户端先合并可合并消息；仍超限时记录指标并断开。
- Push RPC 超时不能阻塞 PlayerActor/MapActor 线程。

### 5.3 server_seq、ACK 与共享 ReplayStore

- 每个有效 Session 使用单调递增 server_seq。
- 可靠消息写入跨 Gateway 可访问的 ReplayStore，例如 Redis Stream/有界列表；不能只存在旧 Gateway 内存。
- seq 分配与可靠消息写入必须原子。
- 客户端增加 ACK 或等价确认协议。
- ACK 使用 session/fence 校验并安全裁剪已确认消息。
- ReplayStore 有 TTL、条数和字节上限。
- 玩家资产事实仍在 GameDB/MySQL，ReplayStore 只负责传输恢复。

### 5.4 Reconnect 回放

```text
Client Reconnect(last_server_seq)
→ Session/Rebind 成功
→ Gateway 读取 seq > last_server_seq
→ 按序发送
→ 缓存不足或有 gap 时返回 NeedFullSnapshot
→ GameLogic/GameDB 生成权威全量状态
```

- Reconnect 成功前不回放到未绑定连接。
- gw0 → gw1 重连仍能访问旧消息。
- GameLogic/Gateway 重启后 seq 不得回退或重复。

## 必测场景

1. 真实业务事件调用 PushToBoundGateway。
2. gw0 玩家只收到发往 gw0 的 Push，不广播 gw1。
3. 玩家迁移到 gw1 后新 Push 只发往 gw1。
4. 旧 session/fence Push 被拒绝。
5. reliable 消息按 server_seq 有序发送。
6. ACK 后已确认消息被安全裁剪。
7. gw0 崩溃后连接 gw1，可回放未确认消息。
8. Replay gap 触发全量快照，不返回伪成功。
9. 慢客户端触发合并和有界断开。
10. GameLogic/Gateway 重启后 seq 不倒退。

## 阶段 5 门禁

- 真实双 Gateway E2E 重连回放通过。
- 不能只测试进程内 PushReplayCache/deque。
- 至少一个真实业务链路使用 PushBatch。

---

# 阶段 6：数据边界、真实认证与幂等一致性

## 目标

收敛正式分布式模式的数据所有权，消除绕过 GameDB 的写路径，补齐认证安全、注册幂等和 Outbox 可靠投递。

## 审查线索

审查基线曾发现：GameLogic 仍可能直接使用 MySQL 相关 Store/ConnectionPool；登录限流检查与记录使用不同状态；Token 生成和 access/refresh 分离不足；Auth Redis 连接并发安全不足；注册 idempotency key 未真正落到存储层。

## 实现任务

### 6.1 正式模式数据边界

```text
Auth → GameDB：账号、凭证、封禁、角色、注册
Session → Redis：会话、fence、路由、Placement
GameLogic → GameDB：玩家数据和资产
GlobalService → GameDB：邮件、公会、好友、全局数据
```

- 正式配置下 GameLogic、GlobalService、Auth 不得直接连接 MySQL。
- 删除隐式直连降级；GameDB 不可用时明确失败并触发熔断。
- 本地单体兼容模式若保留，必须是独立编译/配置开关，不进入正式脚本。
- 增加架构测试和运行时连接审计验证边界。

### 6.2 Auth 与 Token

- Login 验证真实 credential 或可信第三方票据，不能忽略 credential。
- 使用密码学安全随机源生成 access、refresh、session token。
- access 与 refresh token 不同，TTL 和用途分离。
- Token 只存安全摘要或使用等价安全存储。
- Auth Redis 客户端线程安全。
- 登录限流的检查和失败记录使用同一个线程安全状态源，优先 Redis 原子脚本。
- 支持 IP、账号、device 等维度的有界限流和安全审计。

### 6.3 注册和幂等

- Register 走 Gateway → Auth → GameDB。
- GameDB 存储层真正保存并校验 idempotency_key。
- 对账号名、外部账号 ID 等建立业务唯一约束；不要未经产品确认仅凭 device_id 强制一设备一账号。
- 并发重复注册只产生一个业务结果。
- 重试返回首次操作结果，不重复建号或发初始资产。

### 6.4 GameDB 与 Outbox

- 所有资产写带 idempotency_key 和 version/expected_version。
- 业务数据和 Outbox 在同一事务写入。
- 多发布者使用 claim、行锁或 `SKIP LOCKED` 防止同时处理同一行。
- NATS/消息系统必须等待服务端确认，socket send 成功不等于发布成功。
- 明确 at-least-once；消费者按 event_id 幂等。
- brpc、MySQL、Redis 设置 deadline、并发限制、有界队列和熔断指标。

## 必测场景

1. 正式模式 GameLogic 不建立 MySQL 连接。
2. GameDB 不可用时明确失败，不直连 MySQL。
3. 错误 credential 不创建 Session。
4. 登录限流在并发和多 Auth 实例下生效。
5. access/refresh token 不同且刷新流程正确。
6. Token 存储和日志无明文完整 Token。
7. 并发重复注册只创建一个账号。
8. 重放同一资产 idempotency key 不重复扣款或发奖。
9. 两个 Outbox publisher 不丢事件，消费者安全处理重复事件。
10. 唯一约束和业务错误码一致。

## 阶段 6 门禁

- 正式模式数据访问拓扑与设计一致。
- 认证、限流、注册幂等、资产幂等均有真实集成测试。
- 安全扫描无硬编码演示鉴权或完整 Token 日志。

---

# 阶段 7：生产运维、可观测性与故障演练

## 目标

把正确的分布式语义变成可部署、可观测、可摘流、可回滚和可验证的生产候选版本。

## 实现任务

### 7.1 健康检查与优雅停服

- `/health/live` 只表示进程事件循环存活。
- `/health/ready` 检查关键依赖、注册状态、Channel、Redis/MySQL、队列和角色初始化。
- 未 ready 实例不进入 LB 或注册中心可用列表。
- SIGTERM 后执行：

```text
标记 DRAINING
→ 从 LB/注册中心摘流
→ 停止新登录/新地图分配
→ 等待在途请求
→ Gateway 引导重连
→ Logic 迁移或保存关键状态
→ Flush Outbox/可靠 Push
→ 超时后安全退出
```

### 7.2 可观测性

Prometheus 至少包含：

- TCP 连接、断线、非法帧、字节、慢客户端。
- Login/Register/Reconnect 各阶段成功率、延迟、错误码。
- brpc QPS、延迟、超时、熔断和重试。
- 在线玩家、Session 冲突、fence 拒绝、Redis Lua 延迟。
- 每个 Logic 的玩家、地图、Actor 队列和拒绝数。
- Placement lease、迁移状态、stale epoch、恢复耗时。
- GameDB 队列、MySQL 延迟、事务冲突、Outbox 积压。
- Push accepted/rejected/coalesced/replayed/snapshot/gap。
- 注册中心 keepalive/watch/revision/fallback。

贯通 `trace_id + request_id + 脱敏 player_id + generation + route_version`。

### 7.3 安全与管理面

- 删除或编译隔离演示账号、测试崩溃入口和调试后门。
- 管理 HTTP 默认只绑定管理网或 loopback。
- 管理接口使用正式鉴权与最小权限。
- 内部 brpc 支持 mTLS，或提供明确的内网零信任接入及证书轮换方案。
- 客户端入口限制 IP/账号、最大连接、最大帧速率和协议错误次数。

### 7.4 CI、压力与故障演练

CI 至少包含：Debug、Release、unit、integration、多进程 E2E、ASan、UBSan、TSan 独立 job、格式/静态检查和 Compose smoke。

提供并真实执行：

```text
scripts/load_test.sh
scripts/kill_gateway_drill.sh
scripts/kill_session_drill.sh
scripts/kill_logic_drill.sh
scripts/kill_gamedb_drill.sh
scripts/network_partition_drill.sh
```

- 压测输出配置、时长、并发、吞吐、P50/P95/P99、错误率、资源和背压。
- 杀服脚本必须真实终止目标进程/容器。
- 演练失败返回非零，不能只打印说明。

### 7.5 文档和单点声明

更新拓扑、Login/Reconnect/EnterMap/Transfer/Push Replay/Logout 时序、状态所有权、数据边界、发现降级、部署扩缩容、灰度回滚和故障恢复 Runbook。

如果 Redis、MySQL、NATS、etcd 仍是单节点，必须明确标记；不能因为访问层多实例就声称存储已 HA。

## 最终 E2E 场景

1. Client 经 VIP 登录 gw0，创建唯一 Session 并绑定 gl-0。
2. 玩家进入 gl-1 拥有的地图，完成无双写 Transfer。
3. GameLogic 发送可靠 Push。
4. kill gw0，Client 通过 gw1 Reconnect 并回放未确认 Push。
5. kill 一个 Session，另一个继续处理会话。
6. kill gl-1，lease 过期后恢复到新 Owner，旧 epoch 永久失效。
7. 新增 gl-2，无需重启 Gateway 即可发现并承载新地图。
8. GameDB 不可用时资产写明确失败且不直连 MySQL；恢复后幂等重试成功。
9. Gateway/Logic 收到 SIGTERM 后摘流并优雅退出。
10. 压测期间随机 kill Gateway、Session、Logic，不出现双会话、双 Owner 或资产重复写。

## 阶段 7 门禁

- 最终 E2E 由自动脚本执行并返回真实结果。
- Debug、Release、Sanitizer、E2E、Compose CI 全部通过。
- 文档宣称的能力与测试证据一致。
- 输出剩余基础设施单点、容量上限和上线前人工事项。

---

# 最终验收定义

只有同时满足以下条件，才可描述为“达到分布式游戏服务器 MVP 目标”：

1. Gateway、Session、GameLogic、GlobalService、GameDB 可独立多实例运行。
2. Session 多实例真实分担并接管流量，不是只启动多个进程。
3. 玩家路由由权威 Session/Placement 管理，Gateway 与 Logic 使用可比较的 route_version/epoch。
4. GameLogic 是通用池，跨 Logic 进图和迁移闭环。
5. MapInstance 通过 epoch + lease 保证单写，旧 Owner 网络分区后不能继续写。
6. 服务发现具备 Lease、KeepAlive、Watch、动态 Channel 和优雅注销。
7. 跨 Gateway Reconnect 正确，旧连接和旧 fence 不影响新会话。
8. 可靠 Push 具备共享 seq、ACK、跨 Gateway 回放和全量快照降级。
9. 正式模式下 GameLogic/Global/Auth 不绕过 GameDB 写 MySQL。
10. 注册、资产写、转移和消息投递具备真实幂等语义。
11. CI、容器、健康、指标、优雅停服、压力和故障演练可重复执行。
12. Redis/MySQL/NATS/etcd 等剩余基础设施单点被明确记录。

如果只完成多进程拆分、静态 brpc 和基础登录，不得宣称已经完成动态扩缩容、跨节点故障恢复或生产级分布式 MMO 架构。
