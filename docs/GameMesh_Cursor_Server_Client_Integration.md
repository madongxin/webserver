# GameMesh 服务器端：Unity 联调批次 Cursor 执行提示词

> 目标仓库：`https://github.com/madongxin/webserver`
>
> 分支：`main`
>
> 本提示词依据代码基线：`37b19773b6856e5da374f22f66c8540fc9867ca3`
>
> 对接客户端：Unity 2022.3.62f3c1，仓库 `https://github.com/madongxin/luna`

请把本文完整交给服务器仓库中的 Cursor。Cursor 必须直接修改工程、生成测试并执行验证，不能只输出设计方案。

## 1. 本批目标

完成一个可由 Unity Demo 实际联调的纵向切片：

1. Unity 与 Gateway 建立 TCP + protobuf 通信。
2. 注册、登录、主动下线以及断线重连可用。
3. 创建并加载服务器权威玩家资料，包括玩家 ID、名字和基础战斗属性。
4. 两个玩家进入同一公共地图实例、归属同一 GameLogic，并通过 AOI 看到彼此及移动。
5. 单个公共地图实例上限 50 人；第 51 人自动进入同模板的新实例。
6. 玩家 A 可向指定玩家 B 发送普通邮件；B 在线时收到通知并能查询、展示邮件，离线时下次登录也能查询。
7. 提供稳定、版本化的客户端协议和地图数据契约，供 Unity 仓库生成 C# 代码及导出地图数据。

本批是“小型公共地图 MMO Demo”，不是无缝大世界 Cell 分片。本轮不要实现跨 GameLogic 的同一地图 Cell、完整战斗、NPC、寻路代理、技能结算或 Unity 资源流式加载。

## 2. 开始前必须做的事

1. 完整阅读 `AGENTS.md`、CMake、`proto/`、`game/`、`runtime/`、`db/`、配置、脚本、测试和最新 release 文档。
2. 执行并记录：

   ```bash
   git status --short
   git log -1 --oneline
   ```

3. 保留用户所有未提交修改，不得回退或覆盖无关文件。
4. 先运行当前仓库的稳定门禁并保存基线结果：

   ```bash
   ./scripts/check_deps.sh
   ./scripts/build.sh Debug
   ./scripts/test.sh unit
   ./scripts/test.sh integration
   ./scripts/stable_gate.sh
   ```

5. 某条命令因环境依赖不能运行时，必须明确记录“未执行”和缺失依赖，不能写成通过。
6. 每个阶段独立完成、测试并输出结果；上一阶段不通过，不进入下一阶段。
7. 未经用户明确要求，不 commit、不 tag、不 push。

## 3. 固定架构边界

- C++17。
- Client 只连接 Gateway 公网 TCP，不连接 GameLogic、Session、World、GameDB 或 brpc。
- 外网协议沿用：`4 字节 uint32 大端 payload 长度 + GameRequest/GameResponse protobuf`，单帧最大 4 MiB，长度 0 或超限立即断开。
- Gateway 内部继续异步 brpc；不引入 gRPC，不重写 Reactor，不新增独立 Login 进程。
- Auth 与 Session 继续同进程、逻辑分离。
- 玩家可信身份、Session、fence、generation、GameLogic 和地图路由只能由 Gateway/Session 注入，不能信任客户端自报。
- 同一个运行时 `map_instance_id` 同一时刻只有一个可写 GameLogic Owner。
- 同一个公共地图实例中的玩家必须归属该地图的同一个 GameLogic，不能按玩家随机分散。
- 玩家资料和邮件以 MySQL/GameDB 为事实源；Session、Placement、地图容量预留可使用 Redis。
- `MarkDisconnected != Logout`。TCP 断开进入重连宽限期；主动 Logout 或宽限期超时才最终释放 Session 和地图名额。
- 正式模式继续禁止公网 `GrantItem` 和现有管理型 `MailDeliver`。玩家发信必须使用新的、受限的玩家邮件接口，不能复用管理投递权限暴露给客户端。
- Reactor I/O 线程、Player/Map 串行队列中不得同步等待 brpc、Redis 或 MySQL。

