# GameMesh 稳定版本与 Unity 接入准备：Cursor 执行提示词

> 适用仓库：`https://github.com/madongxin/webserver`
>
> 审查基线：`main@57e93934b20cf2f4ec70daef692fd3b7376d333a`
>
> 总目标：先将服务器收敛为可重复构建、可自动回归、可供客户端长期联调的稳定版本，再完成 Unity 客户端接入所需的协议、C# SDK、示例和端到端测试准备。

请将本文整体交给 Cursor。Cursor 必须严格按“工作包一 → 阶段验收 → 工作包二”的顺序执行，不要同时大面积修改两个工作包。

---

## 一、Cursor 总执行规则

你现在位于 GameMesh 项目根目录。请直接检查和修改代码，不要只输出分析或架构建议。

开始前必须：

1. 完整阅读仓库中的 `AGENTS.md`、README、架构文档、CMake、proto、配置、脚本和现有测试。
2. 执行 `git status --short` 和 `git log -1 --oneline`，记录当前基线。
3. 保留用户已有的未提交修改；不得覆盖、回退或删除无关改动。
4. 先还原实际运行调用链，再修改；不要只依据文档推断代码行为。
5. 直接修改代码、proto、配置、脚本、测试和文档，并实际执行验证。
6. 不执行 `git reset --hard`、`git checkout --` 等破坏性命令。
7. 未经用户明确要求，不要 commit、tag 或 push；只给出建议版本号和提交说明。

固定技术边界：

- 使用 C++17。
- 客户端继续使用 TCP 长连接、Reactor 和 ProtoFraming。
- 内部服务继续使用 brpc，不引入 gRPC。
- 不推倒重写现有 Reactor。
- Client 不直接连接 GameLogic，也不使用 brpc。
- GameLogic 是同构通用节点；具体 MapInstance 同一时刻只能有一个可写 Owner。
- Redis 可作为当前稳定开发版本的 Session、Placement、Registry 和 Push Replay 基础设施，但必须明确其可用性边界。
- MySQL 是账号和资产事实源；正式链路中的资产访问必须通过 GameDB。

质量规则：

- 禁止用空实现、固定返回成功、删除断言、屏蔽错误或伪造测试结果来通过验收。
- 必需测试缺少依赖、测试二进制不存在或场景未执行时，必须失败并返回非零退出码，不能显示为成功。
- 可选测试必须与稳定版本门禁测试分开，不能用 `SKIP` 掩盖必需能力未完成。
- 所有外部输入、RPC 超时、异步回调和重试都必须 fail-closed。
- Reactor I/O 线程中不得执行同步 brpc、Redis 或 MySQL 调用。
- brpc 异步请求的 Controller、request、response 和 callback context 必须存活到完成；`done` 必须恰好调用一次。
- 状态修改请求不得进行无幂等保护的自动重试。
- 新增错误必须使用结构化错误码；日志文本不能成为客户端判断逻辑。

执行顺序：

1. 只执行“工作包一：服务器稳定版本”。
2. 完成全部稳定版本门禁后，输出证据并停止，等待用户确认。
3. 用户确认后再执行“工作包二：Unity 接入准备”。
4. 如果工作包一没有通过，不得提前宣称可接入 Unity，也不得通过降低测试标准绕过问题。

---

# 工作包一：产出服务器稳定版本

## 1. 工作包目标

将当前多进程分布式 MMO 骨架收敛为一个稳定的客户端联调基线：

- 能从全新 clone 可重复构建和启动。
- 双 Gateway、双 Session、双 GameLogic、双 GameDB 的核心流程真实可用。
- 登录、普通命令、进图、Push、断线重连和关键资产写入闭环。
- 玩家身份、Session fence、地图 Owner 和数据幂等语义一致。
- 关键故障可以自动测试，失败不会被脚本隐藏。
- 当前不追求无损恢复所有实时地图状态，但必须明确失败模式，并防止双写和错误成功。

## 2. 修复 Gateway 可信身份覆盖不完整

当前 `TrustedPlayerId` 只覆盖部分请求类型。请建立一个统一、可穷举验证的可信身份注入机制。

