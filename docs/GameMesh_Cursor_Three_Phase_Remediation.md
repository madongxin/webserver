# GameMesh 分布式游戏服务器三阶段 Cursor 实施提示词

> 适用仓库：`madongxin/webserver` 当前 `main` 分支  
> 执行方式：严格依次把“阶段一、阶段二、阶段三”完整发送给 Cursor。每个阶段必须通过真实构建和测试门禁后，才能执行下一阶段。

## 总体目标

在保留现有 C++17、Reactor、ProtoFraming、brpc 和多进程拓扑的基础上，把当前工程从“多进程分布式骨架”完善为：

1. 分布式语义正确的游戏服务器 MVP。
2. 支持玩家跨 GameLogic 迁移、MapInstance 故障接管和跨 Gateway 重连。
3. 具备明确数据边界、动态服务发现、真实 HA 测试和生产化验收能力。

---

# 阶段一：修复分布式核心正确性和并发问题

请将下面整段发给 Cursor：

```text
你现在位于 GameMesh 项目根目录，仓库为 madongxin/webserver，请基于当前 main 最新代码继续修改。

本阶段目标：修复当前分布式主链路中的 P0 正确性、并发安全、身份校验和 Map 所有权问题，使 Gateway、Session、GameLogic 多实例能够安全处理真实并发请求。

一、执行约束

开始前必须：

1. 完整阅读 AGENTS.md、README、架构文档、proto、CMake 和测试脚本。
2. 执行 git status，保留现有未提交修改。
3. 根据当前代码定位实现，不要机械依赖旧文档中的行号。
4. 直接修改代码、proto、配置、脚本和测试，不要只输出方案。
5. 使用 C++17、现有 Reactor、ProtoFraming 和 brpc。
6. 不引入 gRPC，不重写整个 Reactor，不改变现有进程拓扑。
7. 不执行破坏性 Git 操作，不提交、不推送，除非我另行要求。
8. 不用假测试、空实现、固定 sleep 或静默 SKIP 让测试通过。
9. 本阶段通过验收前，不开始玩家状态迁移、可靠 Push 和生产化改造。

二、实现 GameLogic 玩家串行执行

当前普通 Dispatch 请求可能直接进入 gameproto::HandleFrame，必须建立真正的玩家串行执行边界。

在 GameLogic 侧实现有界的 PlayerSerialQueue 或 PlayerActorMailbox：

brpc GameLogicService.Dispatch
→ 基础协议校验
→ Fence 校验
→ 投递 player_id 对应队列
→ 串行执行游戏逻辑
→ 完成 brpc callback

要求：

- 同一玩家的命令严格串行。
- 不同玩家允许并行。
- 禁止用一个全局大锁串行所有玩家。
- 每玩家队列和全局待处理请求数都必须有上限。
- 队列满时返回明确的过载错误，不允许无限占用内存。
- 玩家 Unbind/Logout 时安全关闭队列，并正确处理在途请求。
- brpc done 必须恰好调用一次。
- request、response、controller、callback context 生命周期覆盖异步执行过程。
- 不得在 Reactor I/O 线程等待 GameLogic 执行完成。

为 client_seq 实现策略：

- 记录每个有效 Session 的最后已执行 client_seq。
- 重复序号必须幂等返回或明确拒绝，不能重复修改状态。
- 对乱序请求定义明确策略：小窗口缓存或返回 ERR_CLIENT_SEQ_OUT_OF_ORDER。
- 新 session/fence 生效时重置对应序号状态。
- 旧 session 的迟到命令必须被拒绝。

三、收敛可信玩家身份

Gateway 连接绑定信息是可信身份来源，客户端 payload 不是。

Gateway 构造 ClientCommand 时必须使用连接绑定的：

player_id
session_id
fence_token
generation
gamelogic_instance_id
map_instance_id
map_owner_epoch
route_version

要求：

- 客户端不能覆盖这些字段。
- 如果现有业务 GameRequest 内仍携带 player_id，在进入业务 Handler 前统一覆盖为可信值，或者严格验证它等于 ClientCommand.player_id。
- 不一致时返回安全错误并记录指标。
- Register、Login、Reconnect 是未登录连接白名单。
- 其他游戏命令必须要求连接已认证并绑定。
- Logout、EnterMap、物品和资产操作也不能信任 payload 自报身份。

四、校验本机 GameLogic 实例

BindPlayer、Dispatch、PrepareEnterMap 或等价接口必须验证：

request.gamelogic_instance_id == local_gamelogic_instance_id

要求：

- 请求发错实例时返回 ERR_WRONG_GAMELOGIC_OWNER。
- 不允许未知 Logic ID 退化到地址列表中的第一个实例。
- 错误响应可携带当前已知 Owner 和 route version，供 Gateway 刷新路由。
- MapInstanceRegistry 的本机实例 ID 必须来自配置或服务注册，不得硬编码。

五、Map 所有权和 Lease 必须 fail-closed

修复 lease_until_unix == 0 被视为无限有效的问题。

正式分布式模式必须满足：

- Placement 不存在：拒绝写入。
- Lease 缺失或为 0：拒绝写入。
- Lease 已过期：拒绝写入。
- Owner 不等于本机：拒绝写入。
- owner_epoch 不一致：拒绝写入。
- route_version 过期：要求刷新路由。
- Placement state 不是 READY：拒绝普通游戏写入。

首次创建 MapInstance 也必须先由权威 Placement 原子创建记录，然后 GameLogic 才能 Claim，不能由任意 Gateway 元数据直接创建本地 Owner。

GameLogic 处理 EnterMap 前，应从权威 Placement 或可信缓存快照校验：

map_instance_id
owner_gamelogic_instance_id
owner_epoch
route_version
state
lease_until

Redis/Placement 暂时不可用时，正式模式必须 fail-closed。开发单体模式如需兼容，必须通过明确配置开关隔离。

六、修复 brpc Channel 热更新生命周期

当前服务地址热更新不能让在途 RPC 持有悬空的 brpc::Channel*。

统一改为安全生命周期方案，例如：

shared_ptr<const ChannelSnapshot>
instance_id → shared_ptr<brpc::Channel>

要求：

- RPC 发起前取得 Channel 的强引用。
- 在途 RPC 完成前 Channel 不会析构。
- 服务发现更新时构建新快照，然后原子替换。
- 旧快照在最后一个使用者完成后自动释放。
- Gateway、Session、GameLogic、GameDB、GatewayPush 客户端统一使用此机制。
- 禁止返回解锁后可能被并发删除的裸 Channel 指针。
- 增加并发更新和调用测试，建议在 TSan 下验证。

七、修正 Session RPC 自动重试

Session 状态修改接口不能无条件使用 max_retry=2。

分类处理：

- 纯查询：可有限自动重试。
- AcquireSession：默认 max_retry=0，或增加 operation_id 幂等。
- Reconnect：默认 max_retry=0，或增加 operation_id 幂等。
- MarkDisconnected：条件更新、幂等。
- Logout：条件更新、幂等。
- Placement Create/Migrate：必须带 operation_id 和 CAS。

如果实现自动重试，必须：

- 请求包含稳定的 operation_id/idempotency_key。
- Redis Lua 存储同一 operation 的执行结果。
- 重试返回同一个 session/fence/generation，不能再次生成新会话。
- 超时但结果未知时提供查询操作结果的接口。

八、测试要求

新增真实测试，至少覆盖：

1. 同一玩家 100 个并发命令严格按序执行。
2. 两个玩家的命令可以并行。
3. 玩家队列达到上限后明确拒绝。
4. 重复 client_seq 不会重复扣物品或修改状态。
5. 乱序 client_seq 按设计拒绝或缓存。
6. payload player_id 与连接绑定不一致时被拒绝。
7. 请求发送到错误 GameLogic 实例时被拒绝。
8. Placement 不存在时 GameLogic 不能自行成为 Owner。
9. lease 为 0、已过期、epoch 过期时写请求被拒绝。
10. Channel 快照热更新期间并发 RPC 不崩溃、不 UAF。
11. AcquireSession 超时重试不会生成两个有效 Session。
12. 旧 Gateway 断开不会删除新 Gateway 的连接索引。
13. Reactor I/O 线程不存在同步 brpc、Redis、MySQL 等待。

替换以下无效测试：

- 只使用局部变量模拟连接 ID 的测试。
- Expect(true, "...") 之类占位断言。
- 只验证 HTTP 进程存活、没有验证业务链路的测试。

九、阶段门禁

实际执行仓库已有的等价命令，例如：

./scripts/check_deps.sh
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration

如果有 sanitizer 环境，再执行：

./scripts/test.sh tsan
./scripts/test.sh asan

完成后输出：

1. 修改文件清单。
2. 玩家串行队列的实现位置和线程模型。
3. 身份、fence、lease、owner 校验链路。
4. Channel 生命周期方案。
5. Session 重试和幂等策略。
6. 实际执行的命令和真实结果。
7. 未通过的测试和具体阻塞。

任何必需测试未运行或被跳过，都不能声明本阶段完成。通过本阶段门禁后停止，等待我发送第二步。
```

