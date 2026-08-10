# 阶段 0 状态（GameMesh_Cursor_All_Phases）

> 更新：2026-08-08
> 对照：`docs/GameMesh_Cursor_All_Phases.md` 阶段 0

## 已落地

- C++17：`CMAKE_CXX_STANDARD=17`，去掉全局 `-std=c++14`
- Connection ID：`uint64_t` 单调递增，移除 `1000` 回绕；proto `forward`/`session` 的 `connection_id` 改为 `uint64`
- ProtoFraming：`Complete` / `Incomplete` / `Invalid`；非法长度关连
- Buffer：恰好消费全部可读字节（`>=` 边界）
- Channel：`DisableWrite`；发送队列排空后取消 EPOLLOUT
- TcpConnection：读写缓冲上限 + 慢客户端背压关连；`proto_stream` 挂连接上并在关闭时清理
- `TimeStamp::AddTime`：修复 fractional seconds 被截断为 0 的 bug
- 脚本：`build.sh`（打印编译器/C++17/clean）、`test_unit.sh`、`test_reactor.sh`、`run_version.sh`、`test_all.sh`

## 验证命令

```bash
./scripts/build.sh Debug
./scripts/test_reactor.sh
./scripts/test_unit.sh
./scripts/run_version.sh
```

## 风险 / 未完成（不阻塞进入阶段 1 的最小集，但需知晓）

- ASan 全链路（login/收发/断连）尚未在本机跑通报告
- Gateway 绑定表仍有全局 mutex（热路径拆帧已去掉全局 stream 锁）
- `test.sh` 里部分 integration 路径仍有历史 `WARN`/`SKIP` 风格；阶段 0 新脚本已按 fail-closed 执行
