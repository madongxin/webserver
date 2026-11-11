# AOI 扇出 v2（只改服务端）

状态：设计，未改代码。确认前不实现。

目标：带宽不高于现有 2.4 Mbps 预算的前提下，降低 AOI 扇出，并让 LEAVE 在写进连接之前一直留在服务端状态里。

范围：GameMesh 服务端。不改客户端工程。客户端接口只约定，不在本文实现。

## 0. 现有行为核对

下列以当前代码为准。和口述不一致的地方单独标出。

| 口述 | 代码 | 结论 |
| --- | --- | --- |
| 格子 32 米，半径 2，视野 5×5，用呈现坐标 | 图 1002 的 `aoi_cell_size` 是 32（`config/maps/1002.grid.json`）。`MapRuntime::view_radius_` 默认 2，`config/gamelogic.cnf` 的 `aoi_view_radius_cells=2`。`NeighborhoodIds` / `VisibleIds` 用 `PresentedX/Z`。半径 r 的窗口是 `(2r+1)×(2r+1)` 格，r=2 时是 5×5。 | 一致。其它图的格子不是 32 米（1001 为 12，1101 为 8，2101 为 4）。本文的估算只针对 1002。 |
| 新进入发 ENTER；窗口内约每 180ms 一次 MOVE；同一 `state_seq` 配新坐标也发；坐标没变不发 | 逻辑侧 `AllowMoveAoi`（`MapRuntime.cpp`）在外推关闭时：呈现坐标有变化，且距上次发给这个观察者至少 180ms，才 `EmitTo` op=2。未变（平方差 `<= 1e-4`）不发。`state_seq` 不参与这个判断。 | 180ms 是**逻辑侧产生 MOVE 的门**，不是网上实际到达的间隔。网关账本还会再砍一刀，见下一条。 |
| 跨出窗口立刻 LEAVE，并在约 1.6 秒内补发 | `sync_pose` 对不在新窗口里的旧观察者立刻 `EmitTo` op=3，并写入 `leave_again`。`tries>=8` 且间隔 200ms 就停。第一次发出时 `tries=1`，之后每 200ms 再发，直到 `tries` 变成 8 后删除。从第一次到停发大约 **1.4 秒、最多 8 帧**，不是按送达停止。 | 口述的「1.6 秒」接近，但不是可靠送达。写失败也会计满次数后丢掉。 |
| 大于 0.4 米按 `move_speed` 走，不瞬移 | `Move` 里 `step_dist >= 0.4f` 才滑行。剩余距离 `< 0.4` 米时呈现点贴到权威点。 | 一致。 |
| 不用 vx、vz、move_start_ms | `AoiExtrapolateEnabled()` 仅当 `GAMEMESH_AOI_EXTRAPOLATE` 恰好为 `1`。默认关。`FillMoveSnapshot` 在关闭时不写这三个字段。 | 一致。 |
| 200 人上限在格子窗口内不生效 | `VisibleIds` 会把候选裁到 `max_visible`（默认 200）。`RefreshViewers` 随后把**仍在格子窗口内**的人塞回 `neu`，不发 LEAVE。`sync_pose` 的 `in_range` 用的是**不裁剪**的 `NeighborhoodIds`，窗口里的人都会进 `watchers`。`CollectFullView` 的全量快照才会真正裁到 200。 | 一致。推送兴趣集大于快照兴趣集。 |
| 不恢复 5cm 过滤 | 非外推路径用 `1e-4` 的平方距离判断「没动」，没有 5cm 过滤。 | 一致。 |

代码里还有口述没写到、但和 v2 直接冲突的行为：

1. `GameTcpGateway.cpp` 的 `MoveLayer` **恒返回 1**。账本里的近层/远层拆分、`kNearQuotaPercent=60`、`FarHardSilenceMs` 因此没有远层输入。`FarMoveIntervalMs`（默认 200）只在外推打开时有注释用途，非外推路径不读它。
2. `AoiMoveCoalesceLedger::Pick` 在非 legacy 下，若「距上次写出 ≥ 200ms」的人数大于字节预算允许的人数，会把本帧人数**抬到** `min(due, max_n)`，`max_n` 为 `kMaxMoveEntities=200`。这会让记账字节超过本连接预算，并增加 `aoi_move_budget_over_bytes`。注释写明这是为了避免客户端停几秒再跳几十米。
3. 近层静默硬上限 `NearHardSilenceMs` 默认 3000，远层 `FarHardSilenceMs` 默认 15000。因为没有人被标成远层，硬上限实际都走 3000。
4. 慢客户端清槽（`SlowClientTracker`，默认 30 秒没有任何 MOVE 写进内核）把槽里**最后一份 MOVE 副本**放进 `resync`，下次写出时再 `Offer`。它不包含「客户端多出来的实体」，也不能让客户端删除。

