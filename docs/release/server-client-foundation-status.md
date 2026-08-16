# Server/Client Foundation Status

Date: 2026-08-16  
Audit baseline / current HEAD: `60542e51ed5f7e757fced13cb2a069c29739aa36`  
Unity audit baseline: `38a0042a62a1e3975a5315a7e742dbc5342102f4`

This document records **current repository facts**. Historical gate logs are labeled as such and must not be reused as evidence for a later commit or a dirty tree.

## 1. HEAD and worktree (this session)

```text
git rev-parse HEAD → 60542e51ed5f7e757fced13cb2a069c29739aa36
git log -1        → Ship the server foundation slice: protocol freeze, recovery, and world chat.
```

Worktree is **dirty** (S0 scripts/docs + S1 proto/C++/tests). No commit / tag / push unless the user authorizes it.

| 项 | 值 |
| --- | --- |
| 协议 namespace | `GameMesh.Protocol` |
| `proto/game.proto` SHA-256 | `f16462b65fa998a1c1d63be4710b2be927c9ec1b8ef47756803b12798d6e8665` |
| Unity `38a0042` schema SHA-256 | `aed5c952a1aa817a13464af8ae05c14d14c19da0ceedd6b61663d2b39f255bcb` |
| 已发布兼容基线 | `docs/protocol/published/v1/game.desc`（禁止覆盖） |
| 地图模板 | 1001 |

**Unity `38a0042` is incompatible with this dirty tree.** Formal mode requires ClientHello; luna still tracks an older hash. Do not hide this with `GAMEMESH_ALLOW_LEGACY_NO_HELLO=1`. After S1 appends, Unity must import **`f16462b6…`**, not the audit-time `4c29a73…`.

## 2. Verdicts (do not mix)

| Verdict | Meaning | Current |
| --- | --- | --- |
| DEV PASS | unit + integration on this tree | **PASS** on this dirty tree (2026-08-16); not a clean-HEAD `stable_gate.sh` |
| CLIENT READY PASS | C++ TCP `client_ready_gate.sh` | **NOT RUN this closeout**; gate now requires luna hash match and will **FAIL** until Unity imports `f16462b6…` |
| UNITY E2E PASS | real Unity two-client gate | **FAIL / NOT RUN** — protocol hash mismatch; `test_unity_two_clients.sh` is S2 |
| STABLE CANDIDATE PASS | `stable_gate.sh --full` on a **clean** tree | **NOT RUN / STABLE BLOCKED** |

`FOUNDATION STABLE PASS` is **not** claimed.

## 3. Historical evidence (not this-session `--full`)

These results were recorded when shipping `60542e5`. They are **not** a clean-tree `--full` report and **not** a Unity-matched protocol gate.

| Command | Exit | Note |
| --- | ---: | --- |
| `GAMEMESH_JOBS=2 ./scripts/build.sh Debug` | 0 | historical, pre-push dirty tree that became `60542e5` |
| `./scripts/client_ready_gate.sh` | 0 `CLIENT READY PASS` | C++ TCP only; Unity repo was not hash-checked |
| `./scripts/stable_gate.sh --full` | **NOT RUN** | no 30min load / 2h soak on clean `60542e5` |

Older `--full` artifacts under `run/release/` belong to **earlier commits**.

## 4. S0 closeout

Goal: freeze the server contract and correct published facts. Unity hash did not match; the user then authorized continuing S1 on the server.

| Path | Purpose |
| --- | --- |
| `scripts/export_unity_protocol.sh` | Manifest records `server_commit`, `protoc_version`, `required_types`; refuses to overwrite `docs/protocol/published/v1` |
| `scripts/check_luna_protocol_contract.sh` | Calls luna `Tools/GameMesh/check_protocol_contract.sh`; prints both commits and schema hashes |
| `scripts/test_unity_contract.sh` | Accepts `LUNA_REPO` / path argument; runs the Unity checker **before** TCP cluster work |
| `scripts/test_unit.sh` | Manifest field check; luna contract `NOT RUN` when `LUNA_REPO` unset |
| `scripts/client_ready_gate.sh` | `GAMEMESH_REQUIRE_LUNA_CONTRACT=1`; missing/mismatched Unity fails the gate |

S0 protocol export did **not** change `proto/game.proto` field numbers. S1 later **appended** public fields (see §5).

### S0 commands

