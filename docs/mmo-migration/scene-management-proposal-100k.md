# GameMesh 场景管理方案（主图分线 + 动态副本 / 10 万同时在线）

> **文档性质：** 10 万 CCU **评估稿**（数字推演 / 评审题）。**施工权威已换成** `scene-management-construction.md`（P0–P2，保守定额）。
> **版本：** v1.0-draft（评估）；施工请用 construction v1.1
> **日期：** 2026-09-06
> **仓库：** GameMesh（CppWebServer），分支以评审时 `main` 为准。
> **合成来源：**
> 1. 仓库正式基线 `docs/mmo-distributed-architecture.md`（控制面 / 单一 Owner / 同构 Logic 池）
> 2. 历史任务书 `docs/archive/mmo_distributed_architecture_cpp17_gamelogic.md`（MapScheduler、epoch、Transfer、可选 Partition）
> 3. 针对「20 张主图 + 50 种动态副本 + 10 万 CCU」的产品向场景规格
> **现状实现：** 公共地图池 `public_map_capacity=50`、Redis Lua 占位、`MapRuntime` AOI、进图 Transfer。主城分线与副本生命周期 **尚未实现**。

---

## 0. 给评审者的任务（请按此评估）

请作为资深 MMO 分布式服务端架构师评审本文。**不要改写成本仓库禁止的技术栈**（见 §2）。请输出：

1. **总评：** 能否支撑「类天龙八部」20 张常驻主图 + 50 种动态副本 + 10 万同时在线。
2. **必须改的缺陷：** 正确性、一致性、单点、脑裂、容量算错。
3. **建议改的缺陷：** 体验、运维、成本，但不破坏 §2 约束。
4. **数字是否站得住：** Gateway / GameLogic / Session / Redis / 单线人数 / AOI fanout。
5. **协议与 Redis 模型** 是否够原子、是否有竞态。
6. **与现有 50 人公共池** 如何兼容迁移，有无更小的第一步。
7. **明确反对或同意** 的非目标（§12）。
8. 若你有替代方案，必须说明：比本文好在哪、如何映射到现有 Gateway / Session / GameLogic，以及违反了哪些约束。

评审时假设：读者可以读仓库，但评估应能只靠本文自洽。

---

## 1. 产品目标与非目标

### 1.1 产品目标

做成类《天龙八部》的分线 + 副本模型，而不是一张无缝大世界：

| 类型 | 数量级 | 例子 | 玩家关系 |
|------|--------|------|----------|
| 常驻主图（分线） | 约 20 个模板 | 苏州、洛阳、野外地图 | 同模板可多「线」；同线可见，跨线不可见 |
| 动态副本地图 | 约 50 个模板 | 5 人 / 10 人 / 20 人本 | 按队伍创建实例；非队员不可进；空则销毁 |
| 同时在线 | 10 万 CCU | 含在图 + 仅登录未进图 | 在图玩家是场景负载的主体 |

体验要求：

- 好友 / 队伍能 **指定线** 或 **指定副本实例** 汇合。
- 热门主城人满时：**排队或提示换线**，禁止把人偷偷塞进另一条线（指定线时）。
- 系统自动选线时：优先未满软顶、人数较少的线；必要时原子开新线。
- 断线宽限期内回同一 `map_instance_id`（主图）；副本可配置更短保留。
- 主动 Logout 释放占位并对他人 AOI Leave。

### 1.2 非目标（本期不做）

- 无缝大世界跨机器连续 AOI（`MapPartition` 仅作为远期开口，见 §11）。
- 跨线「隐约看见另一条线的人」。
- 按地图编译 / 永久绑定 `gamelogic_苏州`。
- 引入 gRPC、重写 Reactor、让 GameLogic 接受密码。
- 把 Tick / AOI 放到 Gateway 或 World（GlobalService）。
- 第一天 etcd / NATS / 完整 Actor Runtime / K8s。
- 用现在的「50 人公共池自动开互不可见房间」冒充苏州。

---

## 2. 硬约束（评审不得推翻，除非指出仓库必须改宪）

来自 `AGENTS.md` 与正式基线，实施与评审必须遵守：