## 1. 预算算式（估算，不是实测）

默认 `GAMEMESH_EGRESS_BUDGET_MBPS=2.4`，`GAMEMESH_GATEWAY_INSTANCES=2`。

```
每网关字节/秒 = 2.4e6 / 8 / 2 = 150000
每连接字节/秒 = 150000 / 该网关在线连接数
```

`FlushMoveSlot` 用本网关 `BoundPlayerCount()` 做除数。906 人若均分到两个网关，每网关约 453 连接：

```
331 字节/秒/连接
只按 MOVE 负载 52 字节：331/52 ≈ 6.4 个事件/秒
```

`AoiMoveCoalesceLedger.h` 里「大约 6 人」指的就是这个数，**没把每帧 66 字节的线上头算进去**。一帧只打一个头时：

```
(331 - 66) / 52 ≈ 5.1 个事件/秒
```

这是估算。人头不均分、帧里事件数变化、ENTER/LEAVE 占同一预算时，都会偏离。本文不引用压测实测值。

推论：906 在线且**不允许超过预算**时，一条连接大约只能维持 **1 个持续移动的邻居以约 180ms 更新**（1000/180≈5.6 次/秒，和 5 次/秒的预算同量级）。近层若有 K 个都在走，严格 180ms 需要约 `K×5.6` 次/秒，K≥2 就会超出 5 次/秒。近层静默硬上限若仍为 3 秒，5 次/秒的轮转大约覆盖 15 个近层实体（5×3）。再多的人只能更稀，或者突破预算。v2 选择不突破预算。

## 2. 开关

每项独立，未设置等于今天的行为。进程启动时读取；改环境变量后重启生效。半径写在 `InstanceState` 上，中途改环境变量不改已创建的图实例。

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `GAMEMESH_AOI_V2_RADIUS_CELLS` | 未设置 | 设置后覆盖该进程的视野半径。实验值 `1`。不改 `map_manifest` 和 `gamelogic.cnf` 里的 2。 |
| `GAMEMESH_AOI_V2_LAYERS` | 未设置或 `0` | `1` 才启用近远分层，并关掉「due≥200ms 就把人数抬过字节预算」的提额。 |
| `GAMEMESH_AOI_V2_VISIBLE_SET` | 未设置或 `0` | `1` 才用「已通知集合」差集产生 ENTER/LEAVE，并取消 8 次补发上限。 |
| `GAMEMESH_AOI_V2_HYSTERESIS` | 未设置或 `0` | `1` 才把进入半径和离开半径分开。 |
| `GAMEMESH_AOI_V2_RECONCILE` | 不实现 | 对账消息。本文只写接口。确认前不加字段、不发消息。 |

回滚：去掉对应环境变量并重启。四个已实现开关互不依赖，可以只关其中一个。关 `VISIBLE_SET` 时，未确认的 LEAVE 差集会在重启时消失，客户端可能留下旧模型，直到重进图的全量快照。这是回滚代价，写在第 8 节。

## 3. A. 缩小视野与近远分层

### 3.1 半径

`NeighborhoodIds` 的观察者数等于目标周围 `(2r+1)^2` 个格子里的在线人数。1002 上：

| 半径 | 窗口 | 面积 | 相对 r=2 |
| --- | --- | --- | --- |
| 2（现状） | 5×5 | 25×32×32 = 25600 m² | 1 |
| 1（实验） | 3×3 | 9×32×32 = 9216 m² | 9/25 = 0.36 |

这是几何面积比，**估算**。均匀撒点时，观察者数和由此产生的 ENTER/LEAVE 大约按这个比例下降。人堆在小于 3×3 格（约 96 米见方）的一团里时，两种半径都罩住整团，扇出几乎不降；要等人走出这一团，半径 1 才会少看到远处的人。本文没有这次压测的实测观察者数。

