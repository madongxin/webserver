# AOI 序号和全量快照

`client_seq` 是这条连接上客户端请求的序号。`server_seq` 是可靠推送（进视野、离开视野）的序号。面板上 490 和 362 是两个计数器，不是丢了 128 个包。纯移动帧 `reliable=false`，`server_seq` 保持 0。

## 缺口从哪来

09:41 的逻辑在调用 Gateway 之前就执行 `AppendReliable`。Gateway 在 `FindBySession` 失败，或玩家、网关实例、fence、代际对不上时直接拒绝，不发送。回放里已经有这个序号，客户端下一次 `PushAck` 发现不连续，Gateway 把 `AckStatus::Gap` 和 `Stale` 都映射成 `ERR_AOI_RESYNC_REQUIRED`。

09:44–09:56 窗口里两台逻辑的 `gamemesh_push_rejected_total` 速率峰值大约是 40/秒 和 44/秒。这和玩家 819 的 `ERR_AOI_RESYNC_REQUIRED` 是同一条路径。移动超时不是这条错误码的来源。

本轮修改后，拒绝发生在分配序号之前。只有会话匹配通过、帧即将写入该连接发送缓冲时，Gateway 才分配序号并包进 `server_push`。逻辑侧不再预分配。两边要一起换；只换逻辑、Gateway 仍是旧二进制时，可靠帧会以序号 0 按原始业务帧发出。

断线竞态仍在：匹配成功并写入回放之后，发送前连接消失，仍会空一个序号。

## 全量快照为什么会变成「实体 0」

玩家在这台逻辑的地图上时，`CollectFullView` 已经会在邻域仍有在线玩家、兴趣列表却为空时返回 `view_inconsistent`，快照失败，不会给空列表。

另一条路径是请求落到了不拥有该玩家的 GameLogic。`BuildFullStateSnapshot` 的 `not_on_map` 分支原来填上恢复坐标后仍 `ok=true`，实体数为 0。客户端按成功的全量结果替换世界，视野里的人被删掉。09:44 的分线说明人分散在 7 个实例、两台逻辑上，快照打到非属主是可能的。

现在这条分支返回 `ok=false`、`ERR_AOI_RESYNC_REQUIRED`，并清空 `aoi_entities`。若 Redis 里还有该玩家的落点，响应里仍带 `map_instance_id` 和属主逻辑，供客户端改向属主再要一次。`ok` 保持 false。

`ok=true` 且实体为空只保留给「人在这张图上，邻域确实没有其他人」。带邻居的实例必须仍是 `INSTANCE_LIVE` 且实体数至少 1。`world_snapshot_test` 覆盖了这两种结果。
