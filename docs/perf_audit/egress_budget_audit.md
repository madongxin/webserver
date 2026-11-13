# 出口预算和攒批为什么没压住线上字节（B0）

本轮只读核实，并加了每帧事件数、每帧 payload 的分位。发送、AOI、限速脚本的执行都没有改。`tc` 命令没有执行。

标注：下面写「实测」的是 2026-11-13 已经采到的数，或本机文件里读到的配置。写「代码推断」的是从当前源码推出来的，这次没有新的压测样本。缺的数写「无数据」。

## 1. 谁跑 tc，2.4 Mbps 从哪来

实测，本机文件：

- `/etc/rc.local` 是指向 `/etc/rc.d/rc.local` 的符号链接。`/etc/rc.d/rc.local` 第 23 行执行 `scripts/protect_ssh_egress.sh`。开机会跑。
- `scripts/start_formal.sh` 不调用这个脚本，也不执行 `tc`。正式启动不会重新下发队列。
- `deploy/` 下的 `compose.cluster.yml`、`compose.dev.yml`、`docker-entrypoint.sh`、`Dockerfile.runtime`、`prometheus/` 没有引用这个脚本，也没有 `tc`。`deploy/da700a4/` 是一份旧二进制目录，没有作为启动脚本来读。

`scripts/start_formal.sh` 第 15–17 行：注释写「两个 Gateway 共用一条出口。这里的 Mbps 是整机预算，进程内再按实例数拆开。」默认 `GAMEMESH_EGRESS_BUDGET_MBPS=2.4`，`GAMEMESH_GATEWAY_INSTANCES=2`。

代码推断，`AoiMoveCoalesceLedger::EgressBudgetBytesPerSec`：`2.4 * 1000000 / 8 / 2 = 150000` 字节/秒，每个网关一份。两台加起来是 300000 字节/秒，也就是 2.4 Mbps。

和 tc ceil 的关系：

- 类 `1:10` 同时匹配源端口 8081 和 8083，两台网关的游戏流量进同一个类。ceil 是这一类的上限，不是每台网关各一份。
- 旧脚本把这个类写成 rate 1600 kbit、ceil 2000 kbit，也就是两台合计最高约 2.0 Mbps。
- 应用预算 2.4 Mbps 大于这个旧 ceil。文档 `docs/perf_audit/aoi_move_egress.md` 写默认 2.4「留大约 20% 余量」。3 Mbps × 0.8 = 2.4 Mbps，和这句话对得上，但 `start_formal.sh` 里没有把 3 乘 0.8 的算式，3 Mbps 是这台机器云厂商公网出口的实测说明，不是脚本探测出来的。
- 现在脚本默认改成 rate 2200 kbit、ceil 2600 kbit（仍是两台共用）。2.4 Mbps 低于 2600 kbit（2.6 Mbps），高于旧的 2000 kbit。
- 代码推断：`SetEnforceByteBudget` 只有 `GAMEMESH_AOI_V2_LAYERS=1` 才为真，默认关。关的时候 `Pick` 会把名额抬到「间隔不少于 200ms 的实体数」（`AoiMoveCoalesceLedger.h` 约 160–173 行）。预算数字只在 `NoteKernelWrite` 里对 MOVE 帧累加，不挡住发送。所以 2.4 这个数默认不限制线上字节。

手动改当前正在跑的队列（由操作者执行，本轮没有执行）：

```bash
tc class change dev eth0 parent 1:1 classid 1:10 htb rate 2200kbit ceil 2600kbit prio 2 burst 16kb cburst 16kb
tc class change dev eth0 parent 1:1 classid 1:30 htb rate 400kbit ceil 500kbit prio 1 burst 16kb cburst 16kb
```

第二行是监控口，默认和现在一样。不要运行 `scripts/protect_ssh_egress.sh` 来做这次调整：那个脚本会先删掉根 qdisc 再重建。

## 2. 攒批为什么没把一帧填满

代码推断。

`MoveBatchMinEvents` 在 `runtime/MoveSendConfig.h`，读 `GAMEMESH_MOVE_BATCH_MIN`，默认 8。唯一的使用点是 `AoiMoveCoalesceLedger::Pick`：

```text
batch_wait = 没有硬静默 && 候选数 < 8 && 候选数 < limit && 最老等待 < 发送间隔
```

发送间隔默认 200ms（`coalesce_interval_ms_` 为 0 时用 200）。`batch_wait` 为真就先不发，等下一次。

