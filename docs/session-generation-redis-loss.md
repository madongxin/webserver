# Redis 丢失之后，generation 会不会回退

会。现有实现把 `generation`、fence token、`map_owner_epoch` 放在 Redis Hash 里，由 Lua 在键还在的时候加一。键没了，下一次就从 1 再计。这和「进程本地记住的最大值」不是同一个时钟。

## 现在的代码怎么计

`game/SessionStore.cpp` 的 `kLuaAcquire`：

- 读 `HGETALL`。没有键，或断线宽限已经过期并被 `DEL`，`old_gen` 当成 0。
- `old_gen > 0` 时 `gen = old_gen + 1`，否则 `gen = 1`。
- 同时写入新的 `token`。

重连脚本 `kLuaReconnect` 同样是 `(generation or 0) + 1`。校验脚本拿请求里的 token 和 generation 跟 Hash 里的当前值比，不一致就 `TOKEN_MISMATCH` / `GEN_MISMATCH`。

`map_owner_epoch` 在 Placement 的 Redis 记录里，也是存在 Redis 里的整数。Redis 清空后，新开的线会重新从目录里的初始 epoch 开始，不是从上次的值继续。

正式模式在 Redis **连不上** 时会失败关闭。Redis **连得上但数据没了**（重启没开 AOF、主从切换丢最后一段、误 `FLUSHDB`）在现有 Lua 里看起来就是「这个玩家第一次登录」。

## 迟到回调会不会被当成新会话

旧连接手里是旧 token 和旧 generation。Redis 清空后新登录会发新 token。旧 token 对不上 Hash 里的新 token，校验拒绝。

危险在 generation 这个数字本身：

- 清空前 generation 可以是 40。
- 清空后第一次 Acquire 得到 1。
- 任何只比较「数字变大了没有」、却没同时核对 token 的路径，会把 1 当成比 40 更旧，或者反过来把重启后的 1 和很早以前的 1 当成同一代。
- 进程内如果还留着「见过的最大 generation」，Redis 里的新 1 比它小。现有 Acquire **不会**读这个本地最大值，所以不会拒绝。

所以：token 还在比较的路径上，旧回调进不了新会话。generation 作为单调序号，在 Redis 丢数据后会回退，而且服务器不会因此拒绝登录。

## 和任务要求的冲突

要求是：发现 Redis `run_id` 变了就拒绝 `AcquireSession` 和 Bind，直到把 epoch 下界设成 `max(本地持久化最大值, 当前时间毫秒)`。

这和现在的设计冲突，原因有三点：

1. generation 是**每个玩家键**上的小整数，不是全局毫秒时钟。改成毫秒会让所有解析 `generation` 的 Lua、Gateway 顶号和测试的「加一」假设失效。
2. 没有本地持久化的 generation 高水位。要做下界，得先加一块掉电还在的存储，否则进程重启后下界自己也丢了。
3. Redis 清空后的第一次登录，今天就是合法的新会话。改成 fail-closed 会让每次 Redis 重启都登不进去，直到另做一次「基线重建」。现有集成测试和正式启动都没有这一步。

因此这次**不改代码**。下面两个做法可以以后选，不要混在同一次改动里。

## 备选 A：保持 generation 加一，另加 Redis 纪元

登录仍用现在的 `gen = old_gen + 1` 和 token。另外在 Redis 里放一个 `run_id`（Redis 自己的 `INFO server` 里有）对应的 `gamemesh:epoch`。Session 进程启动时读 run_id。和上次写入的不一样，就把 epoch 加一写回去，并拒绝 Acquire/Bind，直到管理员或启动脚本确认「这是一次空库，epoch 已提升」。

玩家键里多一个 `epoch` 字段。校验时 epoch 和 token、generation 一起比。旧连接的 epoch 较小，迟到回调被拒绝。新会话的 generation 仍可以从 1 开始，但 epoch 更大，所以整体序号不会和旧世界撞上。

代价：多一个启动闸门。Redis 正常重启但 AOF 还在时，run_id 也会变，必须把「数据还在」和「数据没了」分开，不能见到 run_id 变化就一律当空库。

## 备选 B：generation 直接改成单调毫秒

Acquire 时 `gen = max(redis 里的旧值 + 1, 本机毫秒时间)`。Redis 清空后，新 generation 是当前毫秒，大于任何旧的小整数。

代价：Lua 和 C++ 里所有「generation 只加一」的测试都要改。两台机器时钟回拨会让毫秒下界失效，所以仍要一个本地或 MySQL 里的最大值，取 max。这就是任务里的「epoch 基线」，它不再是小整数，客户端和顶号逻辑都要接受 64 位时间。

## 取舍

备选 A 不动现有 generation 的加一规则，只在外面加一代「这是哪一次 Redis 寿命」。和现在的 token 校验拼在一起，旧回调会被拒绝，新会话的 generation 不必大于历史上每一个旧整数，但 `(epoch, generation)` 这个对会更大。

备选 B 让 generation 自己单调，调用点更少，但把时钟和登录序号绑在一起，和当前 Lua 的语义差得更远。

在有一块可靠的「上次见过的 run_id / 最大 epoch」之前，两个方案都不该直接写进 Acquire。当前正式环境靠 token 不匹配挡住旧连接；generation 回退是真的，只是还没有单独的闸门。
