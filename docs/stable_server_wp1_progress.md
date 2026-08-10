# 工作包一进度（稳定服务器）

对照：`docs/GameMesh_Cursor_Stable_Server_And_Unity_Preparation.md`
基线：`57e9393` + 本工作区未提交改动；**未宣称稳定版本通过**（Release/ASan/20×E2E/soak 未全跑前保持 BLOCKED）。

| 章节 | 内容 | 状态 |
|------|------|------|
| §2 | TrustedPlayerId 穷举 + mismatch 拒绝 + WorldForward meta + `trusted_player_id_test` | **已做** |
| §3 | Bind/Freeze/Export/Import/Unbind 入 `PlayerSerialQueue`；Freeze 屏障；Unbind 过载不 fallback | **已做** |
| §4 | Formal 权威 Placement：READY/Owner/epoch/route_version/lease；禁止 Logic 隐式 ResolveOrCreate；`placement_authority_test`；EnterMap 仍可 Claim | **已做** |
| §5 | FullSnapshot 发送条件修复；Replay/Snapshot 包 `ServerPushEnvelope` | **已做**（严格双 GW E2E 已部分覆盖 sc3） |
| §6 | Formal Bind fail-closed；稳定幂等键；Outbox insert 失败回滚 | **已做** |
| §7 | Redis Registry 索引；Session `logic_ids`/`PlacementOwners` 5s 刷新；`gateway_push` Redis 注册 + Logic 热更新 | **已做** |
| §8 | `bootstrap_local_config.sh` / `final_e2e.sh` / `stable_gate.sh`；E2E 缺工具硬失败 | **部分**（完整 brpc CI 镜像/Sanitizer 矩阵仍待） |

### 门禁证据（本轮）

| 命令 | 结果 |
|------|------|
| `./scripts/build.sh Debug` | PASS |
| `./scripts/test.sh unit` | PASS（含 `placement_authority_test`） |
| `./scripts/test.sh integration` | PASS（MySQL 不可达时 `phase3_gamedb_asset_test` 仍 SKIP） |
| `./scripts/final_e2e.sh --start-cluster` | PASS（sc1–6,8,9；`pass=8 fail=0`） |
| `./scripts/stable_gate.sh --with-e2e` | 曾因残留 `gw_claim` 导致 GW 起不来；清 claim 后单独 final_e2e PASS |
| `./scripts/build.sh Release` / ASan / 20 轮 soak | **未跑** → 稳定判定 **BLOCKED** |

建议版本号（勿 tag）：`server-stable-v0.1.0-rc1`（门禁全绿后再议）。

工作包二（Unity）**未开始**（须工作包一门禁通过后用户确认）。
