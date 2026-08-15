# GameMesh 服务器基础能力完善：Cursor 分阶段执行提示词

> 目标仓库：`https://github.com/madongxin/webserver`  
> 目标分支：`main`  
> 本次审计基线：`145a64753aacd0d9e1dc7916edee81d15f183148`  
> 配套 Unity 仓库：`https://github.com/madongxin/luna`

> 远端复核：2026-08-15 再次执行 `git pull --ff-only origin main` 和
> `git ls-remote origin refs/heads/main`，服务器远端仍为 `145a647`；如果执行时远端已有
> 更新，Cursor 必须以实际 HEAD 重新核验本文件中的每个缺口，禁止机械重复实现。

请把本文件整体交给 Cursor。Cursor 必须按 S0 → S1 → S2 → S3 顺序执行；每一阶段完成测试并提交阶段报告后才能进入下一阶段。不要一次性重写全部代码。

## 1. 最终目标

在保留当前分布式架构的基础上，补齐客户端正式联调和后续基础玩法开发所需的服务器能力：

- Gateway TCP + ProtoFraming 是唯一客户端入口。
- 内部继续使用 brpc，不引入 gRPC。
- Auth/Session、GameLogic、GlobalService、GameDB 保持现有职责。
- `game.proto` 成为唯一公网协议事实源，客户端不能维护分叉版本。
- 注册、登录、进图、移动、AOI、邮件、断线重连形成真实闭环。
- 增加协议握手、心跳、统一错误码、世界快照、AOI 重同步和最后安全位置。
- 公共地图严格执行每实例 50 人的既定策略。
- 所有关键能力有真实 TCP E2E、故障场景和发布门禁，不能用桩测试或日志文字代替。

## 2. 已确认的现状

当前版本已经具备：

- C++17 Reactor Gateway、brpc 内部 RPC。
- Auth/Session、GameLogic、GlobalService、GameDB 多进程。
- 登录、Session fence/generation、双 Gateway 重连和可靠 Push 基础设施。
- 玩家属性、地图静态数据、MapRuntime、AOI Enter/Move/Leave。
- 公共地图占位、50 人容量配置、玩家邮件发送和邮箱查询。
- `EnterMapRsp.aoi_snapshot`、`MoveReq/MoveRsp`、`AoiDelta`、`MailboxChangedNotify`。
- C++ TCP E2E 脚本和稳定门禁框架。

以下是本次审计确认的真实缺口，不要把它们当成已经完成：

1. Unity 仓库中的 `game.proto` 和生成代码仍落后于服务器版本，协议交付格式尚未形成强制门禁。
2. 公网协议没有 `ClientHello/ServerHello`、最小客户端版本和能力协商。
3. 没有正式心跳、服务器时间同步和空闲连接超时协议。
4. 顶层 `GameResponse` 只有 `ok/message`，业务错误码分散在部分子响应中。
5. `FullStateSnapshotRsp` 目前主要是背包快照，不足以恢复地图、角色位置和 AOI。
6. 玩家最后安全位置没有持久化，GameLogic 重启后通常回出生点。
7. AOI Push 出现序号缺口时，没有客户端可调用的权威 AOI/世界快照接口。
8. 公共地图测试只断言实例数不少于 2、单实例不超过 50；历史结果是 51 人分成 45+6，并未证明“先装满 50 再创建新实例”。
9. `player_profile` 依赖运行期 `CREATE TABLE IF NOT EXISTS`，缺少明确的版本化 SQL migration 和回滚。
10. 发布文档仍引用旧基线，并把“默认 stable gate 通过”与“完整稳定门禁通过”混在一起。
11. `ChatSend` 和 `FriendList` 仍是 `NOT_IMPLEMENTED` 占位；缺少玩家公开资料/在线状态查询接口。

## 3. 全局执行规则

开始前必须：

1. 阅读根目录 `AGENTS.md`、README、CMake、`proto/`、`scripts/`、配置和现有测试。
2. 执行：

   ```bash
   git status --short
   git rev-parse HEAD
   git log -5 --oneline
   ```