事件率同样是估算：逻辑侧一个移动实体每 180ms 向每个观察者产生一次 MOVE，约 `观察者数 × 5.6` 次/秒。网上实际 MOVE 次数被每连接约 5 次/秒卡住。半径从 2 降到 1，在可见的移动人数仍大于这个数时，**出口字节几乎不变**，下降的是被预算丢掉的 MOVE、以及进出窗口的 ENTER/LEAVE。可见移动人数降到约 5 以下之后，出口才会低于预算。

### 3.2 近远层和账本

仅 `GAMEMESH_AOI_V2_LAYERS=1`。

- 近层：落在观察者 3×3 格内，且呈现距离 `<= GAMEMESH_AOI_V2_NEAR_METERS`。建议默认 **32 米**（一格）。代码里已有 `kNearM=16`，但非外推路径把 `far` 丢弃了，`MoveLayer` 又恒为 1，所以 16 米今天不参与调度。不用 16 米做默认，避免和这个死常量绑在一起。
- 远层：在视野窗口内、但不满足近层。逻辑侧产生间隔 `GAMEMESH_AOI_V2_FAR_INTERVAL_MS`，建议默认 **1000**，允许配置 600–1000。标注待压测校准。
- 近层逻辑侧间隔保持现有 180ms。

和账本的衔接（`AoiMoveCoalesceLedger`，`GameTcpGateway.cpp` 的 `MoveLayer` / `FlushMoveSlot`）：

- `MoveLayer` 在开关打开时返回 `1`（近）或 `0`（远），不再恒为 1。外推仍然默认关。
- `Pick` 继续用 `kNearQuotaPercent`（默认 60）把本帧名额先分给近层，剩下给远层。
- 近层静默硬上限沿用 `NearHardSilenceMs`（默认 3000）。远层沿用 `FarHardSilenceMs`（默认 15000）。这两个上限是「排队年龄」，不是逻辑侧的产生间隔。
- 开关打开时，**删除**「due≥200ms 就把 `limit` 抬到 `min(due, 200)`」这段。否则远层降频不会发生，字节会继续超预算。
- 一连接每个实体仍然只留最新坐标：`Offer` 覆盖同一 `entity_id` 的槽，不改。

### 3.3 预算不够时

优先级：

1. 远层本帧名额降到 0。远层实体留在槽里，不另开队列。
2. 近层在剩余字节里按静默从长到短取，人数不超过 `(剩余字节 - 66) / 52`。
3. 近层静默硬上限到期时，只在**本秒还剩的字节**里优先发这些近层实体，不把 `limit` 抬过字节余额。发不完的继续留在槽里，年龄记入静默分位数。
4. 总记账字节（帧负载 + 每帧 66）不超过该连接本秒配额。不提高 `GAMEMESH_EGRESS_BUDGET_MBPS`。

近层保底额度：本帧至少尝试为近层保留 `max(1, limit × 近层名额百分比 / 100)` 个名额，但这个保留**不能**使本秒记账超过配额。配额连 1 个事件都放不下时，本帧一个都不发，包括到期的近层。静默会超过 3000。这种情况打指标，不偷偷加字节。

和「约 6 个实体/秒」的关系：那是 52 字节、不计 66 字节头、453 连接均分时的估算。v2 用同一配额，但按「负载 + 每帧一个头」记账，满员大约是 **5 次/秒/连接**。预期静默（估算，不是实测）：

- 只有 1 个近层邻居在持续移动：大约 200ms。
- 近层 8 个都在移动、远层为 0：5 次/秒轮转，静默大约 8/5 秒 = 1.6 秒，低于 3 秒上限。
- 近层 8 个加上远层 20 个都在移动：远层先被饿死，近层仍约 1.6 秒；远层静默落到 15 秒硬上限附近，并停在那里，不堆积新帧。

### 3.4 与现有慢客户端降级的关系

`SlowClientAction::Degrade` 今天把名额改成 50%、间隔改成 1 秒。v2 分层打开时，降级只再把远层间隔乘 2，不改近层 180ms 的产生门，也不提高字节配额。清槽和关闭的阈值不变（10 秒降级、30 秒清槽、90 秒关闭）。

## 4. B. 可见集合

仅 `GAMEMESH_AOI_V2_VISIBLE_SET=1`。关闭时仍走今天的 `watchers` / `visible_targets` / `leave_again`。

### 4.1 状态

每个观察者一份已通知集合，挂在 `MapRuntime::InstanceState` 上，替换对 LEAVE 的用途，不另做第三份长期索引：

