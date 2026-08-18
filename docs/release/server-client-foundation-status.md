# Server/Client Foundation Status

Date: 2026-08-17
Audit prompt: `docs/GameMesh_Server_Cursor_Login_AOI_Move_Prompt.md`
Server audit / current HEAD: `17912f2033344ee579fa388ba8f7467e1790f772`
Unity audit baseline: `2e43ed53267887614a358116d42b0de9c19b6822`
Unity current main: `f830c097347da7b973809c64486e2b6a25c8c261`

This document records **this worktree**. Historical gate logs (including `60542e5`) are not evidence for the current tree. Do **not** treat this file as “基础版服务器完成”, CLIENT READY, or FOUNDATION STABLE.

## 1. HEAD and worktree

```text
git rev-parse HEAD → 17912f2033344ee579fa388ba8f7467e1790f772
git log -1        → Close foundation S0/S1: protocol contract gates, duplicate-login notify, and hello map manifest.
```

Worktree is **dirty** (authoritative Logout + AOI/move assertions + Unity two-client gate). No commit / tag / push unless the user authorizes it.

| 项 | 值 |
| --- | --- |
| 协议 namespace | `GameMesh.Protocol` |
| `proto/game.proto` SHA-256 | `f16462b65fa998a1c1d63be4710b2be927c9ec1b8ef47756803b12798d6e8665` |
| descriptor SHA-256 (export) | `078461f2c0bfa23c3d806b51dff1734be06777a65e332fe772cf4aa223c4aefb` |
| Unity `f830c09` schema SHA-256 | `f16462b65fa998a1c1d63be4710b2be927c9ec1b8ef47756803b12798d6e8665`（与服务器一致） |
| Unity manifest `source_commit` | `17912f2033344ee579fa388ba8f7467e1790f772` |
| 已发布兼容基线 | `docs/protocol/published/v1/game.desc`（禁止覆盖；compat fields=415） |
| 地图 JSON SHA-256 | `ceef56586c5281dca4ce45340f511d0d577fd724b14131ae5a21d01ea7f41317`（两端一致） |
| 地图模板 | 1001 |
| `LUNA_REPO` | `/root/projects/luna`（协议/脚本稀疏检出；github.com git clone 超时） |
| `UNITY_CLIENT_BIN` | **缺失**（luna Actions artifacts=0，本机无 `GameMeshClient`） |

审计时的 Unity `2e43ed5` / schema `4c29a73…` **已被 luna `f830c09` 取代**。`f830c09` 已导入服务器 `17912f2` proto。不要用 `GAMEMESH_ALLOW_LEGACY_NO_HELLO=1`。

## 2. Verdicts (do not mix)

| Verdict | Meaning | Current |
| --- | --- | --- |
| Login (C++ TCP) | two accounts, same non-zero map instance | **PASS** (`test_two_player_aoi.sh`, 2026-08-17) |
| Logout (C++ TCP) | Rsp ok, Session deleted, AOI Leave, no DISCONNECTED resurrection | **PASS** (same script + `session_store_test`) |
| AOI visibility (C++ TCP) | mutual Enter/snapshot with player_id/name/state_seq/map | **PASS** |
| Move sync (C++ TCP) | bidirectional coords + monotonic `state_seq` | **PASS** |
| Protocol vs Unity | byte-identical `game.proto` + matching manifest | **PASS** (`check_luna_protocol_contract.sh`, luna `f830c09`) |
| Unity two-client E2E | real binaries, Hello/login/AOI/move/logout | **BLOCKED**（无 `GameMeshClient` 二进制） |
| CLIENT READY | `client_ready_gate.sh` with luna hash match | **NOT RUN**（协议已对齐，缺 Unity 二进制） |
| STABLE CANDIDATE | clean-tree `stable_gate.sh --full` | **NOT RUN** |

`FOUNDATION STABLE PASS` is **not** claimed. “基础版服务器完成” is **not** claimed.

## 3. S0 — protocol freeze

`proto/game.proto` was not changed this round. Export required types now include `MapManifestEntry` and `SessionReplacedNotify`. Published v1 descriptor was not overwritten.

| Command | Exit |
| --- | ---: |
| `bash -n scripts/*.sh` | 0 |
| `./scripts/check_public_protocol.sh` | 0 schema `f16462b6…` fields=415 |
| `./scripts/check_deps.sh --full` | 0 |
| `GAMEMESH_JOBS=2 ./scripts/build.sh Debug` | 0 |
| `LUNA_REPO=/root/projects/luna ./scripts/check_luna_protocol_contract.sh` | 0 `luna_protocol_contract=PASS` |
| `LUNA_REPO=/root/projects/luna ./scripts/test_unity_contract.sh` | 0 |

**S0 vs Unity proto: PASS** on luna `f830c09` vs server `17912f2`. Real Unity process E2E remains BLOCKED without a client binary.

## 4. S1 — authoritative Logout

Call chain for a **bound** TCP connection:

