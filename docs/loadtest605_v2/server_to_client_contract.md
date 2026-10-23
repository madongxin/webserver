# 给客户端的约定

协议字段没有变。`schema_sha256=edfd9fb92b1961dac7451d0b78be4e12ef9ee1424d9716d7e02dbcd5d237c6ae`。移动包不要加 `run_id`。

## 序号

`client_seq` 与 `server_seq` 各计各的。两者的差不是丢包数。只有可靠推送（进视野、离开视野）占用 `server_seq`。移动 AOI 的序号是 0。

`PushAck` 得到 `ERR_AOI_RESYNC_REQUIRED` 表示可靠流不连续或确认落在了已确认序号后面。处理是向当前属主再要全量快照，不是把 `client_seq` 回退。

## 全量快照

只在 `ok=true` 且 `recovery_reason=INSTANCE_LIVE` 时，用 `aoi_entities` 替换周围的人。

`ok=false` 时不要清空当前视野，即使 `aoi_entities` 为空。空列表加成功，只在属主确认邻域确实没有其他人时出现。不在这台逻辑上的玩家，服务端现在返回 `ERR_AOI_RESYNC_REQUIRED`。响应里若带了 `map_instance_id` 和 `gamelogic_instance_id`，下一次快照应发到那个属主，而不是把空结果应用掉。

## 移动和心跳

`MoveRsp.ok=true` 表示权威坐标已提交，不表示 AOI 已送达。`ERR_OVERLOADED` 是服务器忙，带有匹配的移动回包和序号，不是坐标校验失败。校验拒绝仍是 `ERR_OUT_OF_BOUNDS`、`ERR_MAP_MISMATCH` 这一类，09:44 的面板拒绝数 0 与此相符。

2 秒移动超时和 12 秒心跳超时仍由客户端判定。服务端没有加长这些等待，也没有关认证。超时后丢弃更晚到达的同一序号即可，不要把「忙碌过」记成服务器拒绝。

09:41 正在跑的进程还没有「非属主快照失败」和「拒绝推送不占序号」这两处。磁盘上的新二进制要等下一次重启才生效。