要求：

1. 登录后，所有带 `player_id` 的客户端业务请求都必须使用 Gateway 连接绑定中的可信 `player_id`。
2. 客户端自报 `player_id` 与连接绑定不一致时，返回安全错误并记录指标；不能把客户端值转发给 GameLogic。
3. 至少检查并覆盖：
   - `ReleaseSkill`
   - `MapPing`
   - `ChatSend`
   - `FriendList`
   - 所有邮件相关请求
   - 背包、物品、好友、地图和战斗等其他含玩家身份的请求
4. 客户端不能覆盖：
   - `session_id`
   - `fence_token`
   - `generation`
   - `gamelogic_instance_id`
   - `map_instance_id`
   - `map_owner_epoch`
   - `route_version`
5. 未登录连接只能调用明确白名单，例如 Register、Login、Reconnect、Heartbeat。
6. 普通游戏命令在身份或路由不完整时必须拒绝，不能随机选择 GameLogic。

测试要求：

- 建立覆盖 `GameRequest` 全部 oneof 分支的参数化测试。
- 每个包含 `player_id` 的请求都验证可信覆盖或不一致拒绝。
- 后续 proto 新增含 `player_id` 的请求时，如果没有加入可信身份策略，测试或构建必须失败。

## 3. 统一玩家生命周期串行语义

当前 Bind、Dispatch、Freeze、Export、Import、Unbind 可能不在同一条玩家串行队列中，存在迁移快照与在途命令竞态。

将以下操作统一进入同一个 per-player mailbox / `PlayerSerialQueue`：

```text
BindPlayer
Dispatch
FreezePlayer
ExportPlayerSnapshot
ImportPlayerSnapshot
UnbindPlayer
Logout / FinalSave
```

必须满足：

1. `FreezePlayer` 是明确的队列屏障：
   - 等待冻结前已入队命令全部完成。
   - 冻结后不再接受新的状态修改命令。
   - Export 只能读取屏障之后的稳定状态。
2. Import 完成并校验快照后才能切换到可写状态。
3. Unbind/Logout 不得与 Dispatch 或 FinalSave 并发修改同一玩家。
4. 队列满时返回明确过载错误，不得切换到同步处理或绕过队列。
5. 所有异步完成路径必须保证回调一次且只调用一次。
6. 玩家 A 的慢请求不能阻塞同一工作线程上的大量无关玩家；如果现有分片线程内存在同步下游调用，应改为异步回调或可挂起任务。

新增真实并发测试：

- Dispatch 与 Freeze 同时发生，快照必须包含冻结前命令，不包含冻结后命令。
- Freeze 后的新写请求被拒绝。
- Export/Import 校验 checksum、player_id、epoch 和快照版本。
- Unbind 与在途 Dispatch 不产生 use-after-free、丢回调或状态倒退。
- 队列过载返回非零错误，不执行同步 fallback。

## 4. 强化 Map Placement 权威校验

正式分布式模式下，GameLogic 处理 EnterMap 和地图写命令前，必须验证权威 Placement。

Placement 至少包含：

```text
realm_id
map_template_id
map_instance_id
owner_gamelogic_instance_id
owner_epoch
route_version
state
lease_expire_at
```

要求：

1. GameLogic 只能处理满足以下全部条件的地图写请求：
   - Placement 存在。
   - `owner_gamelogic_instance_id` 等于本机实例 ID。
   - 请求 `owner_epoch` 等于权威 epoch。
   - 请求 `route_version` 不低于权威版本。
   - Placement 状态为 `READY`。
   - Lease 尚未过期。
2. 正式模式不得让任意 GameLogic 通过第一次 EnterMap 隐式 claim 地图 Owner。
3. `allow_first_claim` 如需保留，只能用于明确的单体/开发模式，并通过配置隔离，默认关闭。
4. Placement 的创建和 Owner 切换只能由 Session/Placement 权威服务通过原子 CAS/Lua 完成。
5. 旧 epoch、旧 route version、RECOVERING/FROZEN 状态和非 Owner 请求必须返回结构化错误。
6. 迁移切换顺序至少保证：

