# 部署 Runbook

## 前置

1. `./scripts/check_deps.sh --full`
2. `./scripts/bootstrap_local_config.sh`（或注入生产 cnf）
3. Redis / MySQL 就绪；`GAMEMESH_FORMAL=1` 时 fail-closed

## 本地 / E2E 拓扑

```bash
./scripts/build.sh Release
./scripts/run_e2e_cluster.sh          # 双 GW / Session / Logic / GameDB
# 客户端: source run/e2e/E2E_PORTS.env → TCP $E2E_GW0_GAME
./scripts/stop_e2e_cluster.sh
```

## 正式多进程

```bash
./scripts/start_formal.sh             # 见脚本内端口与 GAMEMESH_INSTANCE_ID
./scripts/stop_formal.sh
```

## Docker

```bash
# 推荐：先构建工具链镜像（钉 brpc 1.9.0），再以其为 base 或挂载 PREFIX
docker build -f Dockerfile.toolchain -t gamemesh-toolchain:brpc-1.9.0 .
# 应用镜像：build 阶段会执行 install_deps.sh --build-brpc（需网络）
docker build -t gamemesh:local .
docker compose -f compose.yml config
# 角色: gateway|session|gamelogic|world|gamedb （entrypoint argv）
```

报告与候选发布：`./scripts/stable_gate.sh --full` → `run/release/<commit>/manifest.json`。

## 滚动升级要点

1. 先部署兼容读（Session/GameDB schema 向后兼容）。
2. 单实例 DRAINING → 等在途结束 → 停进程 → 换二进制 → 注册 ACTIVE。
3. Gateway / Logic / Session / GameDB 分批；观察 `/metrics` 与错误码。

## 健康检查

- HTTP `/health/ready`、`/health/live`、`/api/version`
- Prometheus：`/metrics`
