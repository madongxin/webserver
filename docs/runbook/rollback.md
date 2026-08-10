# 回滚 Runbook

## 原则

- 回滚不得让旧二进制误读不兼容的新 Redis/MySQL 状态。
- Schema 变更必须可双读或可逆；否则先回滚写路径再回滚二进制。

## 步骤

1. 将目标角色标 DRAINING，摘除新流量。
2. 停止新版本进程；用上一稳定版本二进制与配置启动。
3. 确认 `/health/ready` 与登录/进图冒烟。
4. 若 Redis key 已用新前缀：切回旧 `key_prefix` 或执行文档化迁移回滚脚本。
5. 若 MySQL 迁移不可逆：保持库版本，仅回滚应用（迁移须设计为兼容）。

## 验证

```bash
./scripts/test_dual_gw_e2e.sh          # 或 final_e2e 场景 1–3
curl -fsS http://127.0.0.1:<http>/health/ready
```

## 联系点

保留门禁报告：`run/load/`、`run/soak/`、进程日志目录。