```text
旧 Owner Freeze
→ 导出稳定快照
→ 新 Owner Import/Prepare
→ 权威 CAS 提升 epoch 和 route_version
→ 新 Owner Ready
→ Session/Gateway 更新路由
→ 旧 Owner Unbind
```

7. 任一步失败必须保持单写；无法无损恢复时，明确让玩家重新进图或从持久化快照恢复，不能静默成功。

测试要求：

- 两个 Gateway 查询同一 MapInstance 得到相同 Owner 和 epoch。
- gl-0、gl-1 都可以承载不同地图实例。
- 同一 GameLogic 可以承载多个地图模板。
- 非 Owner、旧 epoch、旧 route version 和过期 lease 请求被拒绝。
- 迁移过程中的并发写不会在两个 Owner 同时成功。

## 5. 补齐可靠 Push 与跨 Gateway 重连

当前 Replay 和 FullSnapshot 链路必须真正接入运行时，不能只保留内存队列或接口。

统一可靠 Push 记录结构：

```text
player_id
session_id
server_seq
message_type
payload
reliable
coalescable
created_at
```

必须完成：

1. Replay 缓存保留完整 Push 元数据，不能只保存 payload。
2. 重连补发必须重新发送 `ServerPushEnvelope`，并保持原始 `server_seq` 和 `message_type`。
3. 修复 FullSnapshot 已生成但因条件判断而不发送的问题。
4. 重连成功后按固定顺序发送：

```text
ReconnectResponse
→ Replay 列表，或 FullSnapshot
→ 恢复实时 Push
```

5. Unity/客户端只有在业务状态成功应用后才发送 PushAck。
6. PushAck 必须校验：
   - 当前 player/session/fence/generation。
   - `ack_seq` 单调不回退。
   - `ack_seq` 不得大于当前已发送的最大序号。
7. 非法超前 ACK 不能裁剪 Replay 缓存。
8. 跨 Gateway 重连后，新 Gateway 必须能从共享可靠存储或权威 GameLogic 获取 Replay；不能依赖旧 Gateway 的进程内内存。
9. 缓存缺口时发送实际 FullSnapshot，并建立新的 ack baseline。
10. `coalescable` 消息允许合并；`reliable` 消息不能因慢客户端静默丢失。
11. 每个连接和每个玩家的发送/Replay 队列必须有上限和过载策略。

严格 E2E 测试：

- 不允许客户端猜测 `server_seq=1`。
- 测试客户端必须真实解析 `ServerPushEnvelope`。
- 断开 gw0 后通过 gw1 重连，断言补发的序列号和类型与断线前缺失消息完全一致。
- 制造 Replay 缓存缺口，断言客户端收到真实 FullSnapshot，而不是任意一帧即算通过。
- 重复 Push 去重、乱序检测、合法 ACK、超前 ACK 和旧 Session ACK 都有测试。

## 6. 收敛 GameDB、幂等与 Outbox

### 6.1 玩家加载必须 fail-closed

正式模式下：

- GameDB 加载失败时不得创建空背包并返回 Bind 成功。
- `EnsurePlayer` 或等价接口返回明确成功/失败结果。
- BindPlayer 只有在玩家必需数据加载完成后才返回 Ready。
- 加载失败时 Gateway 必须回滚未完成 Session 或标记为可重试状态，不能留下半登录在线会话。

### 6.2 稳定幂等键

资产写入的 `idempotency_key` 不能使用当前时间生成。

请使用可由同一业务请求稳定重建的字段，例如：

```text
session_id + request_id/client_seq + operation_type + player_id
```

要求：

- 相同业务请求重试得到相同幂等键。
- 不同业务请求不会冲突。
- `SaveSnapshot`、Grant、Consume、交易和其他资产写都持久化幂等结果。
- 遇到 RPC 超时或结果未知时，先用 `QueryOperationResult` 查询，再决定是否重试，禁止盲重试。

### 6.3 Outbox 原子性

资产事务与 Outbox 必须同事务提交：