```text
client Logout
→ ignore client-reported player/session/fence; use Gateway bind
→ GameLogic.UnbindPlayer (RPC + ursp.ok)；失败则 LogoutRsp.ok=false，保留 bind
→ Session.LogoutV2 / legacy Logout；失败则 LogoutRsp.ok=false，保留 bind
→ 仅 session_ok && logic_ok 时 LogoutRsp.ok=true 并 MarkAuthoritativeLogout（ForgetBind）
→ 该 TCP 断线回调跳过 MarkDisconnected / Unbind，避免把已删除 Session 写成 DISCONNECTED
```

Repeat Logout on the same connection is idempotent (`already offline`). Unbind of an already-unbound player is success and still `LeaveAll`. Session Lua `ALREADY_OFFLINE` is success. After Session DEL, `MarkDisconnected` returns `NOT_FOUND` and Reconnect fails.

Logs record player / logic / map only (no token/fence).

## 5. S2 — AOI / move (no rewrite)

C++ `two-player-aoi` (two Gateways, map 1001) this session:

| Key | Value |
| --- | --- |
| `a_player_id` / `b_player_id` | 143038 / 143039 |
| `same_instance` | 1 (`map_instance_id=64741`) |
| mutual Enter | `player_id`/`name=e2e`/`state_seq=1`/`map_instance_id` |
| A→B and B→A move | `state_seq=2`, coords match |
| `logout_ok` / `logout_idempotent` | 1 |
| `logout_stale_move_rejected` | 1 |
| `b_aoi_move_after_a_logout` | 0 |
| `b_aoi_leave_on_logout` | 1 |
| `session_released` | 1 (`reconnect_after_logout=0`) |
| `b_logout_ok` | 1 |

Protocol `EntitySnapshot` has no separate `entity_id`; tests print `entity_id=player_id`.

## 6. S3 — Unity two-client gate / CI

- Added `scripts/test_unity_two_clients.sh`: requires `LUNA_REPO` + `UNITY_CLIENT_BIN`; protocol check first; missing anything → BLOCKED, not PASS.
- `client_ready_gate.sh`: `GAMEMESH_CI_TCP_ONLY=1` skips luna and prints `CLIENT TCP PASS` (not CLIENT READY).
- `.github/workflows/ci.yml` full job sets `GAMEMESH_CI_TCP_ONLY=1`.
- `.github/workflows/unity-contract.yml` (`workflow_dispatch`): explicit Luna checkout; missing luna/artifact → fail BLOCKED; does not invent a Unity binary in a source-only job.

This continuation (luna `https://github.com/madongxin/luna`, sparse checkout `f830c09`): protocol contract **PASS**; C++ `test_unity_contract.sh` **PASS**; `./scripts/test_unity_two_clients.sh` → **BLOCKED**（luna Releases/Actions artifacts 均为空，本机无 `GameMeshClient`）。

## 7. Tests this session (dirty tree on `17912f2`)

| Command | Exit | Note |
| --- | ---: | --- |
| `bash -n scripts/*.sh` | 0 | |
| `./scripts/check_public_protocol.sh` | 0 | schema `f16462b6…`, dirty=true |
| `./scripts/check_deps.sh --full` | 0 | |
| `GAMEMESH_JOBS=2 ./scripts/build.sh Debug` | 0 | |
| `./scripts/test.sh unit` | 0 | includes `gateway_logout_policy_test` |
| `./scripts/test_reactor.sh` | 0 | |
| `./scripts/test.sh integration` | 0 | Logout + MarkDisconnected `NOT_FOUND` |
| `./scripts/test_hello_heartbeat.sh` | 0 | schema `f16462b6…` |
| `./scripts/test_two_player_aoi.sh` | 0 | see §5 |
| `./scripts/test_map_capacity.sh` | 0 | |
| `LUNA_REPO=/root/projects/luna ./scripts/check_luna_protocol_contract.sh` | 0 | schema+manifest+`source_commit=17912f2` |
| `LUNA_REPO=/root/projects/luna ./scripts/test_unity_contract.sh` | 0 | contract + TCP Hello/login/map hash |
| `LUNA_REPO=/root/projects/luna ./scripts/test_unity_two_clients.sh` | **1 BLOCKED** | 无 `GameMeshClient` 二进制 |
| `git diff --check` | 0 | |

`stable_gate.sh --full`, sanitizers, and real Unity E2E were **not** run this round.

## 8. Client handoff

Luna **main `f830c09` already imported** server `17912f2` proto (schema `f16462b6…`, descriptor `078461f2…`). This server dirty tree did not change `game.proto`.

Still needed for Unity process E2E:

```bash
LUNA_REPO=/root/projects/luna \
UNITY_CLIENT_BIN=/path/to/GameMeshClient.x86_64 \
./scripts/test_unity_two_clients.sh
```

Do not use `GAMEMESH_ALLOW_LEGACY_NO_HELLO=1`. This machine has no Unity Editor / StandaloneLinux64 build; GitHub Actions artifacts for luna were empty.