1. **外网：** Client ↔ 公网 VIP:8081（L4）→ Gateway 集群。客户端只连 VIP，不连 Logic / Session / etcd。
2. **登录链：** Gateway 编排 `AuthService.Login` → `SessionService.AcquireSession` → `GameLogic.BindPlayer`。Auth **不**创建 Session。
3. **Auth / Session** 可同二进制 `session`，proto 分离。
4. **内网 RPC：** 继续 brpc + 现有 proto。**禁止再引入 gRPC。**
5. **不重写** `EventLoop` / `TcpServer` Reactor。
6. **GameLogic 不接受密码 / 凭证。**
7. **`MarkDisconnected` ≠ `Logout`。** 断线保留 Session 与主图占位至宽限期；Logout 才释放。
8. **Push：** GameLogic `PushBatch` 按 `gateway_instance_id` 发给 **一台** Gateway，禁止广播所有 Gateway。
9. **World 进程** = GlobalService：邮件 / 跨图全局业务，**禁止**战斗 Tick / AOI。
10. **资产事实源：** MySQL / GameDB。Redis 只做 Session / Placement / Presence / 限流。
11. **一个 `map_instance_id` 同一时刻只有一个可写 Owner**（`owner_logic_server_id + owner_epoch`）。
12. **GameLogic 同构池：** 任意实例可承载任意已发布模板的多个运行时实例。
13. 服务发现：etcd 可选；静态 `*_addrs` + `IServiceRegistry` 降级必须可用。

---

## 3. 仓库现状（评审对照用）

### 3.1 已落地、应复用

| 能力 | 现状 | 文件 / 键 |
|------|------|-----------|
| 多 Gateway / 多 GameLogic / Session / World / GameDB | 正式集群脚本 | `scripts/start_formal.sh` |
| Session fence / ONLINE / DISCONNECTED | Redis Hash + Lua | `gamemesh:dev:session:{playerId}` |
| 公共池占位 | Lua 原子选房或新建 | `map:inst:` / `map:occ:` / `map:pres:` |
| 指定 `map_instance_id` 满员 | `ERR_MAP_FULL`，不换图 | `PlacementStore` |
| 进图跨 Logic | Gateway Transfer Saga | `GatewayEnterMapOrchestrator` |
| 地图静态数据 | Manifest + SHA-256 | `config/maps/`，模板 1001 / 1002 |
| AOI | 格子广播 ENTER/MOVE/LEAVE | `MapRuntime`，`aoi_view_radius_cells=2` |
| 断线隐藏、宽限期重连 | Disconnect ≠ LeaveAll | `docs/aoi-v1.md` |
| 在线人数集合对账 | `online:players` 对账 ONLINE | `SessionStore::OnlinePlayerCount` |

### 3.2 已暴露、必须在方案中修掉的问题

1. **Owner 倾斜：** `ResolveOrCreateMap` 每次先 `RefreshHealthyLogicOwners`，`SetLogicOwners` 把进程内 `rr_=0`，`PickOwner` 几乎总返回 `owners[0]`（通常 `gl-0`）。进图后玩家被 Transfer 到地图 Owner，**登录哈希均衡会被场景吸走**。
2. **公共池语义过粗：** `EnterMap.map_instance_id=0` 表示「满 50 就新开一个互不可见的同模板房」。这适合压力房，**不适合主城分线**（线号不稳定、好友难汇合、空房残留）。
3. **单出生点叠人：** 模板默认一个 spawn，AOI 半径 2 格时同线所有人互相可见，扇出按「同线人数」而不是「同城人口密度」。
4. **空实例回收弱：** occupancy / inst 键 TTL 长，压测后 occupancy 虚高。
5. **连接限流：** 每 Gateway 每源 IP 2 秒 80 次 connect。10 万从同一 NAT 出口登录会大量 `remote closed`。这是接入问题，不是场景模型问题，但 10 万方案必须单列。
6. **Session 进程 MVP 单点；** Placement 与 Session 同机。10 万必须把 Session 做成无状态多实例，Redis 为权威。

### 3.3 现有协议字段（扩展时优先加字段，避免语义偷换）

`EnterMapReq`（`proto/game.proto`）：

```text
player_id, realm_id, map_template_id,
map_instance_id,          // 0 = 今日：公共池；提案：见 §7.1
map_data_version, map_data_sha256,
operation_id
```

`EnterMapRsp` 已有：`map_instance_id, gamelogic_instance_id, owner_epoch, route_version, spawn, self, aoi_snapshot`。

提案将 **增加** `line_no`、`kind`、`queue_token` 等，并 **收窄** `map_instance_id=0` 在 LINE 模板上的含义（系统选线，而不是 50 人房）。

---

## 4. 核心对象模型

### 4.1 术语

| 术语 | 定义 | ID |
|------|------|-----|
| Realm | 逻辑区服 | `realm_id`（uint32） |
| MapTemplate | 不可变静态图（网格、出生点、AOI 格尺寸） | `map_template_id` |
| kind | 模板的场景类别 | `LINE` \| `DUNGEON` \| （远期 `PARTITION`） |
| MapLine | 主图的一条分线 = 一个运行实例 | `map_instance_id` + `line_no` |
| DungeonInst | 一次开本 = 一个运行实例 | `map_instance_id` + `party_id` |
| MapRuntime | GameLogic 进程内该实例的实体/AOI/Tick | 非全局 ID |
| Owner | 该实例唯一可写 GameLogic | `gamelogic_instance_id` + `owner_epoch` |
| Occupancy | 占位人数（含断线宽限期内未释放者） | `SCARD map:occ` |
| Connected visible | AOI 只推 `connected=true` 的实体 | `MapRuntime` |