- `notified`：观察者 → 实体 id → `NotifiedItem { version, op_pending }`
- `version`：该观察者每次集合变化加 1，写进当次 ENTER/LEAVE 的现有 `state_seq` 旁边的**不新增字段**。版本只存在服务端，用来判断补发是不是旧的。线上事件仍用现有 `AoiEvent`。
- `op_pending`：`none` / `enter` / `leave`。`leave` 表示客户端按服务端认知还拿着这个人，但当前窗口已经不要他，LEAVE 尚未被网关接受。

当前窗口集合每轮现算，不长期保存：分层开关关时等于格子窗口；分层开关开时仍是格子窗口（远层只降 MOVE 频率，不把人赶出视野）。上限开关见 4.3。

### 4.2 ENTER/LEAVE

差集：

- 在当前集合、不在 `notified`：ENTER，插入并标 `enter`。网关接受后改 `none`。
- 在 `notified`、不在当前集合：LEAVE，标 `leave`。网关接受后从 `notified` 删除。
- 已标 `leave` 的，下一轮仍发 LEAVE，**没有 1.6 秒或 8 次上限**。
- 标了 `leave` 的人又走进当前集合：取消 leave，改发 ENTER（或若网关还没接受上一次 LEAVE，则直接把 pending 改回 `none` 并不再发 LEAVE）。

「送达」定义为：网关 `send_frame_urgent` 返回已写入应用缓冲或当场写出（返回值 `> 0`）。不是 TCP ACK，也不是客户端应用。内核里已经写出的字节不撤回。写失败则 `op_pending` 保持 `leave`。

这个确认在发布线程拿到 Push 结果之后，重新拿 `InstanceState` 的锁写回。计算差集和 `EmitTo` 仍然在锁内，且只组批次，不在锁里做 RPC。

`leave_again` 在此开关打开时不再写入。

### 4.3 上限和滞回

`GAMEMESH_AOI_V2_VISIBLE_CAP` 默认 200，与 `max_visible_` 相同。仅本开关打开时，它才是推送集合的硬上限。`RefreshViewers` 里「仍在格子里就塞回」的分支在此开关下不执行。

排序：呈现距离，距离相同则 id 较小者优先。与 `VisibleIds` / `CollectFullView` 现有次序一致。

挤出：当前集合按距离取前 N。已通知且仍在窗口内、但落到 N 名之外的人，进入 LEAVE 差集。为避免第 N 与第 N+1 名对调：

- 已在集合里的人，只有当他的名次 `> N + GAMEMESH_AOI_V2_CAP_SLACK` 才被挤出。建议默认 slack **8**。
- 不在集合里的人，只有当名次 `<= N - slack` 才允许 ENTER 进来占坑。slack 用尽（集合不足 `N - slack`）时，按名次补到 N。

被挤出的人走和走出窗口相同的 LEAVE：留在差集里直到网关接受。

### 4.4 低频对账（只设计，不实现）

开关名预留 `GAMEMESH_AOI_V2_RECONCILE`。确认前：不加 proto 字段，不发消息，不写实现。

建议消息，字段号取 `GameResponse` 里当前未占用的新号（实现时再对 `game.proto` 扫一遍，不改已有字段号）：

- 消息名 `AoiVisibleDigest`
- `map_instance_id`：uint64
- `version`：uint64，等于该观察者 `notified` 的版本
- `entity_ids`：repeated uint64，**只含已通知且 pending 不是 leave 的 id**。正在补 LEAVE 的 id 不放进去，避免客户端先删再等 LEAVE。

放在 `GameResponse` 的新 oneof 上。旧客户端按 proto3 忽略未知字段。不要求旧客户端回包。

客户端约定（本文不实现）：收到摘要后，删除本地有、列表里没有的模型。这是 LEAVE 丢失后的兜底，不是主路径。主路径仍是 op=3。

周期 `GAMEMESH_AOI_V2_RECONCILE_SEC`，建议默认 20，允许 10–30。每个观察者错开，避免同一秒打满。摘要字节计入同一 2.4 Mbps 预算，排在 MOVE 之后、LEAVE 之前：LEAVE 仍优先于摘要。

### 4.5 清槽后的 needs_resync

今天 `FlushMoveSlot` 在 `ClearSlot` 时把 `by_entity` 里最后的 MOVE 拷进 `resync`，稍后原样 `Offer`。这只补位置，不核对「客户端多出来的人」。

