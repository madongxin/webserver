# GameMesh (CppWebServer) Agent Notes

## Topology (MVP)

- Client ↔ public **VIP:8081** (L4 LB) → Gateway ×2 (game TCP). HTTP admin and Push brpc ports are **internal only**.
- Gateway orchestrates login: **AuthService.Login** → **SessionService.AcquireSession** → **GameLogic.BindPlayer**.
- AuthService and SessionService share the `session` binary but keep separate protos; Auth does **not** create Sessions.
- Auth: Redis (access tokens) + GameDB/MySQL (accounts). Session: Redis (fence / ONLINE / DISCONNECTED).
- GameLogic PushBatch targets **one** Gateway by `gateway_instance_id` (not broadcast).
- Binary `world` = logical **GlobalService**. etcd optional; static `*_addrs` fallback via `IServiceRegistry`.

Details: `docs/mmo-migration/topology-auth-session.md`

## Do not

- Do not introduce gRPC or a separate login process.
- Do not rewrite Reactor.
- Do not let GameLogic accept passwords/credentials.
- MarkDisconnected ≠ Logout.

## Build / test

```bash
./scripts/check_deps.sh
./scripts/build.sh Debug          # C++17；支持 clean
./scripts/test_reactor.sh         # 阶段 0 Reactor / ProtoFraming
./scripts/test_unit.sh
./scripts/test_all.sh
./scripts/run_version.sh
./scripts/test.sh unit            # 兼容旧入口
./scripts/test.sh integration
```

全阶段路线图：`docs/GameMesh_Cursor_All_Phases.md`（阶段 0–3 见 `docs/mmo-migration/PHASE*_STATUS.md`）。
Session Redis：连接池 + Lua；正式业务 `Dispatch`；Register/Login 经 Auth→GameDB；`GAMEMESH_FORMAL=1` fail-closed。