**铁律：** 跨 `map_instance_id` 的玩家永远不进入彼此 AOI。同模板不同线 = 不同实例。

### 4.2 模板静态配置（建议加到 manifest / 模板 JSON，不进热路径乱读）

```text
map_template_id
kind                  = LINE | DUNGEON
scene_name
data_version, sha256
aoi_cell_size
nav_sample_step
aoi_view_radius_cells     # 可覆盖进程默认
soft_cap                  # LINE: 600/1000；DUNGEON: 编制人数
hard_cap                  # LINE: soft+20%；DUNGEON: = soft
max_lines                 # 仅 LINE，例如 32
min_lines                 # 仅 LINE，默认 1
empty_close_delay_sec     # LINE: 600；DUNGEON: 30
disconnect_hold_sec       # LINE: 用 Session grace（45）；DUNGEON: 可 60
spawn_policy              = DEFAULT | ROUND_ROBIN | RANDOM_OFFSET
spawn_offset_radius_m     # 建议主城 3～8
```

现网默认（未升级前）：`kind` 缺省视为 **LEGACY_POOL**（今日 50 人公共池），以便灰度。

### 4.3 实例动态状态

```text
map_instance_id
realm_id, map_template_id
kind, line_no                 # DUNGEON 的 line_no=0
party_id / member bitmap      # 仅 DUNGEON
owner_logic_server_id
owner_epoch
route_version
state                         # CREATING | READY | FROZEN | MIGRATING | RECOVERING | DRAINING | CLOSED
lease_until
occupancy
created_at, last_occupied_at
logic_version                 # 拒绝旧二进制 Claim
```

状态机：

```text
CREATING → READY ⇄ DRAINING → CLOSED
READY → FROZEN → MIGRATING → READY（换 Owner，epoch+1）
READY → RECOVERING → READY（Owner 宕机，epoch+1）
任何非 CLOSED 均可因空闲超时 → CLOSED（DUNGEON 更快）
CLOSED 不可加入；索引删除；Runtime 卸载
```

`FROZEN` / `MIGRATING` 期间：拒绝新 Enter；已在图玩家移动可按实现选择「拒绝并提示」或「短暂停 Tick」。第一期建议 **拒绝新 Enter + 暂停非可靠 MOVE 合并窗口**，避免双 Owner。

---

## 5. 目标拓扑与容量账

### 5.1 逻辑拓扑

```text
Client
  → L4 VIP :8081
  → Gateway × G          长连接、Hello、限流、粘性转发、按 gw id 收 Push
  → session × S          无状态；Auth + Session + Placement Lua
  → gamelogic × L        同构；每进程多 MapRuntime
  → world × 1..W         全局业务，无 Tick
  → gamedb × D           访问层，非 MySQL HA
Redis                    Session + Placement + 注册
MySQL                    账号 / 资产
```

### 5.2 10 万 CCU 经验拆分（可被压测修正，评审请挑战）

假设在线分布（可配置）：

| 池 | 占比 | 人数 |
|----|------|------|
| 5 张热门主城 | 40% | 40,000 |
| 15 张普通主图 | 40% | 40,000 |
| 动态副本中 | 20% | 20,000 |

建议 SLA（默认，按图覆盖）：

| 场景 | 软顶 | 硬顶 | 同时实例量级 |
|------|------|------|----------------|
| 热门主城分线 | 1000 | 1200 | 热城 40k / 1000 ≈ 40 线（例如苏州 15～25 线） |
| 普通主图分线 | 600 | 800 | 约 80 线 |
| 5 人副本 | 5 | 5 | 同时约 2000～3500 实例 |
| 10～20 人副本 | 10～20 | 同左 | 同时约 200～800 实例 |

主线合计约 **100～150** 条；副本同时存活 **2k～4k**。

### 5.3 进程规模（第一可运营形态）

| 角色 | 数量 | 单进程预算 | 说明 |
|------|------|------------|------|
| Gateway | 12～16 | 8k～12k TCP | 含未进图；按连接与 Push QPS 扩 |
| Session | 3～5 | 无状态 | 所有实例打同一 Redis；Lua 原子 |
| GameLogic | 40～50 | 2000～3000 **在图** 玩家 | 看 AOI fanout，不是看登录数 |
| World | 1～2 | — | 不进场景 |
| GameDB | 2+ | — | 访问层 |

单 GameLogic 混挂：例如 3～5 条主线 + 80～150 个小副本。用 **实例级串行**（每 `map_instance_id` 一邮箱或一把锁），禁止全进程一把大锁扫所有图。