| Command | Exit |
| --- | ---: |
| `bash -n scripts/*.sh` | 0 |
| `./scripts/check_deps.sh --full` | 0 |
| `LUNA_REPO=/tmp/luna-audit ./scripts/test_unity_contract.sh` | **1 FAIL** — Unity `38a0042` `aed5c952…` ≠ then-server `4c29a73…` |

**S0 vs Unity: FAIL.** Server-side S0 scripts/docs are in the dirty tree. S1 proceeded by user request.

## 5. S1 closeout (duplicate login + hello manifest)

### Call chain (after Bind success)

```text
新 Gateway 登录并认证
→ Session Lua 原子 Acquire（返回 previous_gateway/session/generation，不 TCP 踢人）
→ GameLogic BindPlayer
→ Bind 成功：Gateway → Session.NotifySessionReplaced → KickConnectionAsync(旧 gw)
→ 旧 Gateway KickConnection：EventLoop 发送 SessionReplacedNotify → grace → 关 TCP
→ Bind 失败：RestorePreviousSession（CAS 回旧 fence）或 CompensateLogout；不踢旧连接
```

Session 进程现在写入并加载 `gateway_push_addrs`（与 world/gamedb/logic 相同），否则 `KickConnectionAsync` 会 `no channel`。

### Public proto append-only (`proto/game.proto`)

| Type / field | Number |
| --- | ---: |
| `MapManifestEntry.map_template_id` | 1 |
| `MapManifestEntry.data_version` | 2 |
| `MapManifestEntry.sha256` | 3 |
| `ServerHelloRsp.gameplay_config_version` | 12 |
| `ServerHelloRsp.map_manifest_version` | 13 |
| `ServerHelloRsp.maps` | 14 |
| `SessionReplacedNotify.reason_code` | 1 |
| `SessionReplacedNotify.server_time_ms` | 2 |
| `SessionReplacedNotify.message` | 3 |
| `GameResponse.session_replaced` | 76 |

Internal `proto/session.proto`: `AcquireSessionResponse` previous_* fields 14–24; RPCs `RestorePreviousSession`, `NotifySessionReplaced`. Published v1 descriptor was **not** overwritten.

### S1 commands (this dirty tree, 2026-08-16)

| Command | Exit |
| --- | ---: |
| incremental `cmake --build` gateway/session/e2e client | 0 |
| `./scripts/test.sh unit` | 0 (`gateway_conn_race_test` includes queue_on_loop) |
| `./scripts/test.sh integration` | 0 (`RestorePrevious` `RESTORED prev_gw=gw-8083`) |
| `./scripts/test_hello_heartbeat.sh` | 0 (`map_manifest_version=1` `hello_maps_n=1` schema `f16462b6…`) |
| `./scripts/test_world_snapshot.sh` | 0 |
| `./scripts/test_two_player_aoi.sh` | 0 |
| `./scripts/test_map_capacity.sh` | 0 |
| `./scripts/test_player_mail_e2e.sh` | 0 |
| `./scripts/test_duplicate_login_e2e.sh` | 0 (`old_got_session_replaced=1` `replaced_reason=SESSION_REPLACED`) |

**S1 C++ TCP: PASS.** Cross-gw duplicate login notifies then closes the old connection; new fence works; old fence is rejected.

## 6. Remaining (S2 / known limits)

S2 is **blocked** on Unity importing current `game.proto` / hash `f16462b6…`.

- Real Unity two-client E2E (`LUNA_REPO` + `UNITY_CLIENT_BIN` + `test_unity_two_clients.sh`)
- Clean-tree `stable_gate.sh` / `--with-e2e` / `--full` (20×, 30min load, 2h soak)
- `FriendList` remains `NOT_IMPLEMENTED`. World chat is unreliable (`reliable=false`).

## 7. Migration / rollback

`./scripts/migrate_db.sh` applies `0001_player_profile_up.sql` and `0002_last_safe_position_up.sql`. Rollback last: `./scripts/rollback_db.sh`. Published v1 descriptor is unchanged.

## 8. How to re-verify after Unity imports `f16462b6…`

```bash
./scripts/export_unity_protocol.sh /tmp/gamemesh-proto-export
LUNA_REPO=/path/to/luna ./scripts/check_luna_protocol_contract.sh
LUNA_REPO=/path/to/luna ./scripts/test_unity_contract.sh
LUNA_REPO=/path/to/luna ./scripts/client_ready_gate.sh
```

Hashes and commits in the checker output must match before any UNITY E2E or STABLE CANDIDATE claim.
