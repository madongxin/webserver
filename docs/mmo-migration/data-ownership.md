# 数据所有权（阶段 3）

| 数据 | 权威服务 | 存储 | 禁止 |
|---|---|---|---|
| 账号、密码哈希、封禁 | AuthService → GameDB | MySQL `player_account` | Gateway/GameLogic 直写账号 |
| 会话、fence、路由 | SessionService | Redis | GameLogic 创建 Session |
| 玩家资产/背包 | GameLogic → GameDB | MySQL | Session 存资产 |
| 全局邮件等 | GlobalService(`world`) → GameDB | MySQL | Gateway 直连 MySQL 改业务事实 |
| SQL 执行 | GameDB | MySQL | Auth/Logic 在 **FORMAL** 下绕过 GameDB |

## 正式模式开关

```bash
export GAMEMESH_FORMAL=1
export GAMEMESH_HTTP_BIND=127.0.0.1   # 默认在 FORMAL 下强制 loopback
export GAMEMESH_ENABLE_ADMIN=0       # 危险 HTTP 接口默认关
```

- FORMAL=1：Auth 账号查询/注册必须经 GameDB；无密码账号拒绝登录。
- 开发模式（未设 FORMAL）：Auth 可降级本地 `PlayerAccountStore` 并打印 WARN。
