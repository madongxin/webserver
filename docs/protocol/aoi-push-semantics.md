# AOI / 可靠 Push 顺序语义

Gateway 下发的 `ServerPushEnvelope.server_seq` 对每个 session 单调递增。客户端发现缺口时：

1. 暂停应用依赖连续性的增量（Enter/Leave）。
2. 请求 `WorldSnapshotReq`（回包 `GameResponse.full_snapshot`）。
3. 用快照原子替换本地 AOI 与自身位置。
4. 记录 `baseline_server_seq`，只应用 `server_seq > baseline` 的后续 Push。

## 可靠性

| 事件 | reliable | coalescable | 缺口时 |
| --- | --- | --- | --- |
| AOI Enter / Leave | 是 | 否 | 必须回放或走快照 |
| AOI Move | 否（可合并） | 是 | 可用快照覆盖 |
| EnterMap 通知 / 全量快照 | 是 | 否 | 快照 `baseline_server_seq` 对齐 ReplayStore |

过旧 `PushAck` 或 Replay 无法回放时，Gateway 返回顶层 `ERR_AOI_RESYNC_REQUIRED`（可重试：先拉世界快照）。不允许只回 `need_full_snapshot=true` 却没有 `WorldSnapshotReq` / 重连快照路径。

`gamelogic_instance_id` 仅诊断，客户端不得用它选路。
