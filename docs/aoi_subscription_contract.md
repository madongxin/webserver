# AOI 订阅合同

## 模型

视野是有向的。A 看得到 B，不表示 B 看得到 A。

- `visible_targets[viewer]`：viewer 此刻应该看到的其他玩家。
- `watchers[target]`：正在看 target 的 viewer。一个目标可以被超过 50 人看到；上限加在每个 viewer 的集合上，不加在 `watchers` 上。

候选仍来自该实例的 AOI 格子和 `view_radius`。`view_radius <= 0` 时集合为空。断开连接的玩家不进入别人的集合。

## 上限与平局

每个 viewer 最多 50 个目标。比较键是水平距离平方，相同则 `player_id` 更小者优先。同一输入重复计算得到同一集合（AOI-08）。

## 事件

| 变化 | 发给谁 | op |
| --- | --- | --- |
| 目标新进入 viewer 的集合 | 该 viewer | ENTER (1) |
| 目标离开 viewer 的集合 | 该 viewer | LEAVE (3) |
| 目标仍在集合内且位置变了 | 该 viewer | MOVE (2) |

进图回包里的快照等于该玩家 `visible_targets`，不再给进图玩家自己推一份 ENTER。重连同样以快照为准，并向新的观察者发 ENTER。

离开、断线、跨图进图都会先按旧订阅发 LEAVE，再让周围观察者按新的前 50 补 ENTER。同一玩家在一个实例里只有一条实体（AOI-04）。跨图之后旧图 `HasPlayer` 为假，旧邻居的后续 MOVE 不会再发给他（AOI-06）。

## 客户端

协议字段号没有变。ENTER / MOVE / LEAVE 仍走现有 AOI delta。客户端应按 `player_id` 做 upsert：重复 ENTER 覆盖，LEAVE 删除，MOVE 只更新已存在的人。非可靠 MOVE 可以丢，下一次 ENTER 或重连快照覆盖。

## 还没做

全局锁还在。格子里的候选人仍全部参与排序，50 只限制输出。`PushGrouped` 仍是同步 brpc。这两项没有第二套 AOI 权威。
