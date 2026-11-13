# 回包构成与 MoveRsp 体积（D2）

标注：分类结论是**代码推断**；19:20 的 725 帧/秒 reply 是**实测**网关计数（当时 `frame="reply"` 含 MoveRsp）。心跳请求/回包拆分要等本轮指标部署后再采，当前线上拆分写**无数据**。

## 1. `frame="heartbeat"` 为什么恒为 0

**代码推断（已修指标分类）：** `TcpReplySink` 里 `KindOfResponse` 对 `has_move()` 的回包返回 `"move"`，对 `has_heartbeat()` 返回 `"heartbeat"`。写出时：

- `"heartbeat"` → `AoiNoteTxBytes(Heartbeat)`
- **其它（含 `"move"`）→ 全部进 `AoiNoteTxBytes(Reply)`**

因此 **MoveRsp 被记进 `reply`**，不是 AOI 的 `move` 推送。19:20 实测 `reply`≈725 帧/秒、`heartbeat`=0，与「几乎全是 MoveRsp、心跳回包极少或为 0」一致；不能据此说心跳被记成 reply（心跳分支本身是对的），而是 **MoveRsp 淹没了 reply 桶**。

本轮把 `"move"` 回包改记为 `frame="move_reply"`（只改指标，不改协议）。`reply` 留给登录/其它控制回包。

网关心跳在 `GameTcpGateway` 本连接 EventLoop 直接 `SubmitControlOnLoop(..., "heartbeat")`，成功时同样走 Heartbeat 计数；另增 `gamemesh_heartbeat_requests_total` 统计请求次数。

## 2. 725 帧/秒里各占多少（19:20）

| 成分 | 19:20 实测 | 说明 |
| --- | --- | --- |
| MoveRsp（原记在 reply） | **约 725 帧/秒**（推断占 reply 绝大部分） | 每机器人约 0.8/秒 |
| 心跳回包 | **0**（`frame="heartbeat"`） | 请求侧旧指标无数据 |
| 其它 reply | **无数据** | 需 `move_reply` 拆分后再采 |

部署 D2 后应用：`move_reply` + `heartbeat` + `reply` 三者相加对照总控制回包。

## 3. MoveRsp 字段字节（不改协议；代码推断）

夹具按当前 `game.MoveRsp` / `GameResponse` 常用字段估算（含 4 字节帧长头），**不是抓包**。完整 protobuf 编码随字段有无、名字长度变化。

| 字段 | 大约字节 | 不设可选能否少发 |
| --- | --- | --- |
| 帧长前缀 | 4 | 否 |
| `GameResponse.seq/ok/message` | ~6–12 | message 空可略减 |
| `MoveRsp.ok/error_code` | ~4–8 | 成功路径 error 可空 |
| `position` x/y/z | ~15 | 否（权威坐标） |
| `yaw` | ~5 | 可选，不设可省 |
| `state_seq` / 其它元数据 | ~4–10 | 视 proto 而定 |
| **合计（对照实测 payload≈77）** | **~70–85** | 不改协议、仅省略可选：粗估可到 **约 60–70 字节/帧**（需抓包确认） |

**不改协议、不设置可选字段就能少发**的候选：空 `message`、空 `error_code`、不设 `yaw`（若客户端不依赖）。**等确认后再改代码。**

## 4. 重复 client_seq / 同目标（只统计，等确认）

- `client_seq <= last` → 已有 `ERR_STALE_SEQ` / `stale_seq` 计数。
- 与权威点水平距离 &lt; 5cm 仍接受的请求 → 新计数 `duplicate_target`（及 `gamemesh_move_requests_total{result="duplicate_target"}`）。**本轮不改 AOI/状态推进**，只看重复率。
- 机器人 2 秒超时重发若带新 seq、同目标，会走 `ok`+`duplicate_target`，不会走 `stale_seq`。

建议：看 `duplicate_target / ok` 比例后再决定是否短路写 AOI。
