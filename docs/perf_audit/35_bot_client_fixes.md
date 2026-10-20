# 压测机器人 / 客户端必改（配合本轮服务器）

服务器已做：会话短 TTL 抬到默认、Move 拒绝回写权威坐标、1002 出生打散 4m、AOI≥10Hz 抽样、TooFast 钳位。以下仍须客户端改，否则无效 Move / Hello 空转会继续淹没服务器。

## 1. 移动

- 本地 `move_speed` ≤ 服务器属性（默认 **10**）；步进按 `speed * dt`，勿每帧猛冲。
- 收到 `MoveRsp`（含 `ok=false`）时**必须**采用回包 `position/yaw/state_seq/server_time_ms`，不要继续用本地飞远坐标重发。
- 路径贴 `IsWalkable`；出生打散后勿假设全员同一坐标。

## 2. 会话 TTL

- `LoginReq.ttl_sec` 发 **0**（用服务器默认 7200），或 ≥ 整场压测时长。
- 服务器现会把 `0 < ttl < default` 抬到 default；仍建议客户端不要再写 300。

## 3. Hold 期 Hello 空转 / 重连

- 已登录且 TCP 仍活：不要反复 `ClientHello` / 重连。
- 心跳失败：打日志并**重连+登录**，不要吞掉后空转 Hello。
- 断线：带 `session_id` + fence `token` + `generation` 走 Reconnect；宽限现为 **120s**。
- `reconnect failed` 后勿立刻再 Login 顶号（会 SESSION_REPLACED 踢旧连接）；退避后再完整 Login。
- 不要只 Hello 不 Login。

## 4. 协议 / 重连进图

- Hello `schema_sha256` = 当前服务器（见 `docs/protocol/export/protocol_manifest.json`）。
- `ReconnectRsp` 新增：`map_instance_id` / `need_enter_map` / `on_map`。若 `need_enter_map=true` 或 `on_map=false`，**必须再发 EnterMap**，不要直接 Move。
- 详见 `docs/perf_audit/36_stress_reconnect_ghost_fixes.md`。
