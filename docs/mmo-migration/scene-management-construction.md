# GameMesh 场景管理施工版（P0–P2）

> **本文是施工权威。** 开始改 Placement / EnterMap / 副本时以本文为准。  
> 10 万 CCU 评估稿（数字推演、评审题）见 `scene-management-proposal-100k.md`，**不阻塞本施工包**。  
> **版本：** v1.1-construction  
> **日期：** 2026-09-06

---

## 0. 本包做什么

做成类天龙的 **主图分线 + 动态副本**，控制面继续走 GameMesh：

- 单一 Owner / epoch，Session Redis Lua 占位，Gateway Transfer，World 不跑 Tick。
- 不引入 gRPC，不重写 Reactor，GameLogic 不接受密码，`MarkDisconnected` ≠ `Logout`。
- 1001 / 1002 继续 `LEGACY_POOL`（50 人公共池）。新主城模板再标 `LINE`。

P0–P2 已落地。**P3（换线 RPC、排队票、DRAINING 热迁、Session 多开、按图邮箱）见本文 §9 与 `player-comms.md`。** 10 万硬件清单另开。

---

## 1. 冻结决策（不要再讨论）

| 项 | 决定 |
|----|------|
| 场景种类 | `LEGACY_POOL` / `LINE` / `DUNGEON` 三态并存 |
| 指定线满员 | 返回 `ERR_MAP_LINE_FULL`，**禁止静默换线** |
| 系统选线（`line_no=0` 且 `instance_id=0`） | 未满软顶里选人数最少；必要时原子开新线 |
| 换图 / 换线 | 同模板换线用 `SwitchLine` 原子迁占位；换模板仍可 Leave+Enter |
| 开本 | `CreateDungeon` 放 **Session**（复用 Placement Lua），不放 World |
| 匹配器 / 排队票 | 排队票已做（`EnqueueMap`）；匹配器不做 |
| 主线热迁 / Partition | DRAINING + `LiveMigrateMap` 已做；Partition 不做 |
| 空线 / 空本 | LINE：空且超过 `empty_close_delay` 且线数 > `min_lines` 才关；DUNGEON：空超过 delay 即关 |
| Owner 故障 | 沿用 RECOVERING + Claim；**线号不变** |
| 登录路由 | 仍 `player_id % N` 临时 Bind；进图后以地图 Owner 为准 |

### 首期定额（比评估稿更保守）

| 项 | 施工验收上限 | 不要用评估稿的 |
|----|----------------|----------------|
| 单线 soft / hard | 200 / 400 | 1000 / 1200 |
| 单 Logic 在图 | 800～1500 | 3000 |
| 单线 AOI 可见 p95 | 远小于 128 快照上限 | 「同城全可见」 |
| 第一期 Logic 数 | 现网 2，可加到 4 | 几十台一起上 |

压测先过「2 Logic × 2 线 × 200 人、Owner 打散」。未过之前不把定额调高。

---

## 2. 禁止改动

- 不引入 gRPC；不重写 `EventLoop` / `TcpServer`。
- GameLogic 不接受密码 / 凭证。
- PushBatch 仍按 `gateway_instance_id` 打一台 Gateway。
- World 禁止战斗 Tick / AOI。
- 不删除 50 人 `LEGACY_POOL`，不把 1001/1002 改成 LINE，直到有选线 UI。
- 不把 Tick / 实体权威写进 Redis。
- 指定 `map_instance_id` 满员继续 `ERR_MAP_FULL`（语义不变）。

---

## 3. 施工顺序

```text
P0  Owner 打散（本周，无协议变更）
P1  LINE：kind / line_no / soft·hard / 指定线
P1' 出生打散 + 每模板 AOI（可与 P1 并行，勿阻塞 P1 协议）
P2  DUNGEON：CreateDungeon + members + 空销毁
—— 以上完成后再开 P3 ——
```

---

## 4. P0 — Owner 稳定放置（无 proto）

### 问题

`ResolveOrCreateMap` 每次 `RefreshHealthyLogicOwners` → `SetLogicOwners` 把 `rr_=0` → `PickOwner` 总是 `owners[0]`（gl-0）。进图 Transfer 把登录哈希均衡吸走。

### 改法（三处一起做）

1. **`SetLogicOwners`：禁止重置 `rr_`。** 列表未变则不要改内部游标。
2. **新建实例 Owner：Lua 用 `idgen % |healthy|`。** C++ 只在调用方带了且仍健康的 `preferred_owner` 时写入 ARGV；否则 ARGV 为空，由 Lua 按新 `map_instance_id` 取模。加入已有实例仍用该实例原 Owner。
3. **`RefreshHealthyLogicOwners`：健康集合未变则不要 `ApplyIds` / 不要刷快照版本。** 避免每张进图都重入放置配置。