默认 `enforce_budget_` 为假。`Pick` 在这之后把 `limit` 抬到所有「距上次写出不少于 200ms」的实体数，上限是 `max_n`（网关传 200）。第一次还没写出的实体，`last_written_ms` 为 0，间隔被当成 1000000ms，算进这个名额。槽里只有这一个人时，候选数等于抬高后的 `limit`，`候选数 < limit` 不成立，`batch_wait` 为假，这一帧立刻发出，里面只有 1 个事件。凑满 8 人的等待因此不会发生。

另外，上一帧写出之后 200ms 内 `Pick` 直接返回空。所以一条连接大约 200ms 才有一帧 MOVE，但这一帧里有几个人，取决于这一拍槽里有几个「到点」的人，不受「至少 8 个」约束。

实测：第一次压测 A 时刻，两台网关 tx 帧平均 payload 约 82 字节。这是整段窗口的字节除以帧数，含登录爬坡，不是本轮新测。本轮每帧事件数分布无数据，要等下面的分位指标部署后再采。

代码里的单事件夹具（`test/aoi_move_frame_size_test.cpp`，字段和文档里的移动快照一致，没有名字和血量）：1 个事件的整帧 54 字节，40 个事件 1498 字节，向上取整后约 38 字节/事件。`kAoiMoveEventBytes` 现在是 52，注释说名字和血量大约再加 14 字节。这是预算估算，不是线上抓到的帧长。82 字节更接近「大多是 1 个事件的整帧」，而不是 8 个事件打成一帧（8 个事件按 38 字节估算会到大约 300 字节以上）。这是用 82 的实测和 54/1498 的夹具对照，不是新的抓包。

## 3. TCP_NODELAY 和写路径

代码推断。

`tcp/TcpConnection.cpp` 的 `ConnectionEstablished` 对这个连接 `setsockopt(IPPROTO_TCP, TCP_NODELAY, 1)`。游戏 TCP 连接走这个类，所以 Nagle 是关的。

MOVE 帧由 `FlushMoveSlot` 编成一帧后调用 `TrySendLatestMove`。缓冲为空时 `SendInternal` 对这一帧做一次 `::write`。内核只收下一部分时，剩下的留在应用缓冲，之后 `WriteNonBlocking` 再写。不是「每个事件一次 write」；是「每个被交出的帧一次 write 尝试，没收完就再写剩余字节」。半帧不会被截断丢掉。

## 4. 新指标

只在 MOVE 帧被连接接受时记一笔（`TrySendLatestMove` 返回真，和 `AoiNoteTxBytes(Move)` 同一处）。不改变发不发。

| 名称 | 含义 | 单位 |
| --- | --- | --- |
| `gamemesh_move_frame_events{quantile}` | 一帧里的事件数。分位 0.5、0.9、0.99、1 | events |
| `gamemesh_move_frame_events_count` | 上面分位所用的样本数。环形缓冲最多 4096，满了丢掉最旧的 | frames |
| `gamemesh_move_frame_payload_bytes{quantile}` | 这一帧的 payload，含 4 字节长度头 | bytes |
| `gamemesh_move_frame_payload_bytes_count` | 同上，样本数 | frames |

没有样本时这四条都不输出，不是 0。

`gamemesh_tx_frames_by_frame{frame}` 原来就有，按帧种类计数，数值本身就是帧数，没有再加一条 `_count`。空的时候仍然输出，包括 `frame="move"` 的 0。这是已有计数器，0 表示还没接受过这种帧。

本轮没有新的压测，这四个分位的线上值无数据。

## 5. 估算

帧头是 `game/ProtoFraming.h` 的 4 字节大端长度。38 是上面夹具把 40 个事件摊薄后的每事件字节，不是 `kAoiMoveEventBytes`。66 是 `kOnWireHeaderBytes`。公式按任务书写：每事件摊销 = (帧头 + 38n + 66) / n。标「估算」。

| n | payload 估算 = 4 + 38n | 线上字节 = payload + 66 | 每事件摊销 |
| ---: | ---: | ---: | ---: |
| 1 | 42 | 108 | 108 |
| 2 | 80 | 146 | 73 |
| 4 | 156 | 222 | 55.5 |
| 8 | 308 | 374 | 46.75 |

夹具里 1 个事件的真实整帧是 54 字节，不是 42。差的 12 字节是 `GameResponse`（`message="aoi"`）和 `AoiDelta` 外壳，没有被 38 的摊销完全算进「帧头」。用夹具实测：1 个事件线上字节估算 54+66=120；40 个事件 1498+66=1564，每事件约 39.1。38n 那一列会把小帧估小。