- Outbox insert 失败必须回滚资产写。
- 不能只记录 warning 后提交事务。
- 多 GameDB 实例通过 claim/lock 机制避免同一 Outbox 行被同时发布。
- 事件语义明确为 at-least-once，消费者必须按 event/idempotency key 幂等。

### 6.4 GameDB 多实例

- 读请求可在健康实例间负载和故障切换。
- 写请求只有在幂等键和未知结果查询机制完整时才能故障切换。
- GameDB Channel 动态更新，不固定永久命中单个实例。
- 下游慢调用不得阻塞 Reactor 线程，也不得阻塞同 shard 大量无关玩家。

测试要求：

- GameDB 不可用时 BindPlayer 失败，不创建空状态。
- 同一幂等键并发 10 次只产生一次资产变化。
- RPC 返回未知结果后查询并恢复，不发生重复扣除或发放。
- Outbox insert 人工失败时资产事务回滚。
- kill 当前 GameDB 后，幂等写按预期恢复或明确失败。

## 7. 让动态服务发现真实生效

当前可继续使用 Redis Registry 作为稳定开发版本的服务发现实现，但必须让动态实例变化进入实际路由。

要求：

1. Session/Placement 的健康 GameLogic Owner 列表必须动态更新，不能永久使用启动配置中的 `logic_instance_ids`。
2. 新增 gl-2 后，不重启 Gateway/Session 即可被发现，并真实获得新建 MapInstance。
3. GameLogic 必须动态发现 `gateway_push` 实例，不能只在启动时读取静态地址。
4. Gateway、Session、GameLogic、GlobalService、GameDB、GatewayPush 都使用统一 Registry 接口。
5. 注册信息包含：

```text
service_name
instance_id
advertise_addr
port
version
capacity
status
lease_expire_at
```

6. 只能注册可路由的 advertise address，不能注册 `0.0.0.0`。
7. 支持 Register、KeepAlive、Discover、Drain、Unregister。
8. 实例进入 DRAINING 后停止分配新玩家和新 MapInstance，在途请求按策略完成。
9. 不要在 Redis Lua 中一次性执行无界全量 SCAN；使用增量扫描、索引集合或事件通知，避免阻塞 Redis。
10. Registry 不可用时：
    - 保留最后有效路由。
    - 可使用明确配置的静态地址降级。
    - 不得把未知实例随机映射为列表第一个实例。

文档必须明确：Redis Registry 是当前稳定开发/联调版本的实现，不等于已经实现 etcd v3 或完整生产级控制面；Redis 自身仍需 HA 才能消除单点。

## 8. 修复构建、CI 与可重复环境

必须解决 clean build 和 CI 不能真实构建完整 brpc 工程的问题。

要求：

1. 修复 GCC 13 + `-Werror` 的已知问题，包括 `LogStream.cpp` 中无意义的 `static_cast<const double>`。
2. 全仓库使用 C++17，不再混用 C++14。
3. 提供可重复的完整依赖方案，二选一或同时提供：
   - 固定版本的工具链/依赖 Docker 镜像；
   - 可重复的 `scripts/install_deps.sh`，不自动 sudo，并给出清晰系统依赖说明。
4. CI 必须实际安装或使用已经包含 brpc、protobuf、hiredis、MySQL client、jsoncpp 和 cmake 的镜像。
5. Dockerfile/Compose 必须能正确启动不同 server role，不能用固定 Gateway ENTRYPOINT 吞掉其他服务命令。
6. 全新 clone 后可以从 example 配置生成本地配置，不覆盖用户已有配置。
7. 必须提供并验证：

```text
scripts/check_deps.sh
scripts/bootstrap_local_config.sh
scripts/build.sh
scripts/test.sh
scripts/run_cluster_local.sh
scripts/stop_cluster_local.sh
scripts/smoke_test.sh
scripts/final_e2e.sh
```

8. CI 至少包括：
   - Debug 全量构建和单元测试。
   - Release 全量构建。
   - ASan/UBSan。
   - TSan 或明确隔离的并发测试任务。
   - Shell 语法/静态检查。
   - proto 生成结果一致性检查。
