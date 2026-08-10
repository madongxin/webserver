# 阶段 3 状态（GameMesh_Cursor_All_Phases）

> 更新：2026-08-08
> 对照：`docs/GameMesh_Cursor_All_Phases.md` 阶段 3

## 已落地

### 3.1 真实 Auth
- PBKDF2-HMAC-SHA256（`runtime/PasswordHash`）+ salt/iters 存 MySQL
- `AuthService.Register` → `GameDB.RegisterAccount`；Login 校验 credential
- Gateway 编排 Register/Login（含 credential）；GameLogic **拒绝** Register/Login
- 账号维度登录失败限流；access token 日志脱敏
- 旧无密码账号：仅非 FORMAL 可空凭证登录

### 3.2 数据所有权
- `GAMEMESH_FORMAL=1` fail-closed（见 `data-ownership.md`）
- Auth 正式模式禁止本地 MySQL 降级

### 3.3 GameDB
- RegisterAccount / LookupAccount（含 hash 字段）
- ClaimMail 既有幂等；Register 带 idempotency_key 字段（存储侧可扩展）

### 3.4 内部安全
- 危险 HTTP（GM/asan/db ping）默认关闭；`GAMEMESH_ENABLE_ADMIN=1` 开启
- ASan 崩溃接口需 `GAMEMESH_ENABLE_ASAN_CRASH=1`
- FORMAL 默认 HTTP 管理口绑定 `127.0.0.1`
- brpc `ssl_enable` 配置入口保留（`BrpcSslUtil`）

### 3.5 健康 / 优雅停机
- `/api/liveness` `/api/readiness` `/api/version`
- SIGTERM/SIGINT → draining → `EventLoop::Quit` → brpc Stop
- `scripts/stop_cluster.sh` 先 TERM 后 KILL

### 3.6 可观测（基础）
- version 含 git_sha/build_time 宏位；登录失败/成功结构化日志（无明文密码）
- Prometheus 进程指标仍在；登录/brpc 全量直方图后续补齐

### 3.7 脚本 / Docker / CI
- `bootstrap.sh` `start_cluster.sh` `stop_cluster.sh` `smoke.sh` `load_test.sh` `chaos_kill.sh` `test_integration.sh`
- `Dockerfile` + `deploy/compose.cluster.yml`
- `.github/workflows/ci.yml`

### 3.8 文档
- 本文件 + `data-ownership.md`

## 验证命令

```bash
./scripts/bootstrap.sh
./scripts/build.sh Debug
./scripts/test_unit.sh
./scripts/test_integration.sh   # 需 Redis
./scripts/test_all.sh
```

## 未完成 / 风险

- 第三方 JWT / 完整 RBAC / mTLS 证书轮换 runbook 未完整
- GameDB RPC 熔断与 outbox multi-worker claim 未完成
- Docker 镜像构建依赖本机 brpc 预装；CI 无 brpc 时可能仅部分编译
- 全链路 smoke（登录+Push+跨 GW 重连）仍建议人工/专用客户端补齐
- MySQL HA/备份文档仅原则说明，未出独立运维手册
