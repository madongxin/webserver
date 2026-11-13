# 外推是否能少发（D3，先证实，未做 C1）

开关 `GAMEMESH_AOI_EXTRAPOLATE` 默认关。本轮**不**把外推设为默认，**不**做 C1 修复。

## H1：误差用权威坐标，呈现坐标在滑行 → 每个 tick 都 drifted（证实）

`MapRuntime::AllowMoveAoi` 外推分支（约 1694–1726 行）：

- 预测：`pred = rec.x + rec.vx * elapsed`，其中 `rec.x/rec.z` 在上次允许发送时被赋成 **`subject.x/subject.z`（权威）**。
- 误差：`pred - subject.x/subject.z`（仍是权威）。
- `EmitTo` 发出的是呈现坐标 `aoi_x/aoi_z`。

速度 10 m/s、间隔 200ms、权威点未动而 `vx` 仍为 10（到点/稀疏段常见）时：`pred` 往前走 2m，与权威差 **200cm**，大于 `ExtrapErrorCm` 默认 **100cm** → `drifted=true` → 每个呈现 tick 都允许发送。  
**打开外推也不会按速度少发。** 由 `aoi_extrapolate_tick_test` 的 H1 算术用例固定。完整「近 8m/远 60m 走 20 秒计事件」留到 C1 修判定后再加。

## H2：旧外推用例只走 Move()（证实）

`test/map_runtime_aoi_test.cpp` 里与外推相关的断言（若有）只调用 `Move()`，**没有**在外推打开时对 `AdvancePresented` 计事件数。少发是否成立必须看呈现推进，不能只看 Move 接受。

## H3：ENTER / 快照是否带 vx（核实）

代码推断：`FillSnapshot` 只把 `MapEntity` 拷进列表；实体上若已有 `vx/vz/move_start_ms`，编码进 ENTER 时取决于 `EmitTo`/protobuf 填充是否写入这些可选字段。`AllowMoveAoi` 外推分支只更新 `move_notify_ms` 记录，**不保证** ENTER 帧带速度。C1 再显式带上；本轮不改。

## 计数器

`gamemesh_aoi_move_emit_reason_total{reason=first|vel_changed|drifted|keyframe|arrival_stop|interval}`  
在 `AllowMoveAoi` 决定发送时累加。用于复压时看 drifted 是否占绝大多数。

## C1（未做，等客户端就绪与 D1 复压）

- 误差与记录改为呈现坐标（或记录发出去的坐标）。
- ENTER 带 vx/vz/move_start_ms。
- 远层关键帧 `GAMEMESH_AOI_FAR_KEYFRAME_MS` 默认 10000。
- **确认客户端支持外推之前不要默认打开。**