## 4. 当前代码事实，不要重复或错误假设

当前仓库已经有：

- Gateway Reactor TCP、`ProtoFraming`、`GameRequest/GameResponse`。
- Register/Login/Logout/Reconnect、Session fence/generation。
- Gateway → GameLogic 的 Bind/Dispatch 和 GameLogic → Gateway PushBatch。
- Redis Placement、`map_instance_id/owner_epoch/route_version/lease`。
- `EnterMap/LeaveMap/MapPing` 骨架。
- GameDB、背包和完整邮件查询/领取基础接口。
- `GetPlayerRoute`，可供内部服务查询接收者当前 Gateway 路由。

当前仓库尚未完成：

- 玩家资料/属性持久化和客户端快照；当前 GameLogic 主要加载背包。
- 地图静态数据加载、地图 Tick、AOI 和移动同步。
- 每地图 50 人的原子容量选择；当前模板索引倾向返回单个实例。
- 安全的“玩家给玩家发邮件”公网接口；现有 `MailDeliver` 是管理型能力。

必须在现有边界上增量实现，不得另建第二套登录、Session、地图 Placement、邮件库或 TCP 协议。

---

# 阶段 S1：冻结客户端协议，补齐玩家资料与生命周期

## S1.1 建立客户端协议发布物

`proto/game.proto` 是客户端外网协议的唯一事实源。保留现有字段编号，禁止重命名、改类型、复用或重新编号旧字段。

新增以下等价消息，名称可根据仓库规范微调，但语义必须一致：

```proto
message Vec3 {
  float x = 1;
  float y = 2;
  float z = 3;
}

message PlayerAttributes {
  uint64 player_id = 1;
  string player_name = 2;
  int32 hp = 3;
  int32 max_hp = 4;
  int32 mp = 5;
  int32 max_mp = 6;
  int32 attack = 7;
  int32 spell_power = 8;
  int32 defense = 9;
  int32 magic_resistance = 10;
  float crit_chance = 11;       // 0..1
  float crit_damage = 12;       // 倍率，例如 1.5
  float move_speed = 13;        // 米/秒
  float attack_speed = 14;      // 次/秒或倍率；文档固定一种含义
  uint64 stats_version = 15;
}

message EntitySnapshot {
  uint64 player_id = 1;
  string player_name = 2;
  Vec3 position = 3;
  float yaw = 4;
  int32 hp = 5;
  int32 max_hp = 6;
  uint64 state_seq = 7;
}
```

要求：

1. 在 `game.proto` 增加 `option csharp_namespace = "GameMesh.Protocol";`；该选项不得改变 C++ package。
2. `LoginRsp` 增加玩家资料快照，或者新增登录后 `GetSelfProfile` 请求。优先复用 BindPlayer 已加载结果，避免紧接登录再同步查库。
3. `EnterMapRsp` 至少增加：服务器确认的出生坐标、地图数据版本/哈希、自己状态和进入时 AOI 快照。
4. 新增 `MoveReq/MoveRsp`、`AoiDelta`、`PlayerMailSendReq/Rsp`、`MailboxChangedNotify`。
5. 新增 oneof 字段使用尚未占用的编号，建议从 60 起；一旦写入即视为稳定协议，测试锁定编号。
6. 统一 Push 外层：Gateway 下发 `GameResponse.server_push`，其 `payload` 是一个内层 `GameResponse` 的序列化结果。不要再新增另一种裸 Push 编码。
7. 固定 `message_type`：

   ```text
   aoi.delta.v1
   mailbox.changed.v1
   player.state.v1
   ```

8. 所有响应保留外层 `GameResponse.seq` 用于请求关联；Push 的外层 `seq=0`，可靠顺序使用 `ServerPushEnvelope.server_seq`。

新增：

```text
scripts/export_unity_protocol.sh
docs/client-protocol.md
```

`export_unity_protocol.sh` 必须：