### 改哪些文件

| 文件 | 动作 |
|------|------|
| `game/PlacementStore.cpp` | Lua `pick_new_owner`；C++ 不再用 rr 给新实例派 Owner |
| `game/PlacementStore.h` | 如需 `CreateOwnerHint` / `HasHealthyOwners` |
| `runtime/HealthyLogicOwners.cpp` | 集合未变则短路 |
| `test/placement_store_test.cpp` | 无 preferred 连开多实例，两 Owner 都要有 |

### 验收

- `force_new` + 指定 `preferred_owner` 行为不变。
- 无 preferred 连开 ≥20 个新实例（双 Owner）：两边都有，单边不超过 80%。
- 反复 `SetLogicOwners({"gl-0","gl-1"})` 后上述仍成立。
- Owner 死亡 reclaim 仍换到健康节点。
- 健康列表空：`NO_HEALTHY_GAMELOGIC`。
- 不改 `EnterMapReq` 字段语义。

---

## 5. P1 — LINE

### 模板配置（`config/maps/` + manifest）

```text
kind                # LEGACY_POOL | LINE | DUNGEON（缺省 = LEGACY_POOL）
soft_cap            # LINE 默认 200
hard_cap            # LINE 默认 400
max_lines           # 默认 8
min_lines           # 默认 1
empty_close_delay   # 秒，默认 300
aoi_view_radius_cells / aoi_cell_size   # 可覆盖；P1' 再用
spawn_scatter_radius                    # P1'；P1 可不读
```

### proto（只加字段，不偷换旧语义）

`EnterMapReq`：

| 字段 | 含义 |
|------|------|
| `map_instance_id=0` 且 `line_no=0` | LINE：系统选线；LEGACY：旧 50 人池 |
| `line_no>0` | 指定线；满员 `ERR_MAP_LINE_FULL` |
| `map_instance_id!=0` | 指定实例（旧语义）；满员 `ERR_MAP_FULL` |

`EnterMapRsp` 增加：`kind`, `line_no`, `occupancy`, `soft_cap`（可选 `hard_cap`）。

`QueryMapLinesReq/Rsp`：给选线 UI。`SwitchLine` **P1 不做**，客户端 Leave + Enter。

### Redis（与现有并存）

```text
map:inst:{id}           # 现有 hash + kind, line_no, party_id（副本预留）
map:occ:{id}            # 现有玩家集合
map:pres:{player}       # 现有
map:lines:{realm}:{tpl} # ZSET score=line_no member=instance_id
map:line:{realm}:{tpl}:{line_no}  # STRING instance_id，SET NX
```

`LEGACY_POOL` 继续用 `map:pool:{realm}:{tpl}` ZSET，不写 `map:lines:`。

### Lua 选线（一条脚本）

1. 指定 `instance_id` → 按 kind 校验后 `try_join`；LINE 满 hard → `ERR_MAP_FULL`（指定实例）或走线号错误。
2. 指定 `line_no` → `GET map:line:...`；无则 `ERR_MAP_NO_LINE`（默认不自动建指定线）。
3. 系统选线 → 未满 soft 的线里选 occupancy 最小（并列小 `line_no`）；否则开新线（`SET NX` 防双号）。
4. `max_lines` 且都满 hard → `ERR_MAP_LINE_LIMIT`。

### 错误码（写入 `docs/protocol/public-error-codes.md`）

| code | 何时 |
|------|------|
| `ERR_MAP_LINE_FULL` | 指定线满 hard |
| `ERR_MAP_NO_LINE` | 线号不存在 |
| `ERR_MAP_LINE_LIMIT` | 不能再开线 |

已有 `ERR_MAP_FULL` / `ERR_MAP_UNKNOWN_TEMPLATE` / `ERR_MAP_DATA_MISMATCH` 不动。

### 验收

1. 指定线满员第三人：错误，第三人 `map_instance_id` 不变。
2. soft=2，三人系统选线：出现 2 条线，第三人不进已满软顶线（进新线）。
3. 并发开线：无重复 `line_no`，无超 hard。
4. LEGACY 模板 `EnterMap(0)` 仍是 50 人池。
5. P0 分布验收继续绿。

### 建议改动文件