3. 如果 HEAD 已晚于审计基线，以当前代码为准，先核对缺口是否已经修复，禁止重复实现。
4. 保留用户未提交改动，不执行 `git reset --hard`、`git clean -fd`、强制 checkout 等破坏性命令。
5. 直接修改代码、proto、SQL、脚本、测试和文档，不要只输出建议。
6. 不提交、不 push，除非用户在 Cursor 会话中明确授权。
7. 不新增独立 Login 进程，不改成 gRPC，不推倒重写 Reactor。
8. Reactor I/O 线程中禁止同步 brpc、Redis、MySQL、文件 IO 和长耗时计算。
9. 客户端自报的 `player_id/session/fence/route/epoch` 不能成为可信身份；Gateway 必须用连接绑定覆盖或拒绝。
10. 所有 proto 修改只能追加新字段或新 message；禁止复用、改号或删除已发布字段，删除字段要 `reserved`。
11. 每阶段新增的失败场景必须 fail-closed，并返回稳定错误码。
12. 每阶段更新 `docs/release/server-client-foundation-status.md`，记录真实执行的命令、退出码和未运行项。

---

# S0：冻结协议事实源和可重复数据库升级

## S0.1 统一公网协议交付物

服务器 `proto/game.proto` 是唯一事实源。完善 `scripts/export_unity_protocol.sh`，输出：

```text
game.proto
game.desc
protocol_manifest.json
game.proto.sha256
game.desc.sha256
```

Manifest 至少包含：

```json
{
  "protocol_name": "gamemesh.game",
  "protocol_version": 1,
  "min_supported_protocol_version": 1,
  "server_git_sha": "...",
  "schema_file": "game.proto",
  "schema_sha256": "...",
  "descriptor_file": "game.desc",
  "descriptor_sha256": "...",
  "frame_format": "uint32_be_length_prefixed",
  "max_frame_bytes": 4194304,
  "package": "game",
  "csharp_namespace": "GameMesh.Protocol",
  "generated_at_utc": "..."
}
```

要求：

- 兼容读取当前 manifest 字段时可以保留旧别名，但新导出格式只能有一个权威定义。
- `schema_sha256` 必须是实际导出文件的 hash。
- `git_sha` 必须来自当前 HEAD，脏工作区要额外标记 `dirty=true`。
- 缺少 `protoc`、导出失败、hash 不一致时脚本非零退出。
- 增加 `scripts/check_public_protocol.sh`，验证：
  - `proto/game.proto` 与 `game/game.pb.*` 对应；
  - descriptor 能解析；
  - 已发布字段号未漂移；
  - C# namespace 保持 `GameMesh.Protocol`；
  - 必需类型存在。
- 必需类型至少包括当前联调批次的 Register/Login/Logout/Reconnect、PlayerAttributes、EnterMap、Move、AoiDelta、PlayerMailSend、MailboxChanged、ServerPushEnvelope。

## S0.2 固化兼容规则

新增 `docs/protocol/public-protocol-policy.md`：

- 说明字段号冻结规则。
- 说明服务器与 Unity 的导入/生成顺序。
- 说明不允许手改生成代码。
- 说明 schema hash、协议版本和服务器 build id 的关系。
- 给出向前兼容、最低支持版本和强制升级策略。
- 给出错误码新增规则。

新增兼容性测试：使用上一份已发布 descriptor 与当前 descriptor 比较，发现字段删改、类型不兼容或 oneof 字段号复用时失败。

## S0.3 数据库 migration

为 `player_profile` 以及本阶段后续新增表建立明确 migration：

```text
config/migrations/
  0001_player_profile_up.sql
  0001_player_profile_down.sql
  ...
scripts/migrate_db.sh
scripts/rollback_db.sh
```

要求：

- migration 可重复检测，但不能悄悄吞掉失败。
- 生产/Formal 模式由 migration 建表；运行时 `EnsureTable()` 只能做结构存在性检查，不能在业务流量中隐式 DDL。
- 本地开发可保留显式开关执行 bootstrap DDL，但默认关闭。
- migration 记录版本和 checksum。
- 添加“空库升级、重复执行、回滚到上一版本、缺字段启动失败”测试。

## S0.4 公共地图容量语义

既定规则是：公共地图实例按稳定顺序选择，当前实例到 50 人后，第 51 人才创建或进入下一实例。

修正测试隔离和实现：

- 每次容量测试使用唯一 Redis key prefix 或先清理本测试自己的 key，不能受旧进程/旧测试数据污染。
- 并发 51 人测试必须得到容量分布 `[50, 1]`，而不只是 `max <= 50`。
- 并发 101 人必须得到 `[50, 50, 1]`。
- 重复 EnterMap 不重复占位。
- 断线宽限期内占位策略要明确；最终 Logout/超时必须释放。
- 指定已满实例返回 `ERR_MAP_FULL`，不能自动偷换到其他实例。
- 公共池 `SMEMBERS` 的无序性不能决定业务顺序；增加显式创建序号或有序集合。