- 不依赖调用者当前目录。
- 将 `game.proto`、protobuf descriptor set、协议 manifest 和 SHA-256 输出到用户指定目录。
- manifest 包含协议版本、Git SHA、帧格式、最大帧长、schema hash、生成时间。
- 不直接写死 Unity 仓库路径，不修改外部仓库。
- 缺少 protoc 时非零失败。

## S1.2 玩家资料和属性事实源

新增可迁移、可回滚的 MySQL 表，例如 `player_profile`：

```text
player_id PK/FK
player_name UNIQUE 或按当前产品规则建立索引
hp/max_hp
mp/max_mp
attack
spell_power
defense
magic_resistance
crit_chance
crit_damage
move_speed
attack_speed
stats_version
created_at/updated_at
```

默认值与 Unity 现有 FPS Demo 对齐：`max_hp=100`、`hp=100`、`max_mp=100`、`mp=100`、`move_speed=10`；其他数值定义成服务器配置，不散落魔法数字。

必须满足：

1. 注册成功时在同一业务事务中创建默认 Profile；失败不得只留下半个账号。
2. 对旧账号提供幂等补数据迁移，不能要求清空数据库。
3. 玩家名字以服务器存储为准；客户端不能在登录后自行覆盖。
4. GameDB 增加 `LoadPlayerProfile`，需要时增加受版本保护的 `SavePlayerProfile`；GameLogic 不直连 MySQL。
5. GameLogic Bind 时异步加载 Profile 和背包，任一必需数据加载失败均 fail-closed。
6. `BindPlayerResponse` 返回 Profile，Gateway 将它放入登录响应。
7. HP 归零定义为死亡；MP 消耗、攻击公式和完整战斗本批只建立数据模型，不实现玩法。
8. 对所有属性做范围校验并定义稳定错误码。

## S1.3 注册、登录、Logout、断线语义

保持现有链路：

```text
Register: Client → Gateway → Auth → GameDB
Login: Client → Gateway → Auth → Session.Acquire → GameLogic.Bind
Logout: Client → Gateway → GameLogic FinalSave/Leave → Session 条件释放 → 关闭连接
Disconnect: Gateway → MarkDisconnected → 宽限期；不是立即 Logout
Reconnect: 任意 Gateway → Prepare/Rebind/Commit → 恢复地图和 AOI
```

补齐以下要求：

- Register 的 `device_id + idempotency_key` 重试只能创建一个账号和 Profile。
- 登录失败不创建半 Session；Bind 失败执行补偿。
- 主动 Logout 从 AOI 移除实体并释放公共地图名额。
- TCP 断开可立即让其他玩家收到 AOI 暂离事件，但 Session、PlayerActor 和容量名额保留到宽限期结束；Reconnect 后恢复实体。
- 宽限期超时执行最终保存、地图 Leave、容量释放和 Session 释放。
- 旧 connection/fence/generation 的断线、Logout、Move 和 PushAck 不能影响新会话。

## S1.4 玩家给玩家发邮件

不要把现有管理型 `MailDeliverReq` 开放给客户端。增加受限接口，例如：

```proto
message PlayerMailSendReq {
  uint64 sender_player_id = 1;   // Gateway 用连接身份覆盖
  uint64 receiver_player_id = 2;
  string title = 3;
  string body = 4;
  string operation_id = 5;
}
```

规则：

- 首期不允许附件、货币和物品。
- sender ID 和 sender name 由服务器确定。
- receiver 必须由 GameDB 验证存在。
- 标题 1–64 个 Unicode 字符，正文 1–1000 个 Unicode 字符；拒绝非法 UTF-8 和控制字符。
- `operation_id` 幂等，同一次超时重试只生成一封邮件。
- 做每玩家分钟级限流，限制必须共享或至少在 World 多实例下保持正确。
- 路由到 World/GlobalService，复用现有 MailStore/邮件表，不另建第二套邮件系统。
- 收件人离线：持久化成功即发送成功，登录后 MailList 可见。
- 收件人在线：持久化成功后查询 Session `GetPlayerRoute`，尽力推送 `mailbox.changed.v1`；通知失败不回滚已经提交的邮件，客户端轮询兜底。
- `MailboxChangedNotify` 只提示版本/未读数变化，不携带敏感正文。