---

# 阶段二：补齐状态迁移、自动故障恢复和可靠 Push

请将下面整段发给 Cursor：

```text
继续修改 GameMesh 当前工程。本阶段只能在第一步所有必需测试通过后开始。

本阶段目标：让 GameLogic 通用节点池真正支持玩家跨节点迁移、MapInstance Owner 故障接管，以及跨 Gateway 重连后的可靠 Push。

一、执行约束

1. 阅读 AGENTS.md 和第一步的修改记录。
2. 执行 git status，保留现有修改。
3. 继续使用 C++17、Reactor、ProtoFraming 和 brpc。
4. 不引入 gRPC，不新增无必要的服务进程。
5. 不允许只修改 Redis 字段来伪装故障恢复成功。
6. 不允许测试客户端猜测 server_seq。
7. 不提交、不推送，除非我另行要求。
8. 阶段验收失败时停止并输出真实阻塞。

二、实现玩家跨 GameLogic 状态迁移

当前跨节点 EnterMap 不能只迁移 Session 路由和玩家绑定，必须迁移玩家运行状态。

定义版本化快照，例如：

message PlayerTransferSnapshot {
  uint64 player_id = 1;
  bytes session_id = 2;
  uint64 fence_token = 3;
  uint64 generation = 4;

  string source_gamelogic_id = 5;
  string target_gamelogic_id = 6;
  uint64 source_map_instance_id = 7;
  uint64 target_map_instance_id = 8;
  uint64 target_owner_epoch = 9;
  uint64 route_version = 10;

  uint64 snapshot_version = 11;
  bytes state_payload = 12;
  bytes checksum = 13;
}

快照至少覆盖项目当前已有的运行状态：

- 位置、朝向、地图信息。
- HP/MP 和战斗状态。
- 技能冷却和 Buff。
- 背包及运行期资产版本。
- 任务或其他必要的玩家内存状态。
- 已处理 client_seq 和 server_seq 基线。

实现明确的迁移状态机：

PREPARE
→ FREEZE_SOURCE
→ EXPORT_SNAPSHOT
→ IMPORT_TARGET
→ TARGET_READY
→ SESSION_CUTOVER
→ GATEWAY_ROUTE_UPDATE
→ SOURCE_UNBIND
→ COMPLETED

要求：

- 每一步带稳定 transfer_id/idempotency_key。
- Source Freeze 后拒绝新的状态写。
- Export 快照包含版本和 checksum。
- Target 导入必须幂等。
- Target Ready 前不能切换 Session 路由。
- Session Cutover 使用 route_version/epoch CAS。
- Gateway 更新本地连接路由后才能继续 Dispatch。
- 成功后再 Unbind Source。
- 任意步骤失败必须有明确回滚或恢复策略。
- Gateway 在迁移过程中崩溃，迁移仍能根据 transfer record 继续或回滚。
- 快照失效时可以从 GameDB 持久化快照恢复，但不能静默创建空玩家状态。

三、实现自动 Placement 故障恢复

建立后台 Lease/Scheduler 机制，而不是只依赖脚本手工调用 MarkRecovering/Migrate。

要求：

- GameLogic 周期性续租其拥有的 MapInstance。
- Placement 后台扫描过期 Lease。
- 发现 Owner 失联后原子标记 RECOVERING。
- 停止向失联实例分配新玩家和地图。
- Scheduler 选择健康的新 GameLogic。
- 新 Owner 通过 CAS 获得更高 owner_epoch 和 route_version。
- 新 Owner 从迁移快照、周期快照或 GameDB 恢复。
- Ready 后再切换 Placement 为 READY。
- 旧 Owner 恢复后因 epoch 过期无法继续写。
- 暂不支持无损实时地图恢复时，必须显式返回“重新进入地图/恢复到安全点”，不能假装无损恢复。

状态机至少包括：

READY
DRAINING
FROZEN
RECOVERING
CLAIMING
READY
FAILED

为 Placement 操作增加事件审计，记录旧 Owner、新 Owner、epoch、原因和恢复结果。

四、实现客户端可见的可靠 Push 序号

内部 Push 有 server_seq 还不够，客户端协议必须能收到并确认它。

在保持兼容的前提下新增外层消息，例如：

message ServerPushEnvelope {
  uint64 server_seq = 1;
  uint32 message_type = 2;
  bytes payload = 3;
  bool reliable = 4;
  bool coalescable = 5;
}

客户端 ACK 至少包含：

session_id
fence_token 或 generation
ack_server_seq

要求：

- server_seq 按玩家有效 Session 单调递增。
- 客户端不能猜测 server_seq。
- Gateway 只接受当前 session/generation 的 ACK。
- 重复 ACK 幂等。
- 旧 Session ACK 不得删除新 Session 的消息。
- reliable 消息进入重放存储。
- coalescable 消息允许按业务 key 合并。
- 缓存出现缺口时返回明确的 FULL_SNAPSHOT_REQUIRED。
- 真正生成并发送全量状态快照，而不是只返回一个标志。
- 发送队列、单批消息数量和字节数都有上限。
- 慢客户端达到上限后执行明确的降级或断开策略。

五、修复 Replay Store 的会话隔离

可靠 Push 的 Redis key 不能只使用 player_id。

至少使用：

player_id
session_id 或 generation

要求：

- 新 Session 不会收到旧 Session 的私有可靠消息。
- 顶号后旧 Session 的 replay/ACK 立即失效。
- 跨 Gateway 重连仍能访问相同的 Replay Store。
- GameLogic 或 Gateway 重启后 server_seq 不回退。
- Replay 清理使用 ACK 水位和 TTL。
- 对资产变更等重要消息，消息重放和全量状态同步结果必须一致。

六、真实业务 Push

让至少一个现有游戏功能真实调用 Push，例如：

- 物品变更通知。
- EnterMap 成功后的地图状态。
- 邮件到达。
- 战斗结果或其他适合的现有业务。

要求：

GameLogic
→ 根据 gateway_instance_id 选择唯一 Gateway
→ PushBatch
→ Gateway 校验 player/session/generation
→ 回投连接所属 EventLoop
→ Client 接收 Envelope
→ Client ACK

不允许每条 Push 广播两个 Gateway。

七、测试要求

新增真实多进程测试：

1. 玩家从 gl-0 迁移到 gl-1 后状态、位置和资产版本保持一致。
2. 迁移中产生的新命令被暂停或明确拒绝，不发生双写。
3. Source 导出后崩溃，Target 能继续恢复或明确回滚。
4. Target 导入后、Session Cutover 前崩溃，不会出现双 Owner。
5. Gateway 在迁移中崩溃，迁移记录可恢复。
6. kill 当前 Map Owner 后自动选出新 Owner。
7. 新 owner_epoch 大于旧 epoch。
8. 旧 Owner 恢复后写请求被拒绝。
9. 客户端收到真实 server_seq 并发送 ACK。
10. gw0 断线后连接 gw1，能重放缺失的 reliable Push。
11. E2E 必须确认重放的是断线期间缺少的具体序号，不能只检查收到任意 Push。
12. Replay 缓存不足时，客户端收到并完成全量状态同步。
13. 旧 Session 的消息和 ACK 不会影响新 Session。
14. 慢客户端触发背压策略，服务器内存不会无限增长。
15. 真实网络隔离下触发 Lease 过期和自动恢复。

网络隔离测试不得仅依赖 GAMEMESH_FORCE_NOT_READY 变量模拟，需要使用容器网络、iptables、toxiproxy 或仓库适合的真实故障注入方式。

八、阶段门禁

实际运行：

./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/test_dual_gateway_e2e.sh
./scripts/kill_logic_drill.sh
./scripts/network_partition_drill.sh

如果脚本名称不同，增强并运行现有等价入口。

验收必须使用：

Gateway ×2
Session ×2
GameLogic ×2
GameDB ×2
Redis
MySQL

完成后输出：

1. 状态迁移协议和状态机。
2. 自动 Placement 故障恢复实现位置。
3. Push Envelope、ACK 和 Replay Store 结构。
4. 双 Gateway E2E 的真实日志摘要。
5. kill/partition 演练结果。
6. 失败和数据丢失边界。
7. 仍未实现的恢复能力。

任何跨节点迁移、自动接管或 Push 重放测试未真实执行，都不能声明本阶段完成。完成后停止，等待我发送第三步。
```