## S0 验收

至少运行：

```bash
./scripts/check_deps.sh --full
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/check_public_protocol.sh
./scripts/test_map_capacity.sh
```

S0 未通过时停止，不进入 S1。

---

# S1：协议握手、心跳和统一错误语义

## S1.1 ClientHello / ServerHello

在 `game.proto` 增加握手消息，字段只追加：

```proto
message ClientHelloReq {
  uint32 protocol_version = 1;
  string schema_sha256 = 2;
  string client_version = 3;
  string platform = 4;
  string build_channel = 5;
  repeated string capabilities = 6;
}

message ServerHelloRsp {
  bool ok = 1;
  string error_code = 2;
  string message = 3;
  uint32 protocol_version = 4;
  uint32 min_supported_protocol_version = 5;
  string schema_sha256 = 6;
  string server_build = 7;
  int64 server_time_ms = 8;
  uint32 heartbeat_interval_ms = 9;
  uint32 idle_timeout_ms = 10;
  repeated string capabilities = 11;
}
```

要求：

- 新连接在 Register/Login/Reconnect 前必须先 Hello。
- 为兼容旧 C++ E2E 客户端，提供有截止版本的兼容开关；Formal 默认强制 Hello。
- 协议版本不兼容返回 `ERR_PROTOCOL_VERSION`。
- schema hash 不匹配返回 `ERR_SCHEMA_MISMATCH`，并携带服务器 hash，不允许进入登录。
- 客户端版本过低返回 `ERR_CLIENT_UPGRADE_REQUIRED`。
- Gateway 只保存连接级协商状态，不把 Hello 转发给 GameLogic。
- 记录 hello 成功/失败指标，日志不能包含 Token/密码。

## S1.2 心跳与时间同步

增加 `HeartbeatReq/HeartbeatRsp`：

- 客户端单调时间、最近收到的 server_seq。
- 服务器时间、连接 RTT 计算所需回显值。
- 心跳不是 Session 续命授权，不得绕过 fence。
- 未登录连接和已登录连接都可心跳，但需各自限频。
- Gateway 根据 `idle_timeout_ms` 清理失活 TCP；清理后走现有 `MarkDisconnected`，不能直接 Logout。
- 心跳处理留在 Gateway 轻量路径，不能同步访问 Redis/MySQL。
- 增加随机抖动建议，避免客户端同秒心跳风暴。

## S1.3 统一错误响应

在 `GameResponse` 顶层追加：

```text
error_code
retryable
server_time_ms
trace_id
```

不要改现有字段号。建立公共错误码表：

```text
OK
ERR_INVALID_ARGUMENT
ERR_UNAUTHENTICATED
ERR_FENCE_STALE
ERR_SESSION_EXPIRED
ERR_PROTOCOL_VERSION
ERR_SCHEMA_MISMATCH
ERR_RATE_LIMITED
ERR_OVERLOADED
ERR_DEPENDENCY_UNAVAILABLE
ERR_MAP_FULL
ERR_MAP_DATA_MISMATCH
ERR_NOT_ON_MAP
ERR_STALE_SEQ
ERR_MOVE_TOO_FAST
ERR_AOI_RESYNC_REQUIRED
ERR_MAIL_*
ERR_INTERNAL
```

要求：

- Gateway/Session/GameLogic/GlobalService/GameDB 做错误转换，不直接把底层 MySQL/Redis/brpc 文本暴露给客户端。
- `message` 只作为可诊断文本，不作为客户端业务判断依据。
- 明确哪些错误可重试以及重试前置条件。
- 增加每 error_code 的指标。

## S1.4 基础限流和连接保护

补齐或验证：

- 每 IP 新建连接速率。
- 每连接每秒帧数、字节数和心跳频率。
- 未登录接口白名单。
- Register/Login/玩家邮件各自限流。
- 超大帧、非法 protobuf、持续半包、发送队列过载 fail-closed。
- 慢客户端断开前记录原因和指标。

## S1 验收

新增真实 TCP 测试：