9. `git diff --check` 必须通过；生成文件和文档中的尾随空格需要清理。
10. 必需集成测试缺少 Redis/MySQL/brpc 时必须失败；可单独保留不依赖服务的 low-level 测试任务。

## 9. 稳定版本必须具备的自动化场景

至少自动化以下真实运行场景：

1. 双 Gateway TCP E2E：同一个客户端通过 VIP/端口连接任一 GW，登录和业务请求成功。
2. Login：Auth → Session → 指定 GameLogic BindPlayer 全链路成功。
3. 身份安全：所有含 player_id 的请求不能伪造其他玩家。
4. 普通命令：登录后真实经过 GameLogicService.Dispatch。
5. GameLogic 分配：gl-0 和 gl-1 都能承载玩家和地图。
6. 进图：跨 GameLogic 切换完成 Bind/Freeze/Import/Cutover/Unbind，后续命令到新 Owner。
7. 双 GW 重连：断开 gw0，通过 gw1 Reconnect，旧连接迟到事件不能破坏新连接。
8. Push：实时 Push、精确 Replay、FullSnapshot 和 ACK 都闭环。
9. Session ×2：kill 当前 Session 后新登录/重连可继续，单一有效 fence 不被破坏。
10. Logic 故障：kill 当前 Logic 后，阻止旧 epoch 写；按明确策略恢复或要求重新进图。
11. GameDB 故障：资产写不重复，不错误返回成功。
12. 动态发现：运行时新增 gl-2，无需重启控制面即可承载新地图。
13. 背压：慢客户端、玩家队列满、RPC 超时和 DB 队列满均有明确拒绝策略。
14. 协议安全：半包、粘包、非法长度、超大帧、超速请求和未登录请求均按预期处理。

测试脚本不能只检查进程或 HTTP 存活，必须验证业务响应、状态变化和错误码。

## 10. 稳定版本发布门禁

在宣布“服务器稳定版本完成”前，必须实际执行并保存结果：

```bash
./scripts/check_deps.sh --full
./scripts/bootstrap_local_config.sh
./scripts/build.sh Debug
./scripts/build.sh Release
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/final_e2e.sh
```

并执行可用的：

```text
ASan
UBSan
TSan/并发专项测试
至少 20 轮核心 E2E 重复测试
至少 2 小时基础 soak test
Gateway / Session / GameLogic / GameDB kill drill
```

稳定版本判定：

- 全量 Debug/Release 构建成功。
- 必需单元、集成和 E2E 测试零失败、零伪 SKIP。
- Sanitizer 无未解释错误。
- 核心 E2E 连续 20 轮通过。
- soak test 无持续内存增长、线程泄漏、连接泄漏或错误率恶化。
- 没有未处理的 Critical/High 级正确性问题。
- 所有已知限制均写入 Release Notes。
- 给出建议版本号，例如 `server-stable-v0.1.0-rc1`，但未经用户授权不要创建 tag 或 push。

如果任一门禁无法执行或失败，最终结果必须写成“稳定候选未通过”，列出阻塞和复现命令，不能宣称稳定。

## 11. 工作包一交付格式

完成后输出：

1. 当前 commit 和工作区状态。
2. 修改文件列表。
3. 修复的 P0/P1 问题及实现位置。
4. 最终登录、进图、重连、Push 和资产写链路。
5. 实际执行的构建、测试、E2E、Sanitizer 和 soak 命令。
6. 每个命令的真实结果和日志位置。
7. 仍存在的单点和已知限制。
8. 是否满足稳定版本门禁：`PASS` 或 `BLOCKED`。
9. 建议版本号、Release Notes 和回滚方式。

工作包一输出后停止，等待用户确认再执行工作包二。

---

# 工作包二：为 Unity 客户端接入做好准备

## 1. 工作包前提与目标

只有工作包一稳定版本门禁通过后才能开始。

目标不是在本阶段制作完整 MMO 客户端，而是交付一套 Unity 可直接使用、协议行为清晰、能够自动联调的客户端接入基础：

