# Server/Client Foundation Status — S3

Date: 2026-08-15  
Baseline HEAD: `145a64753aacd0d9e1dc7916edee81d15f183148`  
Worktree: dirty (S0+S1+S2+S3 uncommitted; not pushed)

## 1. HEAD and worktree

- `git rev-parse HEAD` → `145a64753aacd0d9e1dc7916edee81d15f183148`
- `git log -1` → `145a647 Ship the Unity integration slice: profile, public-map AOI, and player mail.`
- Working tree has S0–S3 modifications/untracked files. No commit. No push.

## 2. Files changed (S3)

S0/S1/S2 artifacts remain in the same dirty tree (protocol export, migrations, Hello/Heartbeat, world snapshot, last_safe). S3 additions/fixes:

| Path | Purpose |
| --- | --- |
| `proto/game.proto` | Append-only ChatNotify / PlayerBrief / GetPlayerBrief / QueryOnlineState; ChatSendRsp 4–6 |
| `proto/gamedb.proto` | `LoadPlayerBrief` RPC |
| `game/game.pb.*` / `runtime/brpc/gamedb.pb.*` | Regenerated |
| `game/GameLogic.cpp` | ChatSend world broadcast; GetPlayerBrief; QueryOnlineState; FriendList stub |
| `game/TrustedPlayerId.h` | Overlay querier `player_id` only; do not clobber `target_player_id` |
| `game/CommandPolicy.cpp` | Allowlist get_player_brief / query_online_state |
| `runtime/MessageRoute.cpp` | World-bound Chat / Friend / Brief / Online |
| `runtime/SocialText.h` | Name trim + UTF-8 bounds; world chat 1–200 cp / ≤800 bytes |
| `runtime/GatewayConnGuard.*` | Chat 5/2s and name-query 8/10s per connection |
| `game/SessionStore.*` | Redis `online:players` SET; chat/name quotas; nested Redis lease released before Track/Untrack |
| `redis/RedisClient.*` | `SAdd` / `SRem` / `SMembers` / `Incr` |
| `db/PlayerProfileStore.*` | `LoadByExactName` `LIMIT 3`; ≥2 → `ERR_NAME_AMBIGUOUS` |
| `db/BrpcGameDbRepository.*` / `GameDbServiceImpl.*` | Formal LoadPlayerBrief via GameDB |
| `apps/ServerBootstrap.cpp` | World/GameDB map `gateway_push_addrs`; GameDB SessionRpc + PushReplayStore |
| `runtime/brpc/GameLogicPush.*` | Optional fence/generation on PushToBoundGateway |
| `game/MailService.cpp` | MailboxChanged route via SessionRpc or local SessionStore |
| `game/GameTcpGateway.cpp` | Conn-level chat/name rate limits |
| `client/game_tcp_e2e_client.cpp` | `s3-social`; Exchange ignores seq=0 notifies while waiting for RPC |
| `scripts/test_s3_social.sh` | TCP social gate |
| `scripts/client_ready_gate.sh` | Hello → contract → AOI → capacity → mail → social → world snapshot |
| `scripts/stable_gate.sh` | DEV / CLIENT READY / STABLE CANDIDATE verdicts |
| `scripts/start_formal.sh` | World/GameDB `gateway_push_addrs` + GameDB `session_addrs` |
| `.github/workflows/ci.yml` | Public proto, migrate, client-ready TCP, protocol artifact |
| `docs/protocol/chat-world-channel.md` | World-chat scope |
| `docs/protocol/public-protocol-policy.md` | S3 public error codes |

Published v1 descriptor `docs/protocol/published/v1/game.desc` was **not** overwritten.

## 3. Proto field numbers (S3 append-only)

`ChatSendRsp` append (1–3 unchanged):

- `message_id = 4`
- `server_time_ms = 5`
- `channel = 6`

New messages:

- `ChatNotify` (`message_id=1` … `server_time_ms=6`)
- `PlayerBrief` (`player_id=1`, `player_name=2`, `max_hp=3`, `max_mp=4`)
- `GetPlayerBriefReq` (`player_id=1`, `target_player_id=2`, `player_name=3`)
- `GetPlayerBriefRsp` (`ok=1` … `brief=4`)
- `QueryOnlineStateReq` (`player_id=1`, `target_player_id=2`)
- `QueryOnlineStateRsp` (`ok=1` … `state=5`)