开关打开后，清槽标记 `needs_resync` 时逻辑侧做一次和进图相同的可见集合计算：

- 当前集合中有、槽里没有的：ENTER（或并进补发快照）。
- 槽里有、当前集合没有的：进入 LEAVE 差集。
- 补发快照的实体集合等于这次计算出的当前集合，而不是清槽前槽里碰巧剩下的 MOVE。

快照仍然是现有 `AoiEvent` op=1 的一批，不新增字段。客户端已有的人收到 ENTER/MOVE 都只更新；不在快照里、且随后收到 LEAVE 的人删除。不依赖客户端插值缓冲。

### 4.6 线程、锁、内存

| 数据 | 线程 | 锁 |
| --- | --- | --- |
| `notified`、差集、版本 | gamelogic 上算 AOI 的线程 | `InstanceState::mu`（现有 `LockInst`）。与 `sync_pose`、`RefreshViewers`、`Move` 同一把锁。 |
| 组好的 `AoiPushBatch` | 锁外交给现有 AOI 发布队列 | 不在 `InstanceState::mu` 里做 RPC。 |
| 网关是否写出 | gateway 连接所在的 EventLoop | `MoveCoalesceSlot` 自己的锁。不拿 `MapRuntime` 的锁。 |
| 写回 `op_pending` | gamelogic 发布线程收到 Push 结果后 | 再次 `LockInst`，只改对应观察者的几项。 |

`FlushMoveSlot` 仍只从槽里挑 MOVE。它不遍历 `notified`。LEAVE 不进 MOVE 合并槽，避免被「每实体只留最新坐标」覆盖掉。清槽补发由逻辑侧在 `needs_resync` 时把 ENTER/LEAVE 重新 `EmitTo`，而不是让 `FlushMoveSlot` 去读逻辑的集合。

内存估算（不是实测）：一个人 200 个 id，双向索引，哈希节点按约 48 字节估，906 人若全在同一个逻辑进程上大约 `906 × 200 × 48 × 2 ≈ 17MB`。现有 `visible_targets` 与 `watchers` 已经是这个量级。开关打开时用 `notified` 承担 LEAVE 状态，不再长期保留第三份集合，增量主要是每个条目多一个版本和一个枚举，大约再加几 MB。两个逻辑进程分图时按各自分到的人数计，不是再乘 2。

## 5. C. 边界滞回

仅 `GAMEMESH_AOI_V2_HYSTERESIS=1`。

- 进入半径：`GAMEMESH_AOI_V2_RADIUS_CELLS`，未设置则 2。
- 离开半径：进入半径 + `GAMEMESH_AOI_V2_LEAVE_EXTRA_CELLS`，建议默认 **1**。离开判定用更大的窗口。人必须走出外圈才 LEAVE，走进内圈才 ENTER。停在两圈之间的人维持原状态。

现有代码没有距离滞回。`InterestKeepMeters` 已删除。`RefreshViewers` 把「还在同一视野窗口里」的人留住，只是为了不让 200 人裁剪把窗口内的人 LEAVE 掉，进和出用的是同一个半径。v2 的差异只是：进出用两个半径；上限挤出用名次 slack，而不是再加一圈距离。

滞回和上限同时打开时，先按离开半径得到候选，再按第 4.3 节裁到 N。避免人在边界上因为半径和名次各触发一次进出。

## 6. D. 指标

前缀 `gamemesh_`。每个序列在**没有任何样本**时不输出该时间序列。监控上显示无数据，不把缺测写成 0。已有序列保持现状，避免把旧图全部改成「时有时无」。