### 5.4 AOI 与扇出预算（场景 CPU 的真实上限）

AOI 继续：水平面 X/Z，Chebyshev 格子，半径 `R` 格 → `(2R+1)²` 格。

| 地图 | cell | R | 单人可见 p95 目标 |
|------|------|---|-------------------|
| 主城 | 16～24 m | 1～2 | ≤ 40 |
| 野外 | 12～16 m | 2 | ≤ 30 |
| 副本 | 8～12 m | 2 | ≤ 编制人数 |

**禁止**用「全图广播」替代 AOI。  
MOVE 可合并；ENTER/LEAVE 可靠 Push。  
单 Tick / 单 PushBatch：事件数与字节上限（沿用现有有界队列；超限丢可合并 MOVE，可靠事件不得默默丢，应触发 `ERR_AOI_RESYNC_REQUIRED` / 快照）。

**出生打散：** `spawn_policy=RANDOM_OFFSET`，半径 3～8 m，且必须 walkable。否则软顶 1000 的线会退化成「1000 人同格、每人看见 999」。

粗算：若 1 个 Logic 3000 在图、主城 p95 可见 40、Tick 10Hz、MOVE 可合并到 2～5Hz，则每 Logic 出站实体更新约 `3000 × 40 × 5 ≈ 6e5` 接收者·更新/秒的上界，必须靠合并与距离裁剪压到可接受（评审请给更稳的模型或反例）。

### 5.5 接入限流（10 万登录）

现网 `GatewayConnGuard`：每 IP 每 2s 80 connect。10 万 CCU **不是** 10 万瞬时 connect。方案要求：

- 生产按 VIP/出口网段配置更高阈值，或按 `X-Real-` 不可用时改为 **每 VIP 连接速率 + 每账号 Auth 速率**。
- 压测必须多源 IP 或放宽 connect 限额；业务限流（Auth 20/10s/连接）保留。
- 登录错峰 / 排队服可后做，但要在错误码留 `ERR_OVERLOADED` / 排队票。

---

## 6. 控制面数据（Redis）

前缀沿用 `gamemesh:{env}:`（现网 `gamemesh:dev:`）。以下为提案键，实施时以 Lua 为准。

### 6.1 键设计

| 键 | 类型 | 内容 |
|----|------|------|
| `{p}map:inst:{id}` | Hash | §4.3 字段 |
| `{p}map:occ:{id}` | Set | player_id 占位（含宽限未释放） |
| `{p}map:pres:{player}` | Hash | 当前 reserved/confirmed 的 instance、kind、line、op |
| `{p}map:op:{player}:{op}` | String | 占位幂等结果，EX 600s |
| `{p}map:idgen` | String | 全局 instance id INCR |
| `{p}map:lines:{realm}:{tpl}` | ZSET | score=`line_no`, member=`instance_id` |
| `{p}map:line:{realm}:{tpl}:{line}` | String | 该线当前 instance_id（NX 开线） |
| `{p}map:pool:{realm}:{tpl}` | ZSET | **仅 LEGACY_POOL** 兼容旧 50 人房 |
| `{p}map:dungeon:party:{party}` | Hash | 进行中的 dungeon instance、成员 |
| `{p}map:dungeon:members:{inst}` | Set | 允许进入的 player_id |
| `{p}online:players` | Set | Session ONLINE 对账（已有） |
| `{p}session:{player}` | Hash | 已有 Session |

实例 Hash 增补字段：`kind, lineNo, partyId, softCap, hardCap, emptyAt, logicVersion`。

### 6.2 容量不变量（Lua 必须保证）

1. `SISMEMBER(occ, player)=1` 时再 SADD 不得使 count 超过 hard_cap（幂等命中除外）。
2. `SCARD(occ) ≤ hard_cap` 在任意成功返回后成立。
3. 同一 player 同时最多一个 `pres`（换图：先 Leave/Release 再 Enter，或单 Lua 迁占位）。
4. 相同 `player_id + operation_id` 重试返回同一 `map_instance_id` / `line_no` / Owner。
5. 指定 LINE 满硬顶 → 错误，不改 `line:{tpl}:{line}`。
6. 指定 DUNGEON instance 非成员 → 错误。
7. CLOSED / RECOVERING / FROZEN 不可 `try_join`（RECOVERING 仅允许已占位玩家重连 Confirm）。

### 6.3 Owner 选择（替换进程内 rr_）

**禁止**依赖 `PlacementStore::rr_`。刷新健康列表不得改变已有实例 Owner。

新建实例（开线或开本）在 **Session 进程** 内计算后传入 Lua `owner` 参数，或在 Lua 内用健康列表 CSV + 稳定哈希：

