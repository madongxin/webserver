# 00 — 仓库审计（阶段 0 / W0）

> 日期：2026-08-07
> 基线方案：`docs/mmo-distributed-architecture.md`
> 原则：仓库事实高于外部任务书假设

---

## 1. Reactor 调用链

```text
Acceptor::HandleRead
  → TcpServer::HandleNewConnection
  → 连接绑定到子 EventLoop，注册读回调
  → epoll 可读 → TcpConnection::HandleMessage
  → ReadNonBlocking → message_callback
  → GameTcpGateway::OnMessage
  → ProtoFraming::TryDecodeOneFrame
  → gameproto::HandleFrame
  → GameLogic::Handle
  → TcpConnection::Send（可能进 send_buf_ 等 EPOLLOUT）
```

- HTTP 路径：主 `EventLoop` + `HttpServer`（`test/http_server.cpp`）。
- 游戏 TCP：`GameTcpGateway` **独立线程** 内再建一套 `TcpServer`/`EventLoop`（与 HTTP 分离）。
- Poller：Linux `epoll`（`tcp/Epoller.*`），one-loop-per-thread 风格。

---

## 2. 线程 / 事件循环

| 项 | 事实 |
|----|------|
| HTTP IO 线程数 | `hardware_concurrency()`（至少 4） |
| Game Gateway | 独立 `std::thread` + 自有 EventLoop |
| 业务执行位置 | **在连接所属 EventLoop 上同步**调用 `GameLogic::Handle` |
| 定时器 | `TimerQueue` 挂在 EventLoop；Metrics/道具刷盘/邮件扫描等 |
| 连接表 | `TcpServer::connectionsMap_`（fd → TcpConnection） |
| brpc（可选） | `BrpcGameServer` 另起，默认 `0.0.0.0:8181` |

---

## 3. 可能阻塞 IO 线程的调用（证据）

| 位置 | 风险 |
|------|------|
| `GameLogic::Handle*` | 同步业务；含 Redis session 校验、部分 MySQL（邮件等） |
| `SessionStore` | hiredis 同步调用（若在 IO 路径上） |
| `MailStore` / `ConnectionPool` | 从池取连接 + SQL，邮件路径可能打到 IO 线程 |
| `PlayerItemPersistQueue` | 周期任务在 loop 上；登出 Flush 可能同步 |

→ 对应正式方案阻塞点 §2.3；阶段 1 起要解耦。

---

## 4. 协议

| 项 | 事实 |
|----|------|
| 帧格式 | 4 字节大端长度 + protobuf body（`ProtoFraming`） |
| 最大帧 | `kMaxFrameSize = 4MB` |
| 消息 | `game::GameRequest` / `GameResponse`（`proto/game.proto`） |
| 版本字段 | 无独立 protocol_version 信封；靠 protobuf 字段兼容 |
| 心跳 | 未见独立游戏心跳帧（阶段 3 再补） |
| 请求关联 | `seq` 等在 GameRequest/Response 内 |

---

## 5. 会话

| 项 | 事实 |
|----|------|
| 存储 | Redis `SessionStore`（`ENABLE_REDIS`） |
| 字段 | token、server_id、login_time、device_id |
| 绑定 | 逻辑上 token↔player；**无** gateway_id/connection_id/fence_token |
| 重复登录 | 现实现以 Store 为准；无完整 Session 状态机 |
| 无 Redis | 部分路径放行（与 brpc mail 适配一致） |

---

## 6. 构建 / 依赖 / 测试入口

| 项 | 事实 |
|----|------|
| 语言 | 全局 `-std=c++14`；RocksDB/部分 brpc 源文件局部 C++17 |
| 主 target | `server` → `build/test/webserver` |
| CMake options | `ENABLE_MYSQL` `ENABLE_GAME_PROTOBUF` `ENABLE_REDIS` `ENABLE_ROCKSDB`(OFF) `ENABLE_BRPC`(OFF) `ENABLE_ASAN` |
| Protobuf | 预生成 `game/game.pb.cc` 等；改 proto 需手动 `protoc` |
| brpc | `/usr/local` 的 libbrpc；邮件双栈可选 |
| DB | MySQL + `ConnectionPool` |
| 脚本 | 原先仅有 `scripts/refresh_ide.*`；阶段 0 起补 `check_deps`/`build`/`capture_baseline` |
| 测试 | `mail_unit_test`、`file_hash_test`、`mail_claim_integration_test`、`rocksdb_demo_test`、`mail_brpc_client` |

**推荐配置命令（基线）：**

```bash
./scripts/check_deps.sh
./scripts/build.sh Debug
# 或：
cmake -S . -B build -DENABLE_MYSQL=ON -DENABLE_GAME_PROTOBUF=ON -DENABLE_REDIS=ON -DENABLE_BRPC=ON
cmake --build build -j"$(nproc)"
```

**运行：**

```bash
./build/test/server [http_port] [game_port]
# 默认：8080 / 8081
curl -s http://127.0.0.1:8080/metrics
```

---

## 7. 现有指标清单（/metrics）

| 指标 | 来源 | 备注 |
|------|------|------|
| `process_cpu_seconds_total` | `/proc` | 有 |
| `process_resident_memory_bytes` | `/proc` | 有 |
| `process_open_fds` | `/proc` | 有 |
| `process_threads` | `/proc` | 有 |
| `gamemesh_os_threads{state=}` | `/proc` | 有 |
| `gamemesh_eventloop_tick_seconds` | EventLoopMetrics | last |
| `gamemesh_eventloop_tick_peak_seconds` | EventLoopMetrics | peak |
| `gamemesh_logic_handle_seconds` | LogicMetrics | last |
| `gamemesh_logic_handle_peak_seconds` | LogicMetrics | peak |
| `tcp_send/recv_queue_bytes` | ProcSelfStats | **在 snapshot，未进 /metrics** |

### 阶段 0 缺口（记录，本阶段不改行为强补）

- 活跃 TCP **连接数**（Http + Game）未导出
- 无消息 **QPS / 错误率 / 延迟直方图（p50/p99）**
- 无按 message_type 分类
- 无长连接压测器（仅有 `mail_brpc_client` 等手动客户端）

→ 基线先用现有 gauge + 手工/脚本采样；QPS/p99 在压测工具落地后补（可放阶段 0 尾或阶段 1 初，另开切片）。

---

## 8. 地图相关

仓库中 **无** `map_template_id` / `map_instance_id` / AOI / 场景 Tick。
现有 ID：`player_id`、邮件 id、item 配置 id。
→ 阶段 5 前一律按 `player_id → logic` 路由（正式方案 D3）。

---

## 9. 最小风险迁移地图（现有 → 目标）

| 现有 | 目标 |
|------|------|
| `tcp/*` | Gateway 数据面 |
| `GameTcpGateway` / `ProtoFraming` | `gateway` / `net/framing` |
| `GameLogic` | `gamelogic` |
| `SessionStore` | `session` |
| `Mail*` / `game/brpc` | `world` 中控（邮件模块） |
| `ConnectionPool` / PersistQueue | GameDB |
| `http_server` main | `apps` + `--role=all`（后期） |

---

## 10. 审计结论

- 适合按正式方案 **阶段 0→1→2…** 推进；勿跟 `cpp17_1` 把战斗进程叫 World。
- 阶段 0 交付物：本审计 + 构建脚本 + 基线采集方法 + STATUS；**不改变协议行为**。