## S1.5 S1 测试与阶段门禁

至少覆盖：

1. 新注册玩家创建唯一账号和默认 Profile。
2. 注册超时重试不产生重复账号/Profile。
3. 老账号幂等补 Profile。
4. 登录响应包含完整服务器权威属性。
5. 错误凭证不创建 Session。
6. 主动 Logout、断线、重连、宽限期超时状态正确。
7. 玩家邮件 sender 不能伪造；管理型 MailDeliver 正式模式仍禁止。
8. 发给在线/离线指定玩家均可在 MailList 查询；幂等重试只产生一封。
9. 协议 descriptor/manifest 可重复生成，未修改 proto 时 hash 稳定。

S1 完成后运行全量可用测试并输出修改清单、协议变更、SQL 迁移和真实结果。通过后才进入 S2。

---

# 阶段 S2：地图静态数据、50 人公共实例、移动与 AOI

## S2.1 固定地图数据契约

Unity 水平面采用 X/Z，Y 为高度；服务器不得交换轴。定义 `MapStaticData V1`，由 Unity Editor 导出、GameLogic 启动时只读加载。

建议 JSON 结构：

```json
{
  "schema_version": 1,
  "map_template_id": 1001,
  "scene_name": "MainScene",
  "data_version": 1,
  "bounds_min": [0, 0, 0],
  "bounds_max": [100, 20, 100],
  "aoi_cell_size": 12.0,
  "nav_sample_step": 1.0,
  "grid_width": 100,
  "grid_height": 100,
  "walkable_rle": [1, 20, 0, 3],
  "spawn_points": [{"id":"default","position":[1,2,3],"yaw":0}]
}
```

同时生成独立 SHA-256 文件；服务器对原始 JSON 文件计算并验证哈希。规范必须明确：

- `col = floor((x - min_x) / nav_sample_step)`。
- `row = floor((z - min_z) / nav_sample_step)`。
- RLE 是 `[value,count,...]` 还是 `[count,value,...]`，只能选择一种并写测试。
- AOI 网格大小与 walkable 采样步长是两个概念，不能混用。
- 至少一个合法出生点；缺少、越界、落在不可走区域时启动/加载失败。

新增服务器目录和配置，例如：

```text
config/maps/map_manifest.json
config/maps/map_1001.json
map_data_dir=...
public_map_capacity=50
aoi_cell_size=12
aoi_view_radius_cells=2
map_tick_hz=10
```

要求：

1. GameLogic 启动时加载其支持的全部 MapTemplate 数据并校验版本/哈希。
2. MapInstance 引用共享的不可变 MapTemplate，不为每个实例复制整份 walkable grid。
3. 客户端 EnterMap 携带它所拥有的地图数据版本/哈希；不匹配返回 `ERR_MAP_DATA_MISMATCH` 和服务器期望版本，不能继续进入错误地图。
4. 服务器只加载导出的边界、出生点和 walkable grid，不解析 Unity Scene、Prefab、FBX 或 NavMesh 二进制。

## S2.2 每图 50 人的原子公共地图分配

现有单模板索引不足以表达多个公共实例。保留 Placement 权威模型，增加按 `(realm_id,map_template_id)` 管理多个实例和容量预留。

推荐 Redis 结构：模板实例集合/有序集合、每实例成员集合、玩家当前预留和幂等 operation 结果。实际键名遵守现有 prefix。

必须满足：

1. `EnterMap(map_instance_id=0)` 表示进入公共池：优先选择 `READY + lease 有效 + count < 50` 的实例。
2. 无可用实例时原子创建新 `map_instance_id`，从健康 GameLogic 中选 Owner。
3. 选择实例和为玩家占位必须在一个 Lua/CAS 事务中完成，不能先读 count 再写。
4. 相同 `player_id + operation_id` 重试得到相同结果，不重复占位。
5. GameLogic Enter 成功后确认占位；Bind/Transfer/Enter 失败释放预留。
6. 主动 Leave/Logout/宽限期超时释放名额；短暂断线宽限期内保留名额。
7. 已满实例不能因并发请求超过 50。
8. 玩家指定有效 `map_instance_id` 时，实例已满返回明确错误，不静默进入其他实例；公共池请求才自动换新实例。
9. 每个 MapInstance 仍只有一个 Owner；同实例内两个玩家必然 Dispatch 到相同 GameLogic。
10. 实例空闲后的关闭策略配置化，本批可延迟关闭，但必须清理 Redis 索引和本地 runtime。