- `proto/game.proto`、`proto/session.proto`（Resolve 透传 `line_no`）
- `game/PlacementStore.cpp` 拆 `ReserveLine` / 保留 `ReservePublicSlot`
- `runtime/brpc/SessionServiceImpl.cpp`
- `GatewayEnterMapOrchestrator`（只多带字段，编排顺序不变）
- `config/maps/*`、`test/placement_store_test.cpp`、新 `test/map_line_test.cpp`

---

## 6. P1' — 出生打散与 AOI（可并行）

- 进图落点：模板 spawn ± `spawn_scatter_radius`（walkable）。
- AOI 半径按模板；主城建议小于 1002 的 32。
- 不改 Placement 键。

验收：同线 200 人时 `aoi_snapshot` 人数 p95 < 128，不再整线互相可见。

---

## 7. P2 — DUNGEON

### proto

`CreateDungeonReq`：`player_id`, `template_id`, `member_player_ids[]`, `operation_id`  
`CreateDungeonRsp`：`map_instance_id`, owner, epoch, members

队员随后 `EnterMap(map_instance_id)`。非队员 → `ERR_DUNGEON_NOT_MEMBER`。已关 → `ERR_DUNGEON_NOT_FOUND`。

### Redis

```text
map:inst:{id}  kind=DUNGEON, party_id
map:members:{id}   SET
# 不进 map:lines / map:pool
```

Owner：队长当前 Logic 若健康则用，否则 `id % N`。

空销毁：`occupancy==0` 且超过 `empty_close_delay`（默认 30s）→ CLOSED，删 members，Logic 卸 Runtime。

### 验收

- 非队员 Enter 失败。
- 空本超时后再 Enter → `ERR_DUNGEON_NOT_FOUND`。
- 队员可进；Logout 释放 occ。
- 不写匹配器。

---

## 8. 进图编排（P1/P2 都不改顺序）

```text
Client EnterMap / CreateDungeon
→ Gateway 校验 Hello + Session fence
→ Session Lua 占位
→ Owner ≠ sticky Logic 则 Transfer Saga
→ GameLogic MapRuntime.Enter + ConfirmSlot
→ 回 self + aoi_snapshot
失败：Release 或 AbortTransfer，不得双实体
```

`operation_id` 贯穿。Bind 失败不得留 occ。

---

## 9. P3 — 换线 / 排队 / 热迁 / Session 多开 / 图邮箱

| 项 | 做法 |
|----|------|
| `SwitchLine` | Session Lua 同模板原子迁占位；指定线满员 `ERR_MAP_LINE_FULL`；Gateway 再走 EnterMap Transfer |
| 排队票 | `EnqueueMap` 发 `queue_token`；队首且有空位后 `EnterMap(queue_token)` 兑换 |
| DRAINING / 热迁 | `DrainMap` 拒新进；`LiveMigrateMap` Drain→换 Owner→改 Session 路由；旧 Logic Unload |
| Session ×2 | `start_formal.sh` 默认 `GAMEMESH_SESSION2=8402`；Gateway `list://` + rr |
| 图邮箱 | GameLogic `PlayerSerialQueue::MapMailbox()` 按 `map_instance_id` 串行 Enter/Move/Leave |
| 跨 Logic / 跨线通讯 | **不走 AOI**；World 聊天/私聊/邮件；`QueryOnlineState` 带线号后 `SwitchLine` 见面。见 `player-comms.md` |

未做：跟队长线、MapPartition、VIP NAT 限流改写、10 万硬件。

---

## 10. 当前工单

**P0–P3 已落地。** 1001 仍为 `LEGACY_POOL`。1002 为 LINE（苏州，soft 200 / hard 400）。演示 **1101=LINE**、**2101=DUNGEON**。

空线回收：Session **正式常开** `CloseIdle`（默认 5s 扫 `map:idle`）。LINE 空超过 `empty_close_delay`（1002=300s）且 READY 线数 > `min_lines`（1）才 `CLOSED`。`GAMEMESH_EXPERIMENTAL_PLACEMENT_RECOVERY` 只控制失联热迁。

客户端分线：`QueryMapLines`；`EnterMapRsp.lines` / `SwitchLineRsp.lines` 快照；Push `map.lines.v1`（内层 `QueryMapLinesRsp`，可合并、不可靠）。切线：`SwitchLine`（`line_no>0`，满员 `ERR_MAP_LINE_FULL`）。

监控：`/metrics` 导出 `gamemesh_map_line_occupancy` / `gamemesh_map_line_count`（label `owner`=`gl-*`）。`/monitor` 有分线表。