```text
candidates = healthy ∩ not_draining ∩ version_ok ∩ load(logic) < logic_in_map_soft
if preferred_owner ∈ candidates: owner = preferred   # 副本：队长当前 Logic
else:
    h = hash64(realm, template_id, line_no or next_idgen, salt)
    owner = candidates[h % |candidates|]
```

`load(logic)` 建议：该 Logic 上所有 READY 实例 occupancy 之和（可用 Redis 侧聚合或进程心跳上报 `svc:gamelogic:{id}`）。

第一期若没有可靠 load：用 `idgen % |candidates|` **稳定打散**，仍比 `rr_=0` 正确。第二期再加容量感知（正式基线要求）。

已有实例 **永不** 因 Discover 重跑而改 Owner。只有 Recovering/Migrate 升 epoch。

---

## 7. 公网与编排协议

### 7.1 EnterMap 语义变更（LINE / DUNGEON / LEGACY）

| 模板 kind | `map_instance_id` | 建议新增 `line_no`（字段待分配） | 行为 |
|-----------|-------------------|----------------------------------|------|
| LINE | 0 | 0 | 系统选线（最少人未满软顶；否则开新线或排队） |
| LINE | 0 | N>0 | 进第 N 线；满硬顶 → `ERR_MAP_LINE_FULL` |
| LINE | 具体 id | 忽略或校验一致 | 进该实例；满 → `ERR_MAP_FULL`（与今日指定实例相同） |
| DUNGEON | 0 | — | **禁止**；必须先 CreateDungeon 或带 instance |
| DUNGEON | 具体 id | — | 成员且未满 → 进入 |
| LEGACY_POOL | 0 | — | 保持今日 50 人池，灰度期 |

`EnterMapRsp` 回传 `line_no`、`kind`、`occupancy`、`soft_cap`（客户端 UI 用）。

### 7.2 新增命令（建议，评审可合并进现有 oneof）

**`CreateDungeonReq`**

```text
player_id, realm_id, map_template_id
party_id / member_player_ids[]
difficulty
operation_id
preferred_keep_logic   # bool，默认 true
```

**`CreateDungeonRsp`**

```text
ok, error_code
map_instance_id, owner, epoch, route_version
member_player_ids[]
```

**`QueryMapLinesReq/Rsp`**（选线 UI）

```text
template_id → repeated { line_no, instance_id, occupancy, soft_cap, hard_cap, state }
```

**`SwitchLineReq`：** 指定线 Leave+Enter 的便捷封装，仍走两次 Placement 或单 Lua 迁占位。建议第一期客户端发 Leave + Enter，服务端后做原子换线。

### 7.3 错误码（公网稳定码，扩展 `docs/protocol/public-error-codes.md`）

| error_code | 含义 | 客户端 |
|------------|------|--------|
| `ERR_MAP_FULL` | 指定 instance 满（已有） | 不偷换 |
| `ERR_MAP_LINE_FULL` | 指定线满 | 换线或排队 |
| `ERR_MAP_NO_LINE` | 线号不存在且禁止自动创建 | 刷新线列表 |
| `ERR_MAP_LINE_LIMIT` | 已达 max_lines | 排队 / 换图 |
| `ERR_MAP_UNKNOWN_TEMPLATE` | 已有实现 | 更新客户端资源 |
| `ERR_MAP_DATA_MISMATCH` | 已有 | 更新地图包 |
| `ERR_DUNGEON_NOT_FOUND` | 本不存在或已关 | 重新开本 |
| `ERR_DUNGEON_NOT_MEMBER` | 非队员 | — |
| `ERR_DUNGEON_CREATE_FORBIDDEN` | 无权限 / 已在本中 | — |
| `ERR_MAP_NOT_READY` | FROZEN/RECOVERING/CLOSED | 退避重试或快照 |
| `ERR_QUEUE_NEEDED` | 可选：发给排队票 | 持 ticket 等回调 |

### 7.4 进图编排（保持现有 Gateway 顺序）

```text
1. Client EnterMap / CreateDungeon
2. Gateway 校验 Hello + Session fence
3. Session Lua：占位 / 开线 / 开本（原子）
4. 若 Owner ≠ sticky.gamelogic_instance_id:
     BeginTransfer → Freeze 路由 → ExportSnapshot(old)
     → Bind/Prepare(new Owner) → Commit → Dispatch Enter
     → Unbind old
   否则直接 Dispatch Enter
5. GameLogic MapRuntime.Enter + ConfirmSlot
6. 回包 self + aoi_snapshot；对可见者 ENTER
失败：Release 预留或 AbortTransfer；不得残留双实体
```

幂等：`operation_id` 贯穿 3～6。Bind 失败不得留下 occ。

登录仍：`AcquireSession` 用 `player_id % |healthy|` **临时 Bind**。未进图前命令可在该 Logic 处理（背包/邮件视路由策略）。**进图后权威以地图 Owner 为准。**

