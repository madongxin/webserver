# GameMesh 最新服务器稳定性评估

## 1. 评估对象

- 仓库：`madongxin/webserver`
- 分支：`main`
- 最新提交：`f95fe7f1efd9dc6a2261906858f31db85101fd60`
- 最近稳定候选标签：`server-stable-v0.1.0-rc1`
- 标签指向提交：`b0fecdc664213a9745e9b846469808337163fa60`
- 评估目标：判断当前版本能否作为游戏服务器稳定版本
- 本轮范围：只评估服务器，不包含 Unity 客户端接入

## 2. 最终结论

**当前最新版本不能作为稳定服务器版本发布。**

当前状态：`STABLE BLOCKED`

它已经具备固定双 Gateway、双 Session、双 GameLogic、双 GameDB 的分布式服务器骨架，可以继续用于内部联调，但仍存在会影响地图故障恢复、同一玩家串行执行和资产正确性的 P0 问题。

完成本文列出的最小修复后，可以将其定义为：

> 固定双 Gateway、双 Session、双 GameLogic、双 GameDB 拓扑的稳定联调版本。

这仍不等同于生产级高可用 MMO 服务器，因为 Redis、MySQL 仍是单点，自动 Placement 恢复和有状态地图无损迁移也不在当前稳定范围内。

## 3. 当前已经完成的能力

- C++17 工程基础。
- Client 只连接 Gateway，内部继续使用 brpc。
- Gateway、Auth/Session、GameLogic、GlobalService、GameDB 多进程拆分。
- 固定双 Gateway、双 Session、双 GameLogic、双 GameDB 拓扑。
- 登录主链路：`Auth → Session → GameLogic.BindPlayer`。
- 普通游戏请求通过 `GameLogicService.Dispatch` 转发。
- 跨 Gateway 重连和 Session/GameDB 地址故障切换已有基础实现。
- Session 使用 Redis Lua 管理部分 Session、fence、generation 状态。
- Placement 已具备 `owner_epoch`、`route_version` 和 Lease 字段。
- Push Replay Cache 和可靠 Push 基础结构已经存在。
- 构建、E2E、负载、稳定门禁脚本已经形成基本框架。

## 4. 稳定版本阻塞项

### P0-1：死亡 GameLogic 的 Placement 路由可能被继续续租

相关实现：`game/PlacementStore.cpp`

当前 `ResolveOrCreate` 遇到状态为 READY、但 Lease 已过期的 Placement 时，会保留原 Owner 和 epoch，仅延长 Lease。

如果原 GameLogic 已经宕机，而稳定模式下又没有启用自动恢复调度器，玩家重新登录或重新进入地图时，可能继续得到已经死亡的 GameLogic 地址。

风险：

- 玩家无法重新进入地图。
- 请求持续发送给死亡节点。
- 重新登录无法完成最小故障恢复。
- 与发布文档中的“Logic 宕机后重新登录恢复”承诺不一致。

最小修复要求：

1. READY Lease 过期后不能由普通客户端请求直接给旧 Owner 续租。
2. 必须结合 GameLogic 存活信息判断旧 Owner 是否仍有效。
3. 旧 Owner 已死亡时，通过 CAS 选择存活的新 Owner。
4. 切换 Owner 时递增 `owner_epoch` 和 `route_version`。
5. 增加真实 E2E：kill 当前 Owner，不手工调用 `MarkRecovering`，玩家重新登录或进图后获得新 Owner。

### P0-2：PlayerSerialQueue 的链式异步操作会破坏同玩家串行性

相关实现：

- `runtime/PlayerSerialQueue.cpp`
- `game/MailService.cpp`

当前一次异步操作完成时，会释放该玩家的全部延迟任务。完成回调如果立即为同一玩家启动下一次异步操作，已经释放到主队列的任务仍可能继续执行。

结果是：同一玩家的普通命令可能与第二段异步邮件领取、数据库更新或状态应用同时运行。

风险：

- 同一玩家状态发生并发修改。
- `client_seq`、背包、邮件和在线状态出现竞态。
- Actor/玩家串行模型失效。

该问题已经通过最小复现测试触发，结果为：

```text
chain_serialization_violated=1
```

