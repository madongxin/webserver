# AOI 200 订阅合同

## 上限

每个观察者最多订阅 200 个其他实体。默认 `aoi_max_visible = 200`，实例创建时冻结，允许范围 1..200。同一实例的快照、ENTER、LEAVE、MOVE 使用同一个上限。`SetMaxVisible` 不改已经创建的实例。

配置在地图 manifest 的 `aoi_max_visible`。缺省按 200。写出小于 1 或大于 200 的值会被目录加载钳回 200。1002 显式写 200。LINE 的 soft/hard/max_lines 与这个视野上限无关：1002 仍是 soft 100、hard 400、max_lines 8。

## 方向

- `visible_targets[viewer]`：这个观察者当前订阅的目标。
- `watchers[target]`：当前订阅了这个目标的观察者。

人多到超过上限时，关系可以不对称：A 看见 B，B 不必看见 A。

## Top-K

候选是视野半径覆盖的格子里的其他玩家。半径 0 只含当前格。格子坐标一律是 `WorldToAoiCell`：`floor((x - bounds_min) / cell_size)`。

排序键：距离平方更小者优先；距离相同则 `player_id` 更小者优先。不随机抽样。

## 事件

- 进图：进入者收到快照，不收到指向自己的 ENTER。已在视野内的观察者收到 ENTER。
- 离开或断线：观察者收到 LEAVE，订阅边删除。
- 移动：只有新旧可见集都包含该目标的观察者收到 MOVE。跨格或邻域超过上限时重算差集，可能同时产生 ENTER 和 LEAVE。
- 积压时同一收件人、同一实体的 MOVE 按约 100ms 抽样（目标 ≥10 次/秒），窗内只留最新，跨窗样本都发出并保留递增 `state_seq`。ENTER/LEAVE 保持顺序。`AoiEvent.server_time_ms` 带权威时钟。

## 断线、重连、换图

断线发 LEAVE，实体从该图移除。重连后用新的 gateway / session 重建订阅，之后的 MOVE 使用新 session。换图先在旧图 LEAVE，再在新图进图；同一玩家只出现在一张图上。

## 客户端

客户端应按 ENTER/LEAVE/MOVE 维护本地集合，并以快照为收敛结果。人堆在一起时，看不到的人不是丢包。
