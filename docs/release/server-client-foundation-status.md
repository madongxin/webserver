# Server/Client Foundation Status

Date: 2026-09-16
Audit prompt: `docs/GameMesh_Server_Cursor_Login_AOI_Move_Prompt.md`
Server current HEAD: `c8550639c75b97abce6ed137a6354a7e151a8a45`
Unity sparse checkout: `/root/project/projects/luna` @ `f830c097347da7b973809c64486e2b6a25c8c261`

This document records **this worktree**. Historical gate logs (including `60542e5` / `17912f2`) are not evidence for the current tree. Do **not** treat this file as “基础版服务器完成”, CLIENT READY, or FOUNDATION STABLE.

## 1. HEAD and worktree

```text
git rev-parse HEAD → c8550639c75b97abce6ed137a6354a7e151a8a45
git log -1        → Fix login-enter session miss and leftover LINE layout...
```

Worktree is **dirty** (CentOS 7 `git -C` protocol export, E2E duplicate sess-1, Hello maps_n=4, 1002 soft_cap test). No commit / tag / push unless the user authorizes it.

| 项 | 值 |
| --- | --- |
| 协议 namespace | `GameMesh.Protocol` |
| `proto/game.proto` SHA-256 | `e57e2c4afad4d5434ae398508da1ccb9c455078da9bca26a3b570b3f06695f6f` |
| descriptor SHA-256 (export) | `f37faa1f9e05472c500a366a1ac298fa426e513ba11ae9a0d6060880a9ff602e` |
| 已发布兼容基线 | `docs/protocol/published/v1/game.desc`（未覆盖；compat fields=415） |
| 地图 1001 SHA-256 | `ceef56586c5281dca4ce45340f511d0d577fd724b14131ae5a21d01ea7f41317` |
| Luna schema SHA-256 | `f16462b65fa998a1c1d63be4710b2be927c9ec1b8ef47756803b12798d6e8665` |
| Luna manifest `source_commit` | `17912f2033344ee579fa388ba8f7467e1790f772` |
| `LUNA_REPO` | `/root/project/projects/luna`（稀疏检出） |
| `UNITY_CLIENT_BIN` | **缺失** |

精确差异（S0 停止条件）：

```text
server schema = e57e2c4afad4d5434ae398508da1ccb9c455078da9bca26a3b570b3f06695f6f  (HEAD c855063)
client schema = f16462b65fa998a1c1d63be4710b2be927c9ec1b8ef47756803b12798d6e8665  (luna f830c09 / source_commit 17912f2)
```

Unity 正式 Hello 会收到 `ERR_SCHEMA_MISMATCH`。不要用 `GAMEMESH_ALLOW_LEGACY_NO_HELLO=1`。

## 2. Verdicts (do not mix)

| Verdict | Meaning | Current |
| --- | --- | --- |
| Login (C++ TCP) | two accounts, same non-zero map 1001 instance | **PASS** (`test_two_player_aoi.sh`, 2026-09-16) |
| Logout (C++ TCP) | Rsp ok, Session deleted, AOI Leave, no DISCONNECTED resurrection | **PASS** (same script + `gateway_logout_policy_test`) |
| AOI visibility (C++ TCP) | mutual Enter with player_id/name/state_seq/map | **PASS** |
| Move sync (C++ TCP) | bidirectional coords + monotonic `state_seq` | **PASS** |
| Protocol vs Unity | byte-identical `game.proto` + matching manifest | **FAIL**（见上表 hash 差） |
| Unity two-client E2E | real binaries, Hello/login/AOI/move/logout | **BLOCKED**（无 `GameMeshClient` 二进制） |
| CLIENT READY | `client_ready_gate.sh` with luna hash match | **NOT RUN**（协议未对齐） |
| STABLE CANDIDATE | clean-tree `stable_gate.sh --full` | **NOT RUN** |

`FOUNDATION STABLE PASS` is **not** claimed. “基础版服务器完成” is **not** claimed.

## 3. S0 — protocol freeze

`proto/game.proto` was not changed this round. Export required types include `MapManifestEntry`, `SessionReplacedNotify`, `SwitchLineReq`, `MapLineInfo`. Published v1 descriptor was not overwritten.

CentOS 7 git 1.8 无 `git -C`：`export_unity_protocol.sh` / `check_public_protocol.sh` / `check_luna_protocol_contract.sh` 改为 `--git-dir/--work-tree` 或 `cwd=`，否则 manifest `server_commit=unknown`。

| Command | Exit |
| --- | ---: |
| `bash -n scripts/*.sh` | 0 |
| `./scripts/check_public_protocol.sh` | 0 schema `e57e2c4a…` fields=415 |
| `./scripts/export_unity_protocol.sh /tmp/gamemesh-protocol-c855063` | 0 commit=`c855063` |
| `LUNA_REPO=/root/project/projects/luna ./scripts/check_luna_protocol_contract.sh` | **1 FAIL** hash mismatch |
| `./scripts/check_deps.sh --full` | 0 |