- 冻结并版本化外部 TCP 协议。
- 生成稳定的 C# Protobuf 代码。
- 提供 Unity Runtime SDK。
- 提供最小 Sample 和接入文档。
- 用真实双 GW 环境验证登录、进图、Push 和跨 GW 重连。

如果仓库中尚无 Unity 工程，不要生成 `Library/`、`Temp/` 等大型 Unity 缓存。请先在 `client/unity/` 下交付可导入的 UPM Package、示例源码和联调工具。

## 2. 冻结并版本化客户端协议

审查 `game.proto` 和 TCP 帧格式，建立明确的外部协议契约。

### 2.1 帧格式

文档和代码必须明确：

- 长度字段字节数和大端/小端规则。
- 长度是否包含 header 自身。
- 最大合法帧大小。
- 半包、粘包和多帧解析行为。
- 非法长度和未知消息的关闭/错误策略。
- 心跳和超时规则。

Unity SDK 与 C++ Gateway 必须共享一组 golden framing vectors，分别测试相同字节序列。

### 2.2 协议版本与握手

客户端首次连接或登录请求至少携带：

```text
protocol_version
client_build
platform
device_id
```

服务器返回：

```text
server_version
min_supported_protocol
max_supported_protocol
recommended_client_build
```

版本不兼容时返回稳定错误码，不能只返回文本。

### 2.3 结构化错误码

所有客户端可见响应统一包含：

```text
request_id / client_seq
error_code
error_message（仅用于显示和诊断）
payload
```

新增正式错误码枚举和文档，至少分类：

- 协议/版本错误。
- 未认证和 Session 过期。
- 旧 fence、旧 epoch、旧 route。
- 请求参数错误。
- 过载、限流和超时。
- 依赖不可用。
- 需要重连、重新进图或全量同步。
- 业务错误。

Unity 只能依据 `error_code` 做分支，不解析服务器错误文本。

### 2.4 Protobuf 兼容规则

- 已发布字段号不得改变含义或复用。
- 删除字段使用 `reserved`。
- 新字段默认向后兼容。
- 记录协议版本与 Server/Unity SDK 的兼容矩阵。
- 客户端协议中不得出现 brpc、内部地址、GameLogic 实例 ID 决策权或数据库字段。

### 2.5 响应与推送分离

明确区分：

- 请求响应：通过 `request_id/client_seq` 关联。
- 服务器主动 Push：使用 `ServerPushEnvelope`。

`ServerPushEnvelope` 至少包含：

```text
server_seq
message_type
payload
reliable
server_time_ms
```

FullSnapshot 必须有明确类型、snapshot version 和 baseline server_seq。

## 3. 提供确定性的 C# Protobuf 生成流程

新增或完善：

```text
scripts/generate_unity_proto.sh
client/unity/GameMesh.Unity/Runtime/Generated/
```

要求：

1. 固定或检查 `protoc` 版本。
2. 使用仓库中的权威 proto 生成 C#，禁止手写重复 DTO。
3. 脚本不依赖调用者当前目录。
4. 生成结果可重复；连续执行两次不产生 diff。
5. CI 重新生成并检查仓库中的 C# 文件是否过期。
6. 输出 proto descriptor/hash，方便服务器与客户端确认协议一致。
7. 生成层不直接依赖 `UnityEngine`，便于在普通 .NET 测试中复用。

如果 C# Protobuf 运行库通过 UPM/NuGet 引入，请固定兼容版本并在 README 中写清安装方式。

## 4. 创建 Unity Runtime SDK

建议目录：

```text
client/unity/GameMesh.Unity/
  package.json
  Runtime/
    Transport/
    Protocol/
    Session/
    Push/
    Generated/
  Tests/
    EditMode/
  Samples~/
  README.md
```

根据仓库实际结构调整，不要机械创建重复目录。

SDK 至少提供以下职责：

### 4.1 TCP 与 FrameCodec

- 使用异步 Socket/NetworkStream，不在 Unity 主线程阻塞读写。
- 正确处理半包、粘包、断开、超时和 CancellationToken。
- 单读循环、单写队列，写队列有界。
- 4 字节长度等具体规则必须与服务器 golden vector 一致。
- 超大帧和非法帧立即失败并关闭连接。