`GameRequest` oneof:

- `get_player_brief = 74`
- `query_online_state = 75`

`GameResponse` oneof:

- `chat_notify = 73`
- `get_player_brief = 74`
- `query_online_state = 75`

`gamedb.proto`: `LoadPlayerBrief` / `LoadPlayerBriefReq|Rsp`.

No field reuse or deletion. Trusted overlay covers 31 body cases; querier `player_id` is overwritten, `target_player_id` is preserved.

Current `game.proto` SHA-256: `4c29a73aa7fbed19f122e122bc1832852e593f6bfaca0b7433249391e2ec643d`  
Compat vs frozen v1: `protocol_compat_test` old=`docs/protocol/published/v1/game.desc` fields=415 (additions allowed).

## 4. Migration / rollback

No new SQL in S3. Display names stay non-unique; exact-name lookup is fail-closed (`ERR_NAME_AMBIGUOUS` when `LIMIT 3` returns ≥2 rows).

Existing: `./scripts/migrate_db.sh` applies `0001_player_profile_up.sql` and `0002_last_safe_position_up.sql`. Rollback last: `./scripts/rollback_db.sh`.

## 5. Commands actually run (this session)

| Command | Exit |
| --- | ---: |
| `GAMEMESH_JOBS=2 ./scripts/build.sh Debug` | 0 |
| `./scripts/test.sh unit` | 0 (inside stable_gate) |
| `./scripts/test.sh integration` | 0 (inside stable_gate) |
| `./scripts/check_public_protocol.sh` | 0 (earlier this S3 session) |
| `GAMEMESH_RUN_DIR=run/unity-e2e ./scripts/test_two_player_aoi.sh` | 0 (`mailbox_changed=1`, `mail_e2e_ok=1`) |
| `GAMEMESH_RUN_DIR=run/unity-e2e ./scripts/test_s3_social.sh` | 0 (`s3_social_ok=1`) |
| `./scripts/client_ready_gate.sh` | 0 `CLIENT READY PASS` summary `run/client_ready/summary_1786792728.json` |
| `./scripts/stable_gate.sh` | 0 `DEV PASS` summary `run/stable_gate/summary_1786792900.json` |
| `./scripts/stable_gate.sh --with-e2e` | 0 `CLIENT READY PASS` summary `run/stable_gate/summary_1786792945.json` |

`--with-e2e` client-ready steps (all exit 0): hello_heartbeat, unity_contract, two_player_aoi, map_capacity, player_mail, s3_social, world_snapshot. Nested summary: `run/client_ready/summary_1786792976.json`.

S3 social TCP: unique names, brief by id/name, `online_state=online`, `chat_notify` same `message_id`, FriendList `NOT_IMPLEMENTED`, name/chat flood → `ERR_RATE_LIMITED`.

## 6. Not run

- `./scripts/stable_gate.sh --full` (20× E2E, 30 min load, 2 h soak, sanitizer rebuilds). Not started; would also be blocked by dirty worktree.
- Unity client import/regenerate (`luna` not modified).
- ASan/UBSan/TSan rebuilds outside `--full`.

## 7. Gate result

**S0 PASS** (previous phase).  
**S1 PASS** (previous phase).  
**S2 PASS** (previous phase).  
**S3 PASS** for DEV and CLIENT READY.

| Verdict | Result |
| --- | --- |
| DEV PASS | yes (`stable_gate.sh` exit 0) |
| CLIENT READY PASS | yes (`client_ready_gate.sh` and `stable_gate.sh --with-e2e` exit 0) |
| STABLE CANDIDATE PASS | **no** — `NOT RUN / STABLE BLOCKED` (`--full` soak/load not executed; worktree dirty) |

Do not start S4 until this report is accepted and (if required) committed.

## 8. Remaining limits

- `FriendList` remains `NOT_IMPLEMENTED` (world-module boundary only).
- World chat is unreliable (`reliable=false`, `server_seq=0`); not durable; client dedupes by `message_id`.
- Unity `luna` still needs a follow-up import of this `game.proto` (Hello/Heartbeat + S2 snapshot + S3 social).
- Display names are not unique by schema; ambiguous exact-name queries fail closed.
- Default `--full` stable candidate still requires a clean tree plus 30 min load and 2 h soak on this commit.