最小修复要求：

1. 将“当前异步完成”和“整个异步链结束”区分开。
2. 完成回调准备启动下一段异步时，保持该玩家处于 async-inflight 状态。
3. 只有整个异步链真正结束后，才释放同玩家延迟任务。
4. 增加“异步完成回调再次启动异步”的单元测试。
5. 验证不同玩家仍然可以并行执行。

### P0-3：邮件奖励与正式背包资产存在双数据源

相关实现：

- `db/GameDbAssetStore.cpp`
- `db/AsyncMysqlGameDbRepository.cpp`
- `game/MailService.cpp`
- `game/GameLogic.cpp`

正式资产路径使用：

```text
player_asset_meta
player_asset_bag
```

邮件领取奖励仍写入旧表：

```text
player_item
```

加载背包时，如果 `player_asset_bag` 非空，就不会使用旧表结果。玩家已有正式背包数据、领取邮件后又发生 GameLogic 崩溃或重新登录时，邮件已经标记为 CLAIMED，但奖励可能没有进入正式背包。

风险：

- 邮件已经领取，奖励却丢失。
- 新旧表之间产生永久不一致。
- 旧表 fallback 还可能恢复过期资产。

最小修复要求：

1. 邮件领取必须写入正式的 `player_asset_bag` 和 `player_asset_meta`。
2. 邮件状态、资产、幂等记录和 Outbox 必须在同一个 MySQL 事务中提交。
3. 移除运行期双数据源 fallback，或者提供明确的一次性迁移方案。
4. `LoadInventory` 使用一致性事务/快照读取 meta 和 bag。
5. 增加以下故障测试：
   - 玩家已有正式背包物品；
   - 领取邮件奖励；
   - 在响应前或响应后 kill GameLogic；
   - 重登后奖励只能出现一次且不能丢失。

### P0-4：Gateway 断线路径同步调用 brpc

相关实现：`game/GameTcpGateway.cpp`

TCP 连接断开回调中同步调用：

- `SessionService.MarkDisconnected`
- `GameLogicService.UnbindPlayer`

这些操作位于 Reactor/EventLoop 路径中。Session 或 GameLogic 超时、宕机或发生大规模断线时，网络线程可能被阻塞数秒。

风险：

- 大量连接不能及时读写。
- 断线风暴放大为 Gateway 级联故障。
- 一个后端故障拖慢整个 Gateway。

最小修复要求：

1. 断线回调只采集连接绑定快照并投递异步任务。
2. MarkDisconnected 和 UnbindPlayer 全部改成异步 brpc。
3. callback 不能直接跨线程操作 `TcpConnection`。
4. RPC 超时只记录和补偿，不能阻塞 EventLoop。
5. 增加 Session/Logic 不可用时的断线风暴测试。

### P1-1：可靠 Push 的 ACK 没有验证序列连续性

相关实现：`game/PushReplayStore.cpp`

当前 ACK 只检查目标序号是否存在，没有确认 `last_ack + 1` 到目标 ACK 之间的每一个序号都存在。

例如缓存中存在序号 1 和 3、缺少 2 时，客户端 ACK 3 仍可能被接受，从而把缺失消息永久跨过。

最小修复要求：

1. ACK 只允许推进到连续可达的最大序号。
2. 检测到缺口时返回 `NEED_SNAPSHOT` 或等价状态。
3. 客户端 E2E 必须解析真实 `server_seq`，不能把任何收到的帧都视为回放成功。
4. `replay_n == 0 && need_snapshot == false` 必须判定测试失败，不能只输出警告。

### P1-2：最新提交无法通过自己的稳定门禁

相关实现：`scripts/stable_gate.sh`

当前提交 `f95fe7f` 中的文档包含多处行尾空格，因此：

```bash
git show --check --oneline --no-renames HEAD
```

返回非零状态。

而稳定门禁本身正是先执行该命令，所以当前最新提交按项目自己的规则也是失败状态。

最小修复要求：

1. 清理行尾空格。
2. 在最终候选 commit 上重新执行完整稳定门禁。
3. 门禁报告必须对应最终 commit，不能只引用前一个提交的结果。
4. 保存构建、E2E、Load、Soak 和 Stable Gate 的机器可读报告。