### 4.2 RequestDispatcher

- 为每个请求生成单调 `client_seq/request_id`。
- 使用 `TaskCompletionSource` 或等价机制关联响应。
- 支持超时、取消和连接断开清理。
- 迟到响应不能完成已经属于新连接代次的请求。
- 回调进入 Unity 主线程前不直接操作 GameObject。

### 4.3 SessionContext

维护：

```text
player_id
access_token / reconnect_ticket
session_id（如果外部协议需要）
last_applied_server_seq
connection_generation
```

客户端不保存或决定 GameLogic ID、Map Owner 或 epoch 路由。

敏感 Token 不要默认明文长期保存在 PlayerPrefs。提供 `ISecureTokenStore` 抽象；开发示例可以使用内存实现，并明确生产接入平台安全存储。

### 4.4 ReconnectController

- 断线后使用指数退避和 jitter。
- 连接稳定 VIP/域名，不绑定 gw0/gw1 内部实例地址。
- Reconnect 携带服务器要求的票据和 `last_applied_server_seq`。
- 新连接成功后提升本地 generation，旧连接回调全部失效。
- 区分可重连错误、需要重新登录、需要重新进图和必须升级客户端。

### 4.5 PushDispatcher

- 解析 `ServerPushEnvelope`。
- 按 `server_seq` 去重和顺序应用。
- 检测 gap，不得静默跳过可靠消息。
- 业务状态成功应用后才发送 PushAck。
- FullSnapshot 必须原子替换客户端基线状态，再更新 ack baseline。
- 未知 Push 类型记录并按协议兼容策略处理，不能导致读循环崩溃。

### 4.6 UnityMainThreadDispatcher

- 网络线程只产生纯 C# 数据和事件。
- 所有 UnityEngine 对象操作回投主线程。
- 主线程队列有上限和每帧处理预算，避免 Push 洪峰卡死一帧。

## 5. 提供最小 Unity 示例

在 `Samples~/` 提供一个可导入的最小示例，不要求制作正式游戏画面。

示例至少展示：

- 服务器地址和端口配置。
- Connect / Disconnect。
- Register / Login。
- EnterMap。
- 查询或展示玩家基础状态。
- Grant/ConsumeItem 或当前已有资产操作。
- ReleaseSkill 或当前已有战斗命令。
- 实时 Push 日志。
- 主动断网和 Reconnect。
- 当前 session、last_server_seq、网络状态和结构化错误码。

提供清晰 README：

1. 支持的 Unity/.NET API Level。
2. 导入 UPM Package 的步骤。
3. 生成 proto 的命令。
4. 启动本地服务器集群的命令。
5. 打开 Sample 并运行的步骤。
6. 常见连接、版本和证书问题。

更新 `.gitignore`，排除 Unity 的 `Library/`、`Temp/`、`Logs/`、`Obj/`、构建产物，但保留必要的 `.meta` 文件。

## 6. Unity 客户端自动化测试

### 6.1 不依赖 Unity Editor 的 SDK 测试

尽可能让纯 C# 网络/协议层可以通过普通 .NET 测试执行，至少覆盖：

- FrameCodec golden vectors。
- 半包、粘包、多帧和非法帧。
- request/response seq 关联。
- 请求超时与断线取消。
- 旧 connection generation 响应被忽略。
- Push 去重、乱序、gap 和 ACK。
- FullSnapshot baseline。
- 重连退避状态机。

### 6.2 Unity EditMode/PlayMode 测试

- 主线程事件回投。
- Sample 组件生命周期。
- 场景销毁或 Domain Reload 时正确取消网络任务。
- 多次 Connect/Disconnect 不泄漏线程和 Socket。

如果执行环境没有 Unity Editor：

- 纯 C# 测试仍必须执行。
- Unity batchmode 测试必须明确标记为阻塞项并给出执行命令，不能显示为成功或静默 SKIP。

## 7. 双 GW Unity 端到端联调

新增或完善：

```text
scripts/start_unity_test_cluster.sh
scripts/stop_unity_test_cluster.sh
scripts/test_unity_protocol.sh
scripts/test_unity_reconnect.sh
```