## 4. S1 — authoritative Logout

Call chain for a **bound** TCP connection is unchanged and still matches the prompt:

```text
client Logout
→ ignore client-reported player/session/fence; use Gateway bind
→ GameLogic.UnbindPlayer (RPC + ursp.ok)；失败则 LogoutRsp.ok=false，保留 bind
→ Session.LogoutV2 / legacy Logout；失败则 LogoutRsp.ok=false，保留 bind
→ 仅 session_ok && logic_ok 时 LogoutRsp.ok=true 并 MarkAuthoritativeLogout（ForgetBind）
→ 该 TCP 断线回调跳过 MarkDisconnected / Unbind
```

Repeat Logout on the same connection is idempotent (`already offline`). `gateway_logout_policy_test` 覆盖：Session 失败 / Unbind 失败均不得 `clear_bind`。

## 5. S2 — AOI / move (no rewrite)

C++ `two-player-aoi` (two Gateways, map 1001 LEGACY_POOL) this session:

| Key | Value |
| --- | --- |
| `a_player_id` / `b_player_id` | 8040 / 8041 |
| `same_instance` | 1 (`map_instance_id=71`) |
| mutual Enter | `player_id`/`name=e2e`/`state_seq=1`/`map_instance_id` |
| A→B and B→A move | `state_seq=2`, coords match |
| `logout_ok` / `logout_idempotent` | 1 |
| `logout_stale_move_rejected` | 1 |
| `b_aoi_move_after_a_logout` | 0 |
| `b_aoi_leave_on_logout` | 1 |
| `session_released` | 1 (`reconnect_after_logout=0`) |
| `b_logout_ok` | 1 |

`run_e2e_cluster.sh` 曾在 `start_formal` 已拉起 sess-1 后再绑一次 19096，端口占用导致 `e2e_cluster_healthy` 失败。已去掉重复启动。

## 6. S3 — Unity two-client gate / CI

- `scripts/test_unity_two_clients.sh`：要求 `LUNA_REPO` + `UNITY_CLIENT_BIN`；缺则 BLOCKED。
- `.github/workflows/ci.yml` full job：`GAMEMESH_CI_TCP_ONLY=1`，不宣称 CLIENT READY；Luna 在 `unity-contract.yml`。
- `client_ready_gate.sh` TCP-only 不再把 Luna hash 失败当成 TCP 失败。

本轮 `./scripts/test_unity_two_clients.sh` → **BLOCKED**（无 `GameMeshClient`）。

## 7. Tests this session (dirty tree on `c855063`)

| Command | Exit | Note |
| --- | ---: | --- |
| `bash -n scripts/*.sh` | 0 | |
| `./scripts/check_public_protocol.sh` | 0 | schema `e57e2c4a…` |
| `./scripts/check_deps.sh --full` | 0 | brpc=1 |
| `./scripts/test.sh unit` | 0 | 含 `gateway_logout_policy_test`；1002 软顶断言改为 100 |
| `./scripts/test_reactor.sh` | 0 | |
| `./scripts/test.sh integration` | 0 | |
| `./scripts/test_hello_heartbeat.sh` | 0 | `hello_maps_n=4`；坏 schema=`ERR_SCHEMA_MISMATCH` |
| `./scripts/test_two_player_aoi.sh` | 0 | see §5 |
| `./scripts/test_map_capacity.sh` | 0 | |
| `LUNA_REPO=… ./scripts/check_luna_protocol_contract.sh` | **1** | hash mismatch |
| `LUNA_REPO=… ./scripts/test_unity_two_clients.sh` | **1 BLOCKED** | 无 Unity 二进制 |
| `git diff --check` | 0 | |

`stable_gate.sh --full`、sanitizers、真实 Unity 进程 E2E **未**跑。

## 8. Client handoff

Unity 必须从服务器 **当前** 导出物重新生成 C#（不要停留在 `17912f2` / `f16462b6…`）：

```text
server_commit     c8550639c75b97abce6ed137a6354a7e151a8a45
schema_sha256     e57e2c4afad4d5434ae398508da1ccb9c455078da9bca26a3b570b3f06695f6f
descriptor_sha256 f37faa1f9e05472c500a366a1ac298fa426e513ba11ae9a0d6060880a9ff602e
```

```bash
./scripts/export_unity_protocol.sh /path/to/export_dir
# Luna:
#   Tools/GameMesh/import_server_contract.sh <server-repo-or-export-dir> c8550639c75b97abce6ed137a6354a7e151a8a45
```

Do not use `GAMEMESH_ALLOW_LEGACY_NO_HELLO=1`. This machine has no Unity Editor / StandaloneLinux64 build.