## 5. 本次实际验证结果

### 已完成

- 成功拉取最新 `main@f95fe7f`。
- 仓库工作区保持干净。
- Shell 脚本语法检查通过。
- 可独立编译的 Reactor framing 测试通过。
- Reactor socketpair 集成测试通过。
- PasswordHash 测试通过。
- PushReplayCache 基础测试通过。
- PlayerSerialQueue 链式异步最小复现确认失败。
- 当前提交 `git show --check` 失败。

### 当前环境无法复现的项目

当前检查环境缺少完整构建所需的部分依赖，包括：

- CMake
- Protobuf/protoc
- brpc 开发依赖
- hiredis 开发依赖
- MySQL Client 开发依赖

因此没有把仓库文档中记录的历史 PASS 当作当前提交的独立验证结果。

## 6. 最小实施顺序

### 阶段一：数据与执行正确性

必须完成：

1. 修复 PlayerSerialQueue 链式异步串行性。
2. 统一邮件奖励和正式资产数据源。
3. 补齐资产事务、幂等与 kill/relogin 测试。

阶段门禁：

- 同一玩家任何时刻最多只有一条状态修改链运行。
- 邮件奖励在超时、重试、崩溃和重登后恰好出现一次。
- 资产版本、背包、幂等结果和 Outbox 原子提交。

### 阶段二：故障恢复与 Reactor 稳定性

必须完成：

1. 修复过期 Placement 对死亡 Owner 的软续租。
2. 实现 kill Logic 后重新选择 Owner 并递增 epoch。
3. Gateway 断线相关 RPC 全部异步化。
4. 增加断线风暴和 Logic 宕机 E2E。

阶段门禁：

- kill 当前 Owner 后，玩家无需人工修改 Redis 即可恢复登录/进图。
- 旧 epoch 的请求全部被拒绝。
- 后端超时不阻塞 Gateway Reactor。

### 阶段三：可靠 Push 与正式门禁

必须完成：

1. Push ACK 连续性检查。
2. E2E 客户端解析真实 `server_seq`。
3. 跨 Gateway 重连验证 replay 或 snapshot。
4. 修复最新提交的门禁错误。
5. 在同一个最终 commit 上执行完整稳定门禁。

阶段门禁：

```bash
./scripts/check_deps.sh
./scripts/build.sh Release
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/test_final_e2e.sh
./scripts/stable_gate.sh
```

所有必需测试必须返回 0；不能通过 WARN、SKIP、`|| true` 或删除断言隐藏失败。

## 7. 稳定版本验收标准

满足以下全部条件后，才建议创建新的稳定候选标签：

- [ ] 最新 commit 能通过 `git show --check`。
- [ ] Debug 和 Release 均可全新构建。
- [ ] 同玩家串行链测试通过。
- [ ] 邮件奖励/资产崩溃恢复测试通过。
- [ ] kill Logic 自动恢复 E2E 通过。
- [ ] 双 Gateway 登录和跨 Gateway 重连通过。
- [ ] Session、GameDB 单实例故障切换通过。
- [ ] Gateway 后端故障期间 Reactor 无明显阻塞。
- [ ] Push replay/snapshot 无静默丢失。
- [ ] 30 分钟负载测试达到既定成功率。
- [ ] 2 小时 Soak 无崩溃、无持续内存增长、无资产不一致。
- [ ] Stable Gate 报告与最终候选 commit 完全一致。

## 8. 版本定位建议

完成最小修复后，版本可以定位为：

```text
GameMesh 固定拓扑稳定联调版
Gateway ×2
Auth/Session ×2
GameLogic ×2
GameDB ×2
GlobalService ×1
Redis ×1
MySQL ×1
```

当前版本不应承诺：

- Redis/MySQL 高可用。
- 任意 GameLogic 动态扩缩容。
- Placement 全自动调度。
- 实时地图无损迁移。
- 大世界 Cell/Zone 无缝跨服。
- 生产级跨机房容灾。

这些能力应放在稳定联调版本之后继续建设，不应阻塞当前最小稳定版本，但必须在发布说明中明确边界。
