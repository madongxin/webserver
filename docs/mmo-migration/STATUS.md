# STATUS — GameMesh 迁移进度

> 更新：2026-08-07  
> 正式基线：`docs/mmo-distributed-architecture.md`  
> 工程名：**GameMesh**（原 CppWebServer）

## 当前阶段

**阶段 7 — 独立 Session/GameDB + 生产化薄集成 + 多二进制：本轮已落地。**

## 阶段 0–6

- 已完成（见历史 STATUS / DECISIONS）

## 阶段 7 本轮完成

- [x] `apps/ServerBootstrap` + `gateway|gamelogic|world|session|gamedb`；`server role=*` 复用同一 Bootstrap
- [x] `session.proto` + `SessionBrpcServer`；配置 `session_addrs` 时走 Session RPC
- [x] `gamedb.proto` + `GameDbBrpcServer` + `BrpcGameDbRepository`
- [x] 可选 etcd（prefix=`gamemesh`）/ NATS / `ssl_enable`
- [x] runbook + `scripts/run_midterm_local.sh`

## 验证

```bash
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/run_midterm_local.sh
./scripts/run_version_local.sh
```

## 本版产品目标（可运营切片）

- [x] `gateway` / `gamelogic` / `gamedb` 多开（`logic_addrs` / `gamedb_addrs` + `player_id % N`；Gateway 多端口实例）
- [x] 玩家注册（`Register` → `player_account`）与登录上线
- [x] 下线：`Logout` 经 Session RPC；断线 `FlushBag` 刷道具队列
- [x] 发邮件 / 发道具 / 存库（既有路径 + 登录加载背包聚合）
- [x] **登录边界修正（2026-08-07）：** Gateway 编排 `Auth → AcquireSession → BindPlayer`；GameLogic 拒收 Login 凭证；`GatewayPushService`；`IServiceRegistry`；拓扑见 `docs/mmo-migration/topology-auth-session.md`

验证：`./scripts/run_version_local.sh`（2×gateway + 2×logic + 2×gamedb + Register→Login）
`./scripts/test.sh unit`（含 `auth_session_boundary_test`）

## 角色

正式一键启动（常驻多开：2×gateway + 2×logic + 2×gamedb）：

```bash
./scripts/build.sh Debug
./scripts/start_formal.sh    # 客户端连 Host + Port0=8081 或 Port1=8083
./scripts/stop_formal.sh
```

手动：

```bash
./build/test/session 8092 8401
./build/test/gamedb 8093 8501
./build/test/world 8091
./build/test/gamelogic 8090 8201
./build/test/gateway 8080 8081
./build/test/server all 8080 8081
```