1. 正确 Hello 后才能 Register/Login。
2. 错协议版本、错 schema hash 被拒绝且连接不可继续登录。
3. 心跳能得到时间和 RTT 回显。
4. 心跳停止超过 idle timeout 后 TCP 被清理，Session 进入 DISCONNECTED 而不是 OFFLINE。
5. 心跳洪泛被限流。
6. 顶层错误码稳定，客户端无需解析 message。

S1 未通过时停止，不进入 S2。

---

# S2：断线恢复、世界快照和最后安全位置

## S2.1 扩展全量世界快照

不要另造一套互相冲突的快照。扩展现有 `FullStateSnapshotRsp`，至少包含：

```text
player profile
inventory / asset_version
realm_id
map_template_id
map_instance_id
gamelogic_instance_id（仅诊断，客户端不得选路）
owner_epoch
route_version
self EntitySnapshot
AOI EntitySnapshot 列表
baseline_server_seq
snapshot_version
```

新增客户端可请求的 `WorldSnapshotReq`，或将等价接口清晰命名。要求：

- Gateway 用连接绑定覆盖 player_id。
- GameLogic 在玩家串行队列中生成一致视图。
- 快照大小受限，AOI 只包含当前视野；超过上限返回明确错误。
- snapshot 的 `baseline_server_seq` 与可靠 Push 顺序一致，客户端应用快照后从下一序号继续。
- 生成失败不能发送 `ok=true` 的空快照。

## S2.2 AOI 序号缺口恢复

当前客户端发现 server_seq gap 时需要权威恢复路径：

```text
客户端发现 gap
→ 暂停应用依赖连续性的增量
→ WorldSnapshot/AoiSnapshot
→ 原子替换本地 AOI
→ 设置 baseline_server_seq
→ 继续应用更高序号 Push
```

服务器必须：

- 支持显式 snapshot 请求。
- 对过旧 ACK/无法回放返回 `ERR_AOI_RESYNC_REQUIRED` 或直接提供完整快照。
- 不允许一直返回 `need_full_snapshot=true` 却没有获取快照的方式。
- 可靠 Enter/Leave 与可合并 Move 的顺序语义写入文档和测试。

## S2.3 精确重连和最后安全位置

新增 GameDB 所有的数据模型，例如：

```text
player_id
realm_id
map_template_id
last_safe_x/y/z/yaw
position_version
updated_at
```

规则：

- 正式模式只有 GameDB 写 MySQL，GameLogic 不直接写库。
- Move 不逐包写库；按时间/距离节流，并在 LeaveMap、最终 Logout、优雅停服时强制 flush。
- 只保存服务器已经验证为合法的坐标。
- 宽限期内 Reconnect 优先回原 `map_instance_id` 并返回当前世界快照。
- 原实例仍存在时不能无理由申请新公共实例。
- 原实例失效时按 `map_template_id + last_safe_position` 恢复；坐标不可用则回安全出生点，并返回恢复原因。
- 普通客户端不能指定任意实例、epoch 或坐标绕过校验。
- GameLogic 崩溃后的能力边界要诚实：若不能恢复实时地图状态，返回“重进地图并从持久化位置恢复”，不要宣称无损迁移。

## S2.4 最小角色生命状态

为后续玩法提供最小生命周期，不实现完整战斗：

- HP/MP 保持服务器权威。
- 定义 `ALIVE/DEAD/RESPAWNING`。
- HP=0 时禁止移动和普通游戏写命令。
- 提供 `RespawnReq/Rsp`，在地图安全出生点恢复基础 HP/MP。
- 死亡/复活状态包含在 PlayerAttributes 或世界快照中。
- 防止重复 Respawn 和旧 fence 操作。

如果本阶段工期必须最小化，可把 S2.4 标记为下一里程碑，但不能伪装为完成。

## S2 验收

真实 TCP、多进程测试至少覆盖：

1. gw0 断线后通过 gw1 重连到原会话和原公共地图。
2. 重连后收到 self + AOI 完整快照，两个玩家仍能互相看到。
3. 人为制造 Push gap 后客户端可请求快照恢复。
4. 快照 baseline 后的增量不丢、不重复倒退。
5. 最后安全位置在 GameLogic 重启后可恢复。
6. 非法位置不落库。
7. Logout/宽限期超时/优雅停服会 flush。
8. 旧 fence、旧 epoch、旧 client_seq 均被拒绝。

S2 未通过时停止，不进入 S3。

---

# S3：基础社交接口、真实联调门禁和发布收口