脚本复用服务器稳定版本集群，不复制第二套服务启动逻辑。

至少完成以下真实场景：

1. Unity/同 SDK 控制台客户端通过唯一公网测试入口连接 Gateway。
2. Register/Login 成功，错误密码和不兼容版本返回结构化错误。
3. EnterMap 成功，客户端不感知 GameLogic 实例。
4. Grant/ConsumeItem、ReleaseSkill 等命令通过 seq 得到正确响应。
5. 收到并解析真实 `ServerPushEnvelope`，应用后发送准确 ACK。
6. 断开 gw0，通过 gw1 Reconnect，使用 `last_applied_server_seq` 精确补发。
7. 制造 Replay gap，客户端收到并原子应用 FullSnapshot。
8. 旧 session、旧连接和旧 fence 的请求被拒绝，不能污染新连接。
9. kill 当前 GameLogic 后，客户端收到明确的重试/重新进图语义，不出现假成功。
10. 模拟延迟、丢包、拆包、粘包和慢读取，SDK 不死锁、不无限积压。
11. 使用同一 SDK 的无界面测试客户端并发至少 100 个连接做基础 soak，记录成功率和延迟。

E2E 必须断言具体响应、错误码、Push 类型、序号和最终玩家状态，不能只判断“收到任意帧”或“进程仍存活”。

## 8. Unity 接入准备门禁

完成后至少实际执行：

```bash
./scripts/generate_unity_proto.sh
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/test_unity_protocol.sh
./scripts/test_unity_reconnect.sh
```

如果环境中提供 Unity Editor 路径，再执行 Unity batchmode EditMode/PlayMode 测试。

Unity 准备完成判定：

- 外部协议有版本、结构化错误码和兼容规则。
- C# Protobuf 可重复生成且 CI 检查无 diff。
- SDK 的 TCP、请求关联、重连、Push、ACK 和 FullSnapshot 都有自动测试。
- Sample 可以连接双 GW 本地集群完成核心流程。
- 跨 GW 重连断言精确 server_seq，而不是猜值。
- 客户端无需知道 GameLogic、GameDB 或 brpc。
- 必需测试零失败；缺 Unity Editor 时明确给出尚未执行的人工/CI 门禁。

建议在全部通过后准备版本：

```text
Server: server-unity-stable-v0.1.0-rc1
Unity SDK: com.gamemesh.client 0.1.0-rc.1
Protocol: v1
```

未经用户授权，不要创建 tag 或 push。

## 9. 工作包二交付格式

完成后输出：

1. 修改文件列表。
2. 冻结后的帧格式和协议版本。
3. 错误码清单及兼容策略。
4. C# Proto 生成命令和输出目录。
5. Unity SDK 模块和公开 API。
6. Sample 导入、启动和联调步骤。
7. 实际执行的 .NET、Unity、服务器集成和双 GW E2E 结果。
8. 未执行测试及其阻塞原因。
9. Unity 正式开发前仍存在的限制。
10. 建议 Server、Protocol 和 Unity SDK 版本号。
11. 回滚方式。

---

# 最终禁止事项

- 不要为了快速接 Unity 而绕过服务器稳定版本门禁。
- 不要让 Unity 客户端直接连接 GameLogic、Session、GameDB 或 etcd/Redis Registry。
- 不要在 Unity 中集成 brpc。
- 不要信任客户端自报的 player_id、路由、epoch 或 fence。
- 不要使用错误文本代替结构化错误码。
- 不要在 Unity 主线程执行阻塞 Socket 读写。
- 不要在服务器 Reactor I/O 线程执行同步内部 RPC 或数据库访问。
- 不要用本地单 Gateway 假测试代替双 GW TCP E2E。
- 不要用“收到任意响应”代替对 server_seq、message_type、error_code 和最终状态的精确断言。
- 不要把 Unity 的 `Library/`、`Temp/`、`Logs/` 或大型构建产物提交到仓库。
- 不要在门禁未通过时宣称“稳定版本”或“Unity 接入完成”。