---

## 8. 算法细节

### 8.1 系统选线（LINE，`line_no=0` 且 `instance_id=0`）

在 **一条 Lua** 内：

```text
lines = ZRANGE map:lines:{realm}:{tpl} 0 -1
best = nil
for each line in lines:
    inst = load_inst
    if not usable(READY and lease_ok): continue
    n = SCARD(occ)
    if n < soft_cap:
        pick 人数最少者（并列：line_no 较小）
if best: try_join(best); return
# 全部 ≥ soft_cap
if |lines| >= max_lines:
    if 存在 n < hard_cap: 可加入最空硬顶内 或 返回 ERR_QUEUE_NEEDED
    else return ERR_MAP_LINE_LIMIT
# 开新线
line_no = max(existing)+1 或最小空洞
id = INCR idgen
owner = ARGV.owner 或 lua_hash
HMSET inst ... kind=LINE, line_no
SET NX line:{realm}:{tpl}:{line_no} = id
ZADD lines
SADD occ player
```

并发开线：`SET NX` 失败则加入对方刚创建的线（若未满），避免双线同号。

### 8.2 指定线

```text
id = GET line:{tpl}:{line}
if missing and auto_create_specified_line:
    走开线（仅运营开关，默认关：避免客户端乱指定 99 线刷出空线）
if missing: ERR_MAP_NO_LINE
if SCARD >= hard_cap and player not in occ: ERR_MAP_LINE_FULL
SADD occ
```

### 8.3 创建副本

```text
校验模板 kind=DUNGEON、队长在线、成员数量 ∈ [min,max]
校验队员不在其他 DUNGEON pres（可在 LINE 上，创建成功后 Enter 再迁占位）
id = INCR
SADD members 全体
HMSET inst kind=DUNGEON party
owner = preferred_leader_logic if healthy else hash
返回 id；队员随后 EnterMap(id)
```

第一期 **不做匹配房**：客户端组队后 Create。Match 服务后置。

### 8.4 换图占位

玩家已在苏州-3，进洛阳或进本：

**推荐第一期：两阶段**（实现简单，与现网 Leave/Enter 接近）

1. `LeaveMap`：Runtime LeaveAll + `ReleaseByPlayer` + AOI LEAVE  
2. `EnterMap` 新目标  

窗口：两阶段之间玩家 `NOT_ON_MAP`。客户端必须处理。

**第二期：单 Lua `MigrateOccupancy`**  
同一脚本 SREM 旧 occ、SADD 新 occ，失败全回滚，再 Dispatch Leave+Enter。减少「占了两张图」或「两张都没占」。

### 8.5 空闲关闭

Session 或 Logic 租约心跳线程（已有 `MapLeaseKeeper` 方向）：

- LINE：`occupancy==0` 且 `now - last_occupied > empty_close_delay` 且 `line_count > min_lines` → CLOSED，删 `line:` 索引。
- DUNGEON：`occupancy==0` 超过 `empty_close_delay`（默认 30s）或结算完成 → CLOSED，删 members。
- CLOSED 后 Logic 卸载 `MapRuntime`。

### 8.6 Owner 故障

沿用基线，写进场景语义：

1. Logic 租约过期 → 实例 `RECOVERING`。
2. 健康 Logic `Claim(id, expect_epoch, new_epoch=old+1)`。
3. 从 GameDB / 内存快照恢复必要状态（主图：玩家位置走 last_safe / 重连快照；副本：未结算可失败关闭，产品可配「可恢复」）。
4. 玩家 Session 仍指向同一 `map_instance_id`；Gateway 发现 Owner 变化后 Transfer 或 Rebind。
5. 旧 epoch 写一律拒绝。

**主线线号不变。** 玩家还在「苏州-3」，只换 `gl-xx`。

### 8.7 排空下线某 Logic

1. 标记 DRAINING：不再被选为新 Owner。
2. 将其上 LINE 逐个 FROZEN → 快照 → 新 Owner Claim → 批量 Transfer 在图玩家。
3. DUNGEON 优先自然结束；超时则失败结算并踢回主图 last_safe。
4. deadline 后输出未迁完清单，禁止直接丢状态。

第一期可以 **不实现热迁主线**，只做宕机 Recovering；排空用「维护公告 + 踢回选线」。评审若认为 10 万运维不可接受，请给出最小热迁范围。

---

## 9. GameLogic 内场景运行时

### 9.1 第一期（现在的 MapRuntime 演进，不上完整 Actor）