| 名字 | 类型 | 单位 | HELP 要点 |
| --- | --- | --- | --- |
| `gamemesh_aoi_visible_count` | 分位数 p50/p95/p99/max | 人 | 每个观察者当前已通知且非 leave-pending 的人数。 |
| `gamemesh_aoi_sub_enter_per_sec` | gauge，由现有计数的区间增量得到；若本进程还没有过 ENTER，不输出 | 次/秒 | ENTER 速率。现有 `gamemesh_aoi_sub_enter_total` 保留。 |
| `gamemesh_aoi_sub_leave_per_sec` | 同上 | 次/秒 | LEAVE 速率。现有 total 保留。 |
| `gamemesh_aoi_edge_flap_10s` | gauge | 次 | 同一观察者与同一实体在 10 秒内 ENTER 之后又 LEAVE（或相反）的来回次数。无来回则不输出。 |
| `gamemesh_aoi_move_silence_near_p50_ms` 及 p95/p99/max | gauge | 毫秒 | 已有近层静默。分层关闭时人人是近层，序列含义不变。无待发实体时不输出。 |
| `gamemesh_aoi_move_silence_far_p50_ms` 及 p95/p99/max | gauge | 毫秒 | 已有远层静默。分层关闭时没有 layer 0，今天就不会有远层样本。 |
| `gamemesh_aoi_leave_pending` | gauge | 个 | 所有观察者 `op_pending=leave` 的条目数。开关关闭时不输出。 |
| `gamemesh_aoi_leave_pending_age_ms` | p50/p95/p99/max | 毫秒 | 这些条目自进入差集起的年龄。无条目时不输出。 |
| `gamemesh_aoi_reconcile_total` | counter | 次 | 对账消息发出次数。未实现前不注册。 |
| `gamemesh_aoi_reconcile_removed_total` | counter | 个 | 对账导致客户端应删除的 id 数（服务端统计的是摘要里故意不含、且当时仍在旧 notified 里的人数）。未实现前不注册。 |
| `gamemesh_aoi_cap_evict_total` | counter | 次 | 被上限挤出并进入 LEAVE 差集的次数。开关关闭时不注册。 |

## 7. E. 测试方案

只测 `MapRuntime` 与账本，不打开 socket。修复前对应断言应当失败。实现时再落成测试，本文不改测试代码。

1. **卡住超过 1.6 秒。** 观察者已收到实体。把网关接受回调固定为失败。推进超过 8×200ms。差集里这个实体仍是 `leave`，并且下一次计算还会再产生 LEAVE。然后回调改为成功。差集清空，且不会再对这个已删除的实体发 MOVE。今天的 `leave_again` 会在约 1.4 秒后停止，此断言失败。
2. **同一片格子里的人群。** 窗口内放入超过 200 人。可见集合大小 `<= N`（默认 200）。让第 N 与第 N+1 名的距离来回对调一个 slack 以内。ENTER/LEAVE 次数为 0。今天 `sync_pose` 会把窗口内的人全部放进观察者，此断言失败。
3. **边界来回。** 实体在进入半径内侧与离开半径外侧之间走 20 个来回。打开滞回后，ENTER/LEAVE 不超过 2 次（进一次、出一次）。关闭滞回的对照会随每次跨格增长。今天没有两圈半径，跨格即进出，此断言在开关打开前失败。
4. **预算。** 近层 4 个、远层 30 个都在移动，每连接配额按 331 字节/秒。一个 1 秒窗口内远层发出次数少于近层，近层静默不超过 `NearHardSilenceMs`，记账字节不超过配额。今天 `Pick` 会把 due≥200ms 的人数抬过字节余额，字节断言失败。
5. **清槽恢复。** 槽里留下 3 个已不在窗口内的旧 MOVE，窗口内另有 2 个槽里没有的人。`needs_resync` 之后的补发包含这 2 个的 ENTER，且对那 3 个产生 LEAVE。今天只把槽里的旧 MOVE 重新 `Offer`，此断言失败。

## 8. F. 验证与回滚

压测时每次只开一类开关，顺序固定：

1. 只设 `GAMEMESH_AOI_V2_RADIUS_CELLS=1`，再开 `GAMEMESH_AOI_V2_LAYERS=1`。
2. 在上一步之上开 `GAMEMESH_AOI_V2_VISIBLE_SET=1`。
3. 在上一步之上开 `GAMEMESH_AOI_V2_HYSTERESIS=1`。

对账开关不在这轮打开。

906 在线、人满之后的第 1、3、5 分钟各取一次窗口。与旧构建并排，写明构建 SHA 和时间窗口。看这些量：

- 每观察者可见数（p50/p99）
- AOI 事件/秒（ENTER、LEAVE、MOVE 分开）
- 出口 Mbps（现有 `rate(gamemesh_gateway_tx_bytes_total)*8/1e6`）
- 近远层静默 p99 与 max
- 心跳 p99（现有心跳延迟序列；没有样本则写无数据）
- 自己的移动回包 p99（现有 move 回包延迟；没有样本则写无数据）

回滚：删除对应环境变量，重启集群。不要在进程存活期间切换，避免已创建图实例的半径和差集停在半开状态。

