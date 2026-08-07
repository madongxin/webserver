# Runbook：灰度与回滚

## 静态配置灰度（无 etcd）

1. 准备新 Logic/World 实例（新端口或新机），健康检查：日志出现 `*BrpcServer listening`，`/metrics` 可访问。
2. 修改 `config/gateway.cnf`：
   - 扩容：追加 `logic_addrs` / `logic_instance_ids`（保持一一对应）
   - World：切换 `world_addrs` 到新实例
   - Session / GameDB：切换 `session_addrs` / `gamedb_addrs`
3. **滚动重启 Gateway**（当前实现启动期 Init Channel，不热更配置）。
4. 观察 Login、邮件、MapPing；异常则执行回滚。

## etcd 发现灰度（可选）

1. `gateway.cnf` 设置 `etcd_endpoints=http://127.0.0.1:2379`（或集群列表）。
2. 各服启动时 `EtcdDiscovery::Register`；Gateway 优先 `Discover(gamelogic|world|session)`。
3. Discover 失败时 **WARN 并回退** 静态 `logic_addrs` / `world_addrs` / `session_addrs`。
4. 摘流：停止实例并让 TTL 过期，或删 etcd key；Gateway 下次启动/重读时不再发现该实例（当前为启动期 Discover，需重启 Gateway）。

## 回滚路径

| 级别 | 动作 |
|------|------|
| 配置回滚 | 还原上一版 `gateway.cnf`（及 world/session/gamedb.cnf），重启 Gateway（及受影响角色） |
| 拓扑回滚 | 清空 `session_addrs` / `gamedb_addrs` / `etcd_endpoints`，回到进程内 SessionStore + 本地 AsyncMysql |
| 单体回滚 | `server all`（或 `server` 无 role）：InProcess + 本地邮件/会话，Mail brpc 工具口可选 |
| 二进制回滚 | 部署上一版 `角色二进制` / `server` 产物，保留同一套静态 cnf |

## SSL（mTLS）开关

各 `*.cnf`：`ssl_enable=0|1`，可选 `ssl_cert` / `ssl_key` / `ssl_ca`。默认关闭；开启后 Server/Channel 读入 brpc SSL options。本地联调保持 `ssl_enable=0`。

## NATS outbox

`config/gamedb.cnf`：`nats_url=` 空=仅日志并 MarkPublished；`nats://host:port` 时 PUB `gamedb.<event_type>` 成功后再标记。失败保留 unpublished 重试。