- 每 `map_instance_id` 一份 `InstanceState`：实体表、AOI 格、connected 标志。
- 每实例串行处理 Enter / Move / Leave / Disconnect / Reconnect。
- 锁内 **禁止** brpc / Redis / MySQL；算完收件人再 `EmitAoi`。
- 模板只读共享，不复制 walkable。
- Tick：`map_tick_hz` 可按模板覆盖（主城 10，野外 10，副本 10～15）。
- Disconnect：AOI 暂离，occ 不释放（主图）。
- Logout / 宽限期到：LeaveAll + Release。

完整 `MapActor` / `PlayerActor` 邮箱是基线中后期，**不改变** Line/Dungeon 语义。升 Actor 时按 instance / player 拆邮箱即可。

### 9.2 可见性

- 仅同 instance、connected、落在 `(2R+1)²` 格内。
- 进图回 `aoi_snapshot`（不含自己）。
- 快照人数上限沿用 `GAMEMESH_SNAPSHOT_AOI_MAX`（默认 128）；超限 `ERR_SNAPSHOT_TOO_LARGE`——因此主城 **必须** 把 p95 可见压到远小于 128。

### 9.3 NPC / 怪物（提案开口，第一期可只玩家）

- 与玩家同一 `MapRuntime` 实体表，不同 `entity_kind`。
- 不进 `map:occ`（occ 只计玩家容量）。
- 副本 AI 只在 Owner 上跑。

---

## 10. 可观测性与验收

### 10.1 指标（扩展 `gamemesh_*`）

- `map_instances{kind,template,logic}`  
- `map_occupancy{instance,template,line}`  
- `map_occupancy_p99{template}`  
- `logic_in_map_players{logic}`  
- `aoi_fanout_p95{template}`  
- `enter_select_line` / `enter_create_line` / `enter_line_full`  
- `dungeon_create` / `dungeon_destroy` / `dungeon_active`  
- `placement_owner_gl0_ratio`（报警：长期 >70% 且有 ≥2 Logic）  
- `transfer_total` / `transfer_fail`  
- `map_recovering` / `claim_total`  
- Gateway `tcp_connect` / `conn_rate_limited`

### 10.2 正确性验收（必须自动化）

1. 指定苏州-3 满员第三人 → `ERR_MAP_LINE_FULL`，第三人 instance 不变。
2. 系统选线：软顶 2、三人进同一模板 → 产生 2 条线，第三人不进已满软顶线（或进新线）。
3. 并发 100 人开线：无重复 `line_no`，无超 hard_cap。
4. 双 Logic：新线 Owner 不全是 gl-0（统计 100 条新线，单 Owner 占比 < 70%，在修复 rr_ 后）。
5. 进图 Transfer：登录在 gl-1，线 Owner 在 gl-0 → 最终 Bind 为 gl-0，无双实体。
6. 副本非队员 Enter → `ERR_DUNGEON_NOT_MEMBER`。
7. 副本 occ=0 超时销毁；再 Enter → `ERR_DUNGEON_NOT_FOUND`。
8. kill Owner Logic：线号不变，epoch+1，重连回同一 instance。
9. Logout：occ-1，对方 AOI Leave，Session 删除。
10. MarkDisconnected：occ 不变，对方 Leave，宽限内 Reconnect 再 ENTER。
11. 地图 hash 错 → `ERR_MAP_DATA_MISMATCH`。
12. LEGACY_POOL 模板行为与今日 50 人池兼容。

### 10.3 规模验收（可分期）

- 2 Logic × 2 线 × 200 人：Owner 打散、AOI p95、无 Transfer 风暴。
- 再升到 4 Logic / 单线 800 人。
- 副本 500 实例同时存活，创建/销毁无泄漏（Redis 键、Runtime map）。

---

## 11. 远期开口（写清楚以免评审误判为遗漏）

### 11.1 MapPartition（无缝跨机）

仅当 **单线必须超过单机 AOI/CPU** 且产品要求同一连续空间时：

- 把一张逻辑大图拆成 `partition_id`，每个 Partition 仍是一个 `map_instance_id` + 单 Owner。
- 邻接 Partition **显式** 交换边界实体，不假设邻接实例自动可见。
- **本期不做。** 分线模型应先把单线压在 800～1200。

### 11.2 Session 分片

基线建议 Rendezvous Hash，不要裸 `player_id % session_count` 在扩缩容时砸会话。  
第一期：3～5 个 Session **无状态** 共用 Redis，无需分片。Redis 成为规格瓶颈后再分 Placement key。

### 11.3 Match / 队列

热线排队、副本匹配作为独立模块，经 Session 发 ticket，再 Enter。不要把匹配状态写入 MapRuntime。

---

## 12. 明确反对的做法