## S3.1 玩家公开资料和在线状态

增加只返回非敏感字段的查询接口：

```text
GetPlayerBrief(player_id 或精确名字)
QueryOnlineState(player_id)
```

返回：

- player_id、player_name、基础展示信息。
- online/offline/disconnected；不要返回 Gateway/Logic 内网地址、session_id、token。
- 精确名字查询需规范化、长度限制、限流和防批量枚举。

这样 Unity 邮件界面不必永远手工猜 ID，也为好友/组队打基础。

## S3.2 最小世界聊天

当前 `ChatSend` 是桩。实现最小“世界频道”闭环：

- Gateway 使用连接身份覆盖 player_id。
- GlobalService 负责限流、敏感长度校验、生成消息 ID 和时间戳。
- 广播到在线玩家所在 Gateway，按 Gateway 批量 Push，不广播空 Gateway。
- 客户端去重；服务器不保证聊天成为资产事实源。
- 每玩家/每 IP 限流，最大 UTF-8 字节和字符数明确。
- 暂不实现私聊、好友、公会；在文档中标记范围。

## S3.3 服务器端客户端就绪门禁

新增：

```bash
./scripts/client_ready_gate.sh
```

门禁必须使用公网 Gateway TCP，依次验证：

1. Hello + Heartbeat。
2. Register + Login + Profile。
3. 地图 hash 正确时 EnterMap，错误时拒绝。
4. 两玩家同图 AOI Enter/Move/Leave。
5. 51 人严格 50+1。
6. 玩家邮件发送、接收、MailboxChanged 或可验证兜底。
7. gw0 → gw1 重连。
8. Push gap → WorldSnapshot → 恢复。
9. 最后安全位置恢复。
10. Logout 后 Session 最终释放。

所有断言必须由协议响应、管理指标或结构化结果验证，不能只 grep 一条“PASS”日志。

## S3.4 修正 stable gate

- `stable_gate.sh --full` 中 required 的场景不能用 `skipped:true, exit_code:0` 计为稳定通过。
- 明确区分：
  - `DEV PASS`
  - `CLIENT READY PASS`
  - `STABLE CANDIDATE PASS`
- 完整稳定门禁至少包含 Debug/Release、unit/integration、client-ready、sanitizers、20× E2E、负载和 soak。
- 不能复用其他 commit 的报告。
- 工作区 dirty 时不能产生正式 stable verdict。
- 更新过时的 `docs/release/unity-integration-s3.md`，写入当前 commit、真实命令和真实退出码。

## S3.5 CI

CI 至少增加：

- Public proto compatibility + manifest/hash。
- Debug/Release build。
- Unit + integration。
- SQL migration 测试。
- Sanitizer（可按矩阵）。
- 可启动依赖容器时跑 client-ready C++ TCP gate。
- 生成协议导出物供 Unity pipeline 下载，artifact 绑定 commit SHA。

## S3 验收

```bash
./scripts/client_ready_gate.sh
./scripts/stable_gate.sh --with-e2e
./scripts/stable_gate.sh --full
```

如果 30 分钟负载或 2 小时 soak 未执行，最终报告必须写 `NOT RUN / STABLE BLOCKED`，不能写稳定通过。

---

# 4. 建议保留到后续版本的内容

以下不是本轮客户端基础联调的阻塞项，不要为了“完善”而膨胀范围：

- 无缝全域大世界 Cell 跨服迁移。
- 完整战斗、技能、掉落、任务、交易、公会。
- 跨地图实时快照无损迁移。
- Redis Cluster/MySQL 集群自动部署。
- Kubernetes、Service Mesh 或更换 RPC 框架。

先把单 MapInstance 单 Owner、公共地图 50 人、断线恢复和客户端闭环做正确。

# 5. Cursor 每阶段最终输出格式

每阶段结束只输出事实：

1. 当前 HEAD 和工作区状态。
2. 修改文件列表及用途。
3. 新增/修改的 proto 字段号。
4. 数据 migration 及回滚方式。
5. 实际运行命令、退出码、报告路径。
6. 未运行的测试及原因。
7. 本阶段是否通过门禁。
8. 仍存在的限制。

禁止：

- 用“理论上可用”代替测试。
- 用 C++ 假客户端证明 Unity 已联调成功。
- 为通过测试删除断言、返回固定成功或吞掉错误。
- 把历史日志当成本次提交的验证结果。
- 未经授权提交或推送。
