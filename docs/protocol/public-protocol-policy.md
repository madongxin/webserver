# 公网协议兼容策略

`proto/game.proto` 是客户端外网协议的唯一事实源。C++ 生成物在 `game/game.pb.*`，Unity 必须从本仓库导出物生成 C#，禁止手改生成代码或维护分叉 proto。

## 字段号冻结

- 已发布字段禁止改名、改类型、改号、删除后复用编号。
- 删除字段必须 `reserved` 原编号与名称。
- 新消息、新 oneof 成员只能追加未占用编号。
- `GameRequest`/`GameResponse` 顶层 `seq/ok/message` 编号不变。

## Unity 导入顺序

1. 服务器仓库 `./scripts/export_unity_protocol.sh <dir>` 产出：
   `game.proto`、`game.desc`、`protocol_manifest.json`、`game.proto.sha256`、`game.desc.sha256`
2. Unity 管道只消费该目录，并用 `schema_sha256` 与本地文件核对。
3. 用 `protoc` / protobuf-net 等从 **导出的 game.proto** 生成 C#（`csharp_namespace = GameMesh.Protocol`）。
4. 禁止把服务器 `game/game.pb.h` 或手改 C# 当协议源。

## schema hash、协议版本、build id

| 项 | 含义 |
| --- | --- |
| `protocol_version` | 整数协议世代；破坏性变更才加一 |
| `min_supported_protocol_version` | 服务器仍接受的最低客户端协议世代 |
| `schema_sha256` | 导出 `game.proto` 的 SHA-256 |
| `server_git_sha` | 导出时 HEAD；工作区脏则 `dirty=true` |
| 服务器 build id | 进程 `/api/version` 的 git sha，应与联调包 manifest 一致 |

握手（S1）时：协议世代不兼容 → `ERR_PROTOCOL_VERSION`；hash 不一致 → `ERR_SCHEMA_MISMATCH`（携带服务器 hash）。

Formal 默认强制 `ClientHello`。兼容开关 `GAMEMESH_ALLOW_LEGACY_NO_HELLO=1` 允许旧 C++ E2E 跳过握手，**截止 2026-09-15 后删除**。

## 向前兼容

- 加 optional/新 oneof 成员：旧客户端可忽略。
- 服务器不得依赖“客户端一定填新字段”才能完成 Register/Login/EnterMap。
- 最低支持版本以下的客户端必须强制升级，不能静默降级语义。

## 错误码

- 客户端只判断顶层或子响应的稳定 `error_code`，不解析 `message` 文本。
- 新增错误码使用 `ERR_` 前缀、大写蛇形；不复用已发布码的含义。
- 底层 MySQL/Redis/brpc 文本不得直接下发。
- S3 新增：`ERR_NAME_AMBIGUOUS`（精确名命中多行）、`ERR_NOT_FOUND`（公开资料不存在）、`ERR_CHANNEL_FORBIDDEN`（非 world 频道）。限流仍用 `ERR_RATE_LIMITED`。

## 检查

```bash
./scripts/check_public_protocol.sh
./build/test/protocol_compat_test   # 与 docs/protocol/published/v1/game.desc 比较
```