## S2.3 MapRuntime 与 AOI

在 GameLogic 中实现最小 `MapRuntime/MapActor`：

- 每个 `map_instance_id` 一个运行时实体容器。
- 每个实例单写：通过 map mailbox/shard 或等价串行机制处理 Enter、Move、Leave、Disconnect/Reconnect。
- 空间索引使用 X/Z 均匀网格；Key 至少包含 `(map_instance_id, cell_x, cell_z)`。
- 不要把 AOI 放到 Gateway、Session 或 World。
- 锁内不能调用 brpc/Redis/MySQL；计算完收件人列表后再异步 Push。

实体状态至少包含：

```text
player_id, player_name, position(x,y,z), yaw,
hp, max_hp, state_seq, connected
```

AOI 语义：

1. 玩家进入地图：返回自己和当前视野内实体快照；附近旧玩家收到 `ENTER`。
2. 玩家移动：更新空间格，计算旧视野与新视野集合差；分别发送 `ENTER/MOVE/LEAVE`。
3. 玩家主动离开/Logout：附近玩家收到 `LEAVE`。
4. 断线：从可见实体集合暂时移除但保留 Session/容量；Reconnect 后重新 Enter AOI。
5. 不同 MapInstance 的玩家永远不可见。
6. `ENTER/LEAVE` 可走可靠 Push；高频 `MOVE` 使用 `reliable=false, coalescable=true`，按 `(recipient,entity_id)` 合并旧位置。
7. AOI Push 按玩家绑定的 `gateway_instance_id` 分组发送，不广播两个 Gateway。
8. 每 Tick 和每 PushBatch 有数量/字节上限，慢客户端执行现有有界队列策略。

## S2.4 移动协议和服务器校验

客户端发位置是输入/建议，最终位置由服务器确认。`MoveReq` 至少包含：

```text
player_id（Gateway 覆盖）
map_instance_id（Gateway 路由覆盖或严格校验）
position
yaw
client_time_ms
```

使用外层 `GameRequest.seq` 作为 `client_seq`，要求：

- 未登录、未进图、错误 fence/epoch/route、**小于 last 的旧 seq** 全部拒绝。Hello/Heartbeat/PushAck 在 Gateway 本地处理，Logic 允许 seq 空洞（EnterMap=4 后 Move=6 合法）。
- 坐标必须有限，拒绝 NaN/Inf。
- 校验地图边界和 walkable grid。
- 基于服务器上次确认位置、服务器时间、玩家 `move_speed` 和有限容差做速度校验。
- 不信任客户端传来的 move_speed、HP 或其他属性。
- `MoveRsp` 返回服务器确认的位置、yaw、state_seq 和 server_time。
- 本批允许客户端预测 + 服务器纠偏，不要求复杂 rollback netcode。

## S2.5 S2 测试与阶段门禁

单元测试：

1. Map JSON/哈希/RLE 正反例。
2. X/Z 到 walkable cell、AOI cell 的边界计算。
3. AOI 同格、跨格、视野进入/离开和跨地图隔离。
4. 非法坐标、超速、不可走区域和旧 state/client seq 被拒绝。
5. 50/51 人顺序分配正确。
6. 至少 100 个并发占位无实例超过 50，失败补偿不泄漏容量。

集成测试：

1. 两个 TCP 客户端注册、登录，进入模板 1001 后得到相同 `map_instance_id` 和相同 GameLogic Owner。
2. 两端均收到对方 AOI Enter；A 移动后 B 收到 A 的 AOI Move；A Logout 后 B 收到 Leave。
3. 第 51 个客户端进入新的实例；第一实例恰好 50 人。
4. 两个实例可在 gl-0/gl-1 任一健康节点承载，但每个实例只有一个 Owner。
5. 地图数据版本不一致客户端被明确拒绝。