---

# 阶段三：收敛 GameDB、动态发现和生产级验收

请将下面整段发给 Cursor：

```text
继续修改 GameMesh 当前工程。本阶段只能在前两步验收通过后开始。

本阶段目标：收敛资产数据边界，完成 GameDB 多实例、动态服务发现、安全、部署、压力测试和生产验收，使项目达到可部署的分布式游戏服务器版本。

一、执行约束

1. 阅读 AGENTS.md、前两阶段交付记录和当前架构文档。
2. 执行 git status 并保留所有现有修改。
3. 使用 C++17、Reactor、ProtoFraming、brpc。
4. 不引入 gRPC，不允许正式模式绕过 GameDB 直连 MySQL。
5. 不允许把 HTTP 存活检查当成业务 HA 测试。
6. 不允许关键 E2E 因缺少工具而 SKIP 后返回成功。
7. 不提交、不推送，除非我另行要求。

二、扩充 GameDB 玩家和资产接口

当前 GameDB 不能只提供账号和邮件接口。

增加版本化、幂等的玩家数据接口，例如：

service GameDBService {
  rpc LoadPlayer(LoadPlayerRequest) returns (LoadPlayerResponse);
  rpc LoadInventory(LoadInventoryRequest) returns (LoadInventoryResponse);
  rpc ApplyAssetMutation(AssetMutationRequest) returns (AssetMutationResponse);
  rpc SavePlayerSnapshot(SavePlayerSnapshotRequest) returns (SavePlayerSnapshotResponse);
  rpc FlushPlayer(FlushPlayerRequest) returns (FlushPlayerResponse);
}

资产修改请求至少包含：

player_id
idempotency_key
expected_version
mutation_type
mutation_payload
trace_id

要求：

- 货币、背包、奖励、扣除在 MySQL 事务内执行。
- 使用 expected_version 防止并发覆盖。
- idempotency_key 有数据库唯一约束。
- 重复请求返回第一次执行结果。
- 资产写和 Outbox 在同一事务。
- 事件发布采用 at-least-once，消费者幂等。
- 正式模式下 GameLogic 不再使用本地 MySQL 持久化队列作为事实源。
- GameLogic 内存状态只作为运行缓存，以 GameDB 返回版本为依据。
- GameDB 不可用时资产写必须明确失败或进入受控降级，不能 fail-open。
- 本地单体兼容路径必须由显式构建或配置开关隔离。

三、实现 GameDB 真正多实例 HA

修复 Auth/GameLogic/GlobalService 只取 gamedb_addrs 第一个地址的问题。

要求：

- 支持多个 GameDB 地址和动态实例。
- 使用 brpc NamingService、Channel 快照或等价安全机制。
- 只对幂等请求有限重试。
- 非幂等资产写不得由框架盲目自动重试。
- 实例进入 DRAINING 后停止接收新请求。
- 健康检查区分 live 和 ready。
- kill 当前正在处理业务的 GameDB 后，客户端真实业务能切换到另一实例。
- 测试必须执行 LoadPlayer、资产修改和幂等重试，不能只检查另一个进程的 HTTP /api/version。

四、完成动态服务发现

实现 etcd v3 Lease/KeepAlive/Watch，或使用经验证能满足相同语义的 brpc NamingService。

注册信息至少包含：

service_name
instance_id
rpc_address
push_address
management_address
version
capacity
status
revision

要求：

- 区分 listen_addr 和 advertise_addr。
- 禁止注册 0.0.0.0 或不可路由的 loopback 地址。
- Lease 自动续租。
- Watch 支持新增、删除、状态和地址更新。
- 处理 etcd revision，断线重连后执行全量同步再继续 Watch。
- 优雅停服时先标记 DRAINING，再注销。
- ChannelManager 使用不可变 shared_ptr 快照，保证在途 RPC 生命周期。
- etcd 不可用时可使用最后有效快照和静态配置降级。
- 降级不能把仍有效的动态 Channel 清空。
- 业务 Handler 不直接解析 *_addrs 字符串。

验证动态扩缩容：

运行中增加 gl-2
→ 自动发现
→ 开始承载新 MapInstance
→ Gateway/Session 不重启

摘除 gl-2 时必须先 Drain 或触发明确的 Placement 恢复。

五、完善 Auth 和 Session 多实例安全

要求：

- 登录限流状态存入 Redis，多个 Auth 实例共享。
- 检查和记录失败次数必须使用同一份状态，不能使用两个独立静态 map。
- Token 使用密码学安全随机数生成器。
- access token 与 refresh token 分离。
- Token 只保存哈希或安全表示，并支持过期和撤销。
- Auth Redis 客户端使用线程安全连接池。
- 注册使用稳定 idempotency_key。
- device_id 或等价唯一标识必须有明确唯一约束和冲突策略。
- 日志不得记录密码、完整 Token、fence_token 或敏感配置。
- 管理端口只绑定管理网络或 loopback。
- 删除或构建隔离 demo 账号、测试崩溃入口和危险管理 API。

六、健康检查和优雅停服

统一实现：

/health/live
/health/ready

Ready 至少检查：

- 关键 brpc Channel。
- Redis。
- MySQL，仅 GameDB。
- 服务注册状态。
- 必要的 Placement/Session 依赖。
- 是否处于 DRAINING。

SIGTERM 流程：

标记 DRAINING
→ 从发现/LB 摘流
→ 停止新登录和新地图分配
→ 等待在途请求
→ Flush 必要状态
→ 超时后安全退出

Gateway 摘流时允许客户端重连其他 Gateway。GameLogic 摘流时迁移其玩家和 MapInstance。

七、构建、容器与 CI

提供可复现的工具链镜像，必须包含兼容版本的：

- CMake
- Protobuf/protoc
- brpc
- hiredis
- MySQL Client
- OpenSSL
- 测试工具

修复 Docker：

- 镜像内安装或构建依赖，不依赖宿主机挂载私有 .so。
- Gateway、Session、GameLogic、GlobalService、GameDB 能通过不同 command/entrypoint 正确启动。
- 不固定所有容器启动为 Gateway。
- 镜像中使用非 root 用户。
- 配置和密钥通过环境或 Secret 注入。

CI 至少包含：

Debug build
Release build
Unit tests
Integration tests
ASan
UBSan
TSan
ShellCheck
Formatting
Static analysis

CI 缺少必需依赖时必须失败并指出原因，不能把关键 job 标记成功。

八、真实压力和故障测试

重写当前只发送顺序 HTTP 请求的 load test。

GameTCP 压测至少支持：

- 并发 TCP 连接。
- Login/Reconnect。
- 高频 Dispatch。
- EnterMap。
- Push/ACK。
- 慢客户端。
- 半包、粘包和非法帧。
- 指定持续时间、并发数和消息速率。

记录：

- QPS。
- P50/P95/P99。
- 登录成功率。
- RPC 超时和错误码。
- EventLoop 延迟。
- 玩家队列深度。
- 在线玩家数。
- CPU、内存和网络。
- Redis/MySQL 延迟。
- Push replay/full snapshot 数量。

故障演练至少包括：

1. kill gw0，客户端通过 gw1 Reconnect。
2. kill session-0，真实 Login/Reconnect 继续成功。
3. kill gl-0，其 MapInstance 自动恢复到 gl-1。
4. 动态加入 gl-2，不重启 Gateway。
5. kill gamedb-0，真实资产操作经 gamedb-1 完成。
6. Redis/MySQL 短暂不可用时系统 fail-closed。
7. 网络分区恢复后不存在双 Owner 或旧 fence 写入。

九、替换假测试和宽松脚本

全仓库检查并处理：

- Expect(true, ...) 占位测试。
- 只测试局部变量、不调用真实类的测试。
- 只检查进程或 HTTP 存活的所谓 HA 测试。
- 固定 sleep 后假定服务完成的竞态测试。
- 关键工具缺失时 SKIP 并返回 0。
- 只检查收到任意 Push、没有校验具体 server_seq 的测试。
- 使用环境变量模拟网络分区，却没有真实断网的测试。

必需场景缺少依赖时，测试必须返回非零并输出安装或配置要求。

十、最终验收拓扑

至少使用：

L4 LB/VIP
Gateway ×2
Auth/Session ×2
GameLogic ×2，可动态增加 gl-2
GlobalService
GameDB ×2
Redis
MySQL
etcd

实际执行仓库等价命令：

./scripts/check_deps.sh
./scripts/build.sh Debug
./scripts/build.sh Release
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/test_final_e2e.sh
./scripts/load_test.sh
./scripts/kill_drill.sh

并执行可用的 ASan、UBSan、TSan 测试。

十一、最终交付

更新：

- Mermaid 拓扑图。
- 登录、重连、EnterMap、迁移和 Logout 时序。
- 服务和数据所有权表。
- Map Placement 状态机。
- Push ACK/Replay 协议。
- 部署和扩缩容手册。
- 故障演练 Runbook。
- 回滚方式。
- 当前仍存在的基础设施单点。

最终输出：

1. 修改文件列表。
2. 最终端到端调用链。
3. GameDB 资产一致性和幂等方案。
4. 动态发现和扩缩容测试结果。
5. 双 Gateway、双 Session、双 Logic、双 GameDB 的真实业务结果。
6. 压测数据。
7. sanitizer 和 CI 结果。
8. 已消除和仍存在的单点。
9. 未通过项目及明确原因。
10. 是否达到“分布式 MVP”“预生产”“生产可用”中的哪一级，并提供证据。

任何必需构建、业务 E2E、故障演练或压力测试没有实际运行，都不能声明生产化完成。
```

---

## 建议执行方式

1. 先把“阶段一”完整发送给 Cursor。
2. 检查 Cursor 提供的真实构建、测试结果和未完成项。
3. 阶段一门禁全部通过后，再发送“阶段二”。
4. 阶段二的双 Gateway、跨 GameLogic 迁移、故障接管和可靠 Push E2E 通过后，再发送“阶段三”。
5. 每阶段完成后单独建立 Git commit，便于审查、回滚和定位问题。

不要让 Cursor 一次执行全部三阶段。每个阶段完成后，应先审查代码差异和测试证据，再决定是否进入下一阶段。