本设计未验证、实现后仍要单独写明的部分：

- 公网丢包。送达定义停在「网关写入」，写进内核之后的丢包要靠以后的对账，而不是这一轮。
- 半径改成 1 之后，人在 3×3 格外突然消失，对操作的影响。这是产品感受，不是本仓库能测的。
- 近层 32 米、远层 1000ms、名次 slack 8、离开半径多 1 格，都是建议值，没有压测校准。

## 9. 行为对照

| 行为 | 现在 | v2（对应开关打开时） | 位置 |
| --- | --- | --- | --- |
| 视野半径 | 进程默认 2，图实例创建时抄进 `InstanceState::view_radius` | 环境变量可覆盖为 1，默认不覆盖 | `MapRuntime::ResolvedRadius`，`Enter` |
| 谁收到 MOVE | 窗口内所有人，逻辑侧 180ms | 近层 180ms，远层配置间隔 | `AllowMoveAoi`，`MoveLayer` |
| 账本分层 | `MoveLayer` 恒为 1 | 近=1，远=0 | `GameTcpGateway.cpp` `MoveLayer` |
| 字节不够 | `Pick` 把人数抬到 due≥200ms 的人数，可超预算 | 不抬。先停远层，再在余额内发近层 | `AoiMoveCoalesceLedger::Pick` |
| LEAVE 停止条件 | `leave_again` 最多约 1.4 秒 / 8 次 | 留在差集里，直到网关写入返回成功 | `MapRuntime::sync_pose`，发布结果回写 |
| 200 人上限 | 推送路径在窗口内不生效 | 硬上限，名次 slack 内不来回挤 | `VisibleIds`，`RefreshViewers`，`sync_pose` |
| 进出半径 | 同一个半径 | 离开半径 = 进入半径 + 额外格数 | `NeighborhoodIds` 的调用处 |
| 清槽补发 | 槽内最后 MOVE | 按当前可见集合重算 ENTER/LEAVE | `FlushMoveSlot`，`SlowClientTracker` |
| AOI 帧怎么写出 | `aoi.delta.v1` 走 `send_frame_urgent`，不再进可靠队列（工作区里已有这一步，尚未作为 v2 开关） | 保持。差集的清除以这次写入的返回值为准 | `GatewayPushServer.cpp` 的 `env.send` |
| 对账 | 无 | 只设计，不实现 | 新消息，确认后再加 |

## 10. 风险

1. 关掉「due≥200ms 提额」之后，窗口里人一多，静默会从大约 200ms 升到第 3.3 节的估算。客户端只播收到的点，看起来会一顿一顿。这是用字节上限换的，不是意外。
2. 半径 1 在人群散开后会少看到约 3/5 的面积。簇在 96 米以内时几乎看不出差别。
3. 名次 slack 太小会在第 N 名附近抖动；太大则超限更多。默认 8 没有数据。
4. 离开半径多 1 格时，人已经看不见角色，模型还留着，直到走出外圈。这是用滞回避免边界闪烁，和「一出窗口立刻删」相反。两个开关不要在同一次压测里一起解释结果。
5. 送达只到网关写入。内核之后的丢包仍然会留模型，直到以后的对账。对账未实现。
6. 回滚 `VISIBLE_SET` 并重启后，差集丢失。旧客户端会留着重启前的模型，直到重新进图。
7. 全量快照今天裁到 200，推送不裁。打开上限后两边才一致。若只开半径、不开可见集合，快照和推送仍不一致。
8. `send_frame_urgent` 在 bRPC 线程上等连接线程，最多约 200ms。LEAVE 不再进可靠队列之后，发布线程会更常走这条等待。需要在压测里看发布耗时，不在设计里假定它免费。

## 11. 需要拍板

1. 预算和近层 180ms 不能同时满足时，是否同意**保预算、让静默变长**？本文按保预算写。
2. 近层距离阈值用 32 米，还是改回代码里那个未使用的 16 米？
3. 远层默认间隔用 1000ms，还是 600ms？
4. 离开半径比进入半径多 1 格，是否接受？
5. 上限滞回的名次 slack 用 8，是否接受？
6. 对账消息是否采用第 4.4 节的 `AoiVisibleDigest`？确认前不实现。
7. 「送达」停在网关写入成功、而不是客户端 ACK，是否接受？
8. 半径实验是否只走环境变量，不改 `map_manifest` 和 `gamelogic.cnf` 的默认 2？