S2 通过后才进入 S3。

---

# 阶段 S3：双客户端联调门禁、脚本、文档和回归

## S3.1 建立真实 TCP E2E 客户端

扩展现有 `game_tcp_e2e_client`，不要用直接 brpc 或直接调用 Service 实现替代公网路径。增加命令：

```text
register-login-profile
enter-public-map
move
send-player-mail
mail-list
two-player-aoi
map-capacity-51
unity-contract-check
```

E2E 必须真实解析：

- 外层 GameResponse。
- `ServerPushEnvelope`。
- 内层 GameResponse/AoiDelta/MailboxChangedNotify。
- 请求 seq、server_seq 和 PushAck。

## S3.2 新增脚本

```text
scripts/test_unity_contract.sh
scripts/test_two_player_aoi.sh
scripts/test_map_capacity.sh
scripts/test_player_mail_e2e.sh
scripts/run_unity_integration_server.sh
scripts/stop_unity_integration_server.sh
```

要求：

- 使用 `set -euo pipefail`。
- 不依赖调用者当前目录。
- PID 精确记录，停止时不得按进程名误杀。
- 端口、数据库、Redis、map data dir 可配置。
- 测试场景未执行、超时、缺少关键 Push 或字段时返回非零。
- 不允许 `grep PASS || true`、固定输出成功或只验证进程存在。

## S3.3 最终端到端验收

至少自动化以下流程：

1. 启动 MySQL、Redis、Gateway ×2、Session/Auth ×2、GameLogic ×2、World、GameDB ×2。
2. A、B 分别注册并登录，确认不同 player ID、完整 Profile。
3. A、B 进入公共地图 1001，确认相同实例和 Owner。
4. 双方收到彼此 AOI Enter。
5. A 移动，B 收到正确 player ID、递增 state_seq 和新坐标。
6. A 给 B 发无附件普通邮件；B 收到 MailboxChanged 或轮询发现，MailGet 正文一致。
7. A 主动 Logout；B 收到 AOI Leave，A Session 最终释放。
8. 再建立 51 个玩家，确认第 51 人进入新实例，任何实例不超过 50。
9. A 在 gw0 断线后通过 gw1 在宽限期内 Reconnect，恢复原地图容量和 AOI，不产生重复实体。
10. 重启 GameLogic 后至少从持久化重新加载玩家 Profile；不能用默认值覆盖已有属性。

## S3.4 文档与交付

更新：

```text
docs/client-protocol.md
docs/unity-integration-runbook.md
docs/map-data-v1.md
docs/aoi-v1.md
docs/release/<本批版本>.md
```

文档必须说明：

- Unity 和服务器版本组合、schema hash、地图 hash。
- 启动顺序、端口和本地配置生成方式。
- 注册、登录、Logout、Reconnect、EnterMap、Move、AOI、玩家邮件时序。
- 50 人容量语义和断线宽限期是否占位。
- 已知限制：单 MapInstance 单 GameLogic，不是跨 Cell 无缝大世界。
- 协议兼容规则及回滚方法。

最终运行并记录：

```bash
./scripts/check_deps.sh
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/test_unity_contract.sh
./scripts/test_two_player_aoi.sh
./scripts/test_map_capacity.sh
./scripts/test_player_mail_e2e.sh
./scripts/stable_gate.sh
```

## 5. 最终输出格式

Cursor 完成每个阶段后必须输出：

1. 当前 commit、工作区状态和修改文件清单。
2. 实际新增/修改的 protobuf 消息及字段编号。
3. SQL 迁移及回滚脚本。
4. 玩家资料、地图容量、AOI 和玩家邮件的代码入口。
5. 实际执行的构建/测试命令、退出码和关键结果。
6. 未执行测试及原因。
7. 协议 manifest 和地图数据的输出位置。
8. 仍存在的限制和回滚方式。

不得只说“理论上可用”。只有真实 TCP 双客户端流程和稳定门禁全部通过，才可标记本批服务器端完成。