1. 用 50 人 `LEGACY_POOL` 当苏州（线号漂移、好友无法指定）。  
2. 登录 `player_id % N` 当场景均衡。  
3. 进程内 `rr_` + 每次 Discover 清零。  
4. AOI / Tick 放进 World 或 Gateway。  
5. 两个 Logic 写同一 `map_instance_id`。  
6. 按地图编译进程。  
7. Redis 当地图实体事实源（坐标权威在 Logic 内存；持久化走 last_safe / GameDB）。  
8. 指定线满员却静默换线。  
9. 副本公共池随便进。  
10. 为 10 万第一天引入 gRPC / 重写 Reactor / 无缝 Partition。

---

## 13. 分阶段实施（仍须人工启动，本文不授权立刻全做）

| 阶段 | 内容 | 验收 | 停做 |
|------|------|------|------|
| **P0** | 修 Owner：稳定哈希或 idgen%N；刷新列表不重置已有 Owner；指标 `owner` 分布 | 新开 50 实例，两 Logic 均有房 | 不改客户端协议语义 |
| **P1** | 模板 `kind=LINE`；`line_no`；按模板 soft/hard；选最少人线；指定线满员错误 | §10.2.1–4 | 不删 LEGACY_POOL |
| **P2** | `CreateDungeon` + members；空实例销毁 | §10.2.6–7 | 无匹配器 |
| **P3** | Session 多开；Logic/GW 水平扩；load 感知放置；AOI 分图配置；出生打散 | 单线 800 压测 | 无热迁 |
| **P4** | 换线封装、排队票、队伍聚合进线；可选主线热迁 | 运维演练 | Partition 仍可选 |

**建议的最小第一步（若评审同意）：只做 P0。** 不改玩法也能避免 5000 进图全堆 gl-0。

兼容：manifest 无 `kind` 的模板 = `LEGACY_POOL`，`EnterMap(0)` 保持 50 人池。1001/1002 可先留 LEGACY，新主城模板再标 LINE。

---

## 14. 协议 / 配置改动清单（供评审估工作量）

**proto（建议）**

- `EnterMapReq.line_no`  
- `EnterMapRsp.{kind,line_no,occupancy,soft_cap}`  
- `CreateDungeonReq/Rsp`  
- `QueryMapLinesReq/Rsp`  
- 错误码字符串冻结进 `public-error-codes.md`

**配置**

- `map_manifest.json` 或每模板 JSON：`kind, soft_cap, hard_cap, max_lines, aoi_*`  
- `gamelogic.cnf`：`public_map_capacity` 降级为 LEGACY 默认，不再当主城容量

**C++（方向，非本文件任务）**

- `PlacementStore` 拆 `ReserveLine` / `ReserveDungeon` / `ReserveLegacyPool`  
- 删除或停用 `SetLogicOwners` 内 `rr_=0`  
- `MapRuntime` 出生偏移  
- `MapLeaseKeeper` 空闲关闭  
- 测试：`session_store` 级 Lua 单测 + 双 Logic 进图 E2E

**客户端**

- Hello 展示多模板（已有 maps[]）  
- 选线 UI + 指定 `line_no`  
- 组队开本  
- 处理 `ERR_MAP_LINE_FULL` / 换线  
- 退出发 Logout（已在做）

---

## 15. 风险与开放问题（请评审逐条表态）

1. 单线 1000、可见 p95=40、10Hz，单 Logic 3k 在图是否过于乐观？更稳妥的单进程在图上限是多少？  
2. 第一期不做主线热迁，运营能否接受「停机迁 Logic」？  
3. 换图两阶段 Leave+Enter 的空窗，是否必须第一期就做原子迁占位？  
4. 副本失败关闭 vs 可恢复，哪类模板需要持久化战斗状态？  
5. Session 3～5 无状态打单 Redis，10 万进图 QPS 下 Lua `SMEMBERS` 线列表是否该改成「只扫未满线索引」？  
6. `CreateDungeon` 放在 Session 还是 World/Match？本文放 Session 以复用 Placement 事务。  
7. 是否允许「自动跟队长线」作为服务端逻辑，而不是客户端查线再 Enter？  
8. 连接限流从「每 IP 80/2s」改成什么，才不误伤小区 NAT，同时挡住洪泛？  
9. LEGACY_POOL 与 LINE 长期并存，是否造成运营两套房间？停用条件是什么？  
10. 若产品坚持「同城所有人互相可见」，本文应拒绝还是强制上 Partition？本文立场是 **拒绝，用分线**。

---

## 16. 一句话立场（供评审打分）

> **控制面遵守 GameMesh 基线（同构 Logic、单 Owner/epoch、Session 占位、断线≠登出、World 不跑图）；数据面把场景分成常驻分线与短寿命副本，用线号和队员名单替代 50 人公共池；10 万靠「几十个 GameLogic × 每线数百到一千人 × 裁剪后的 AOI」，而不是一张无缝大地图。**

评审请对这句话：同意 / 修正 / 反对，并给出可执行的修正段落（仍遵守 §2）。
