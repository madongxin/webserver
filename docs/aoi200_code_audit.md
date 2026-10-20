# AOI 200 代码审计

基线 `main` `3bb747ff2ed982f2d55298e0f9a2343085571ed9`。本文件描述该提交之上的未提交改动。远端未再 fetch。正式集群未重启。

## 50 → 200

`MapRuntime` 默认 `max_visible_` 为 200。`Enter` 把调用方传入的 `max_visible` 冻结到 `InstanceState`，范围钳制到 1..200。`SetMaxVisible` 只影响之后新建的实例。`VisibleIds`、快照、ENTER、LEAVE、MOVE 都读实例上的这个值。

地图策略 `MapScenePolicy::aoi_max_visible` 默认 200。`MapCatalog` 读 manifest 字段 `aoi_max_visible`，小于 1 或大于 200 都钳到 200。`config/maps/map_manifest.json` 的 1002 写了 200。`GameLogic::HandleEnterMap` 把 `pol.aoi_max_visible` 传给 `Enter`。1002 的 soft 100、hard 400、max_lines 8、spawn 未改。

旧的 50 人断言测试在进图前调用 `SetMaxVisible(50)`，用来保住平局规则，不是生产默认。

## 网格坐标

修复前：`PlaceInCell` / `VisibleIds` 用 `MapStaticData::WorldToAoiCell`（`(x - bounds_min) / step`），`NeighborhoodIds` 用 `floor(x / cell)`，并且 `view_radius <= 0` 直接返回空。`bounds_min` 非 0 时，邻居查找和落格不是同一套格子。

修复后：`NeighborhoodIds` 与 `VisibleIds` 都走 `WorldToAoiCell`。`ResolvedRadius` 把半径 0 收成只扫当前格，两边一致。

证据：`aoi200_subscription_test` 的 AOI200-01..06 退出码 0（2026-10-15）。

## 锁

`MapRuntime::mu_` 只保护 `maps_` 和 `player_map_`。每个实例有自己的 `mutex`。公开方法先在注册表锁下复制 `shared_ptr`，放开注册表锁，再锁实例。跨图 `Enter` 按 `map_instance_id` 升序锁两个实例。`player_map_` 的写入发生在实例锁放开之后。`Unload` 先从 `maps_` 摘掉，再锁实例清空。不在持有实例锁时再拿 `mu_`。

`entity_count` 和 `unloaded` 是原子量，注册表侧的空图回收不读实例容器。

并发证据：AOI200 测试里 4 线程各 30 次移动、4 个实例，`move_ok==120`，退出码 0。1/2/8 实例和 TSAN 未跑。

## Top-200 与快路径

`VisibleIds` 仍是邻域全扫描加 `nth_element`，距离平方优先、`player_id` 较小者优先。不是随机抽 200，也不是 O(1)。

`Move` 仅在同时满足下面条件时跳过全量 `RefreshViewers`：格子没变、邻域人数不超过 `max_visible + 1`、移动者当前可见集已经等于邻域去掉自己。这时只向已有 watcher 发 MOVE。400 人叠在同一格时邻域大于上限，走慢路径。

没有优化前的同 cap 对照计时。不能写加速比。

## 订阅与 Push

有向订阅保留：`visible_targets[viewer]` 与 `watchers[target]`。MOVE 只发给新旧集合都包含该目标的观察者。进图玩家拿快照，不给自己发 ENTER。ENTER/LEAVE 是差集。同 Gateway 的 PushBatch 合并与 MOVE 只留最新位置是上一轮已有行为，本轮没有改同步 PushBatch。

## 指标

`ServerStats` 增加了候选数、选中数、查询耗时分桶（100/500/2000/inf 微秒）、刷新耗时、ENTER/LEAVE/MOVE 计数、快路径计数、锁等待与持有累计。`OpsMetrics` 导出了其中一部分。分桶不是 HDR 的 p95。

## 未修

- 同步 PushBatch 未改。
- TSAN 未跑。
- 正式进程仍是上次重启时的二进制。
