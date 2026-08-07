# 01 — 实施计划（对照正式基线）

> 权威步骤见：`docs/mmo-distributed-architecture.md` §8 / §9  
> 本文只跟踪执行状态与本仓库命令，不另起炉灶。

---

## 阶段总览

| 阶段 | 目标 | 状态 |
|------|------|------|
| 0 | 基线冻结：审计、构建脚本、指标采集、压测入口说明 | **完成** |
| 1 | 单进程解耦 + player 串行队列 | **完成** |
| 2 | Gateway ↔ GameLogic（brpc）一条链 + 双 Logic | **完成** |
| 3 | Session / fence / 重连顶号 | **完成** |
| 4 | GameDB 幂等垂直样例（邮件领取） | **完成** |
| 5 | 地图 Placement 骨架（无 AOI/Tick） | **完成** |
| 6 | World 中控（邮件 + 聊天/好友骨架） | **完成（待确认）**；etcd/mTLS 延后 |

---

## 阶段 0 工作项

| ID | 内容 | 验收 | 状态 |
|----|------|------|------|
| W0 | 仓库审计 → `00-repository-audit.md` | 事实与正式方案对齐 | 本轮完成 |
| W0b | `DECISIONS.md` / `STATUS.md` 初始化 | 可续作 | 本轮完成 |
| W1a | `scripts/check_deps.sh` + `scripts/build.sh` | 可重复构建 | 本轮完成 |
| W1b | `scripts/capture_baseline.sh` + 基线记录模板 | 能拉 /metrics 落盘 | 本轮完成 |
| W1c | 压测入口说明（现有工具 + 缺口） | 文档可复现步骤 | 本轮完成 |
| W1d | 在相同配置下至少采一轮基线数字 | `baselines/` 有样本或注明环境未起服 | 本轮尽量完成 |

**阶段 0 不做：** 改协议、拆进程、引入 etcd/NATS、补全连接数/QPS 代码（仅登记缺口）。

---

## 下一阶段入口（阶段 1）

仅在阶段 0 STATUS 标为完成且人工确认后开始：

- `SessionHandle` / `ReplySink` / `InProcessTransport`
- 按 `player_id` 串行队列；IO 只解码入队

---

## 常用命令

```bash
./scripts/check_deps.sh
./scripts/build.sh Debug
./build/test/server 8080 8081
./scripts/capture_baseline.sh http://127.0.0.1:8080
./build/test/mail_unit_test
./build/test/file_hash_test
```
