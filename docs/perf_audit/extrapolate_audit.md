# 外推是否能少发（D3 证实 → C1 修复）

开关 `GAMEMESH_AOI_EXTRAPOLATE` **默认仍关**。C1 已改判定口径；未确认客户端外推前不要默认打开。

## H1：旧口径（权威算误差）→ 每 tick drifted（已证实，C1 已修）

旧 `AllowMoveAoi` 外推分支：

- 预测：`pred = rec.x + rec.vx * elapsed`，`rec.x/z` 存的是 **权威** `subject.x/z`。
- 误差：再和 **权威** 比。
- `EmitTo` 发出的是 **呈现** `aoi_x/aoi_z`。

权威已到点、呈现仍滑行且 `vx≠0` 时：每个 present tick 都会超过 `ExtrapErrorCm`（100cm）→ 外推省不了流量。

**C1：** `rec` 与误差一律改用 `PresentedX/Z`；并区分 `arrival_stop`；远处关键帧 `GAMEMESH_AOI_FAR_KEYFRAME_MS` 默认 10000。  
单测：`aoi_extrapolate_tick_test`（legacy H1 签名保留 + C1 匀速 20s 预算）。

## H2：旧外推用例只走 Move()（仍成立）

少发是否成立必须看呈现推进 / emit_reason，不能只看 Move 接受。

## H3：ENTER / 快照带 vx（已具备，随开关）

`FillEntitySnapshot` / `FillMoveSnapshot` 在 `AoiExtrapolateEnabled()` 时写入 `vx/vz/move_start_ms`。  
`EmitTo` 在 `aoi_live` 时用呈现坐标覆盖发出的 `position`。

## 计数器

`gamemesh_aoi_move_emit_reason_total{reason=first|vel_changed|drifted|keyframe|arrival_stop|interval}`

灰度时期望：`interval`/`drifted` 降，`vel_changed`/`keyframe`/`arrival_stop` 成主导。

## 客户端

见 `p1_extrapolate_client_checklist_20261114.md`。确认支持前不默认打开、不单独改 schema 默认值。
