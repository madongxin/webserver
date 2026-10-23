# 客户端协议交接

外网 `game.proto` 没有改。当前 Unity 的 Hello `schema_sha256` 继续用 `edfd9fb92b1961dac7451d0b78be4e12ef9ee1424d9716d7e02dbcd5d237c6ae`。内部的 `gateway_push.proto` 加了拒绝原因和逐条结果，客户端看不到。

## 可靠 AOI

`ServerPush.ServerSeq` 仍是 Gateway 在会话、fence、generation 都对上之后分配的。序号写进回放后不会因为发送失败而收回。

- `queued=true`：帧进了 Gateway 发送缓冲或 200ms 移动合并槽。不是内核写完，也不是客户端已经应用。
- `replay_pending`：序号已经在 `PushReplayStore` 里，这一帧没有进发送缓冲。客户端按原来的缺口逻辑补放；补不上再要 FullSnapshot。
- FullSnapshot 在 `ok=false` / `ERR_AOI_RESYNC_REQUIRED` 时不能当成已经恢复。
- ENTER / LEAVE 仍是可靠帧。MOVE 仍是 `server_seq=0` 的可合并帧。
- 断线玩家自己会暂时收不到 AOI，直到重新 `BindPlayer`。这不是 Logout。还在线上的邻居仍收到 LEAVE。

客户端不要把 `GameResponse.seq` 当成 `last_server_seq`。可靠序号只看 `ServerPush.ServerSeq`。

## 自身坐标

没有现成的轻量只读自身坐标接口。

| 已有消息 | 有没有自身坐标 |
| --- | --- |
| `MoveRsp` | 有。权威坐标、yaw、`state_seq`、`server_time_ms`。`ok=true` 表示这次移动已提交，不表示 AOI 已送达。 |
| `EnterMapRsp.self` | 有。只在进图时。 |
| `GetSelfProfileRsp` | 没有坐标，只有属性。 |
| `MapPingRsp` | 没有坐标。只有 `owner_epoch`、人数、logic 实例。 |
| `WorldSnapshot` / `FullStateSnapshotRsp` | 有自身和整片视野。贵，不能拿来给每个机器人对坐标。 |

机器人在 Move 结果不确定时，用上一次 `ok=true` 的 `MoveRsp` 做自身位置。只有可靠序号缺口才走 FullSnapshot。

`SelfPoseReq` / `SelfPoseRsp` 这次没有加进 `game.proto`。加上去会改 Hello 的 schema，当前 Unity 会被 `ERR_SCHEMA_MISMATCH` 拒掉。等客户端一起改时再用空闲 oneof 号 **95**（请求和响应目前都没用 95）：

```protobuf
message SelfPoseReq {
  uint64 player_id = 1;
  uint64 map_instance_id = 2;
}
message SelfPoseRsp {
  bool ok = 1;
  string message = 2;
  string error_code = 3;
  uint64 map_instance_id = 4;
  uint64 owner_epoch = 5;
  Vec3 position = 6;
  float yaw = 7;
  uint64 state_seq = 8;
  int64 server_time_ms = 9;
}
```

约定：只查自己的 `MapRuntime` 坐标，校验当前 session、fence 和地图归属；不扫 AOI，不改地图，不读数据库；每个玩家最多约 1 次/秒。在那之前服务端不会发这个字段，客户端也不要改 Hello。

## 漏斗

服务端能对上的只有：Gateway 收到的帧、生成的回复、进入发送缓冲的回复、PushBatch 耗时的总和和次数、按 reason 的拒绝数。

对不上、不要编：

- 内核写完
- 回包年龄 p95 / p99
- 客户端的 enqueue / dequeue / write_complete / raw_rsp / matched

单台 Gateway 收到大约 80–100 帧/s，只说明这次客户端写了这么多，不是服务器上限。13:47–13:57 里服务器把收到的 Move 在几十毫秒内做完了；机器人走得慢，是客户端没把计划中的移动写出去。

状态：协议兼容 **已保持**。`SelfPose` **设计未接入**。605 复测 **未跑**。
