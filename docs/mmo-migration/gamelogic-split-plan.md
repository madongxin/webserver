# GameLogic 拆分方案

只定顺序，不改代码。依据是当前 `game/GameLogic.h` / `game/GameLogic.cpp` 和 `game/MailService.cpp` 的依赖。

`GameLogic` 仍是进程内单例，一把 `mu_` 盖住背包、技能 CD、档案、资产版本和安全点。`MailService.cpp` 包含 `GameLogic.h`，`GameLogic.cpp` 又包含 `MailService.h`。

## 1. 现在混在一起的职责

| 块 | 方法 | 依赖的成员 |
| --- | --- | --- |
| 会话校验 | `RequireSessionToken`、`InvalidateSessionTokenCache`、`Handle` 入口 | 文件内 token 缓存，不在 `mu_` 里 |
| 背包与资产版本 | `EnsurePlayerLoaded`、`GetItemCount`、`ApplyItemReward`、`ApplyItemRewardsWithVersion`、`HandleConsumeItem`、`HandleGrantItem`、`TryReloadAssets` | `inventory_`、`asset_version_`、`asset_dirty_`、`player_load_ok_` |
| 档案与生命状态 | `LoadProfileUnlocked`、`GetPlayerAttributes`、`BindAuthenticatedPlayer`、`SetLifeStateForTest` | `profiles_` |
| 技能 | `HandleReleaseSkill` | `skill_cd_until_ms_` |
| 地图进出、移动、AOI | `HandleEnterMap`、`HandleLeaveMap`、`HandleMove`、`HandleInteractPortal`、`HandleRespawn`、`EmitAoi`、`BuildFullStateSnapshot` | 主要在 `MapRuntime`；快照还读背包和 `last_safe_` |
| 安全点 | `FlushLastSafe`、`MaybePersistLastSafe` | `last_safe_` |
| 跨逻辑迁移 | `ExportRuntimeState`、`ImportRuntimeState` | 背包、技能 CD、资产版本 |
| 邮件转发 | `Handle` 里的邮件 case | 调 `MailService`，自己不存邮件 |
| 好友转发 | `HandleFriendList` 以及 `Handle` 里的好友 case | 调 `FriendService` |
| 测试开关 | `SetAssetReloadBlockedForTest` 等 | `reload_*_for_test_`、`apply_blocked_for_test_` |

登录和注册在 `HandleLogin` / `HandleRegister` 里直接拒绝，不保存密码。

## 2. 拆分顺序

每一步都可以单独编译、单独跑现有测试。后一步不依赖前一步已经拆完，但按这个顺序冲突最少。

1. **命令表已经单独在 `CommandSpec`。** 下一步只让 `Handle` 按表查处理函数，不再在 `GameLogic.cpp` 里增加 case。路由测试继续用 `command_route_table_test`。
2. **先拆安全点。** `last_safe_` 和 `MapRuntime` 的位置是相邻的，但和背包锁无关。新类型 `LastSafeStore` 自己加锁，`MaybePersistLastSafe` 搬过去。`world_snapshot` 相关测试能单独编。
3. **再拆背包。** `PlayerInventory` 拿走 `inventory_`、`asset_version_`、`asset_dirty_` 和加载函数。`GetItemCount` 变成转发。`asset_dirty_consistency_test` 改调新类型，不需要先动地图。
4. **技能 CD 跟着背包一起搬，或作为背包的下一个小步。** 它只有 `HandleReleaseSkill` 在用，没有远程 IO。
5. **档案最后从 `mu_` 里拿出来。** `BindAuthenticatedPlayer` 同时碰档案和背包，所以要等背包的锁边界稳定。
6. **邮件环最后拆。** `MailService` 不再包含 `GameLogic.h`，只依赖 `PlayerInventory` 的发奖接口。这一步会动的编译单元最多，放在后面。

## 3. 和现有队列、地图的关系

`PlayerSerialQueue` 保证同一个玩家的 `Handle` 不会并发。拆出去的类型仍要从这条队列里调用，不要在地图线程里改背包。

`MapRuntime` 有自己的实例锁。`BuildFullStateSnapshot` 已经会在持 `mu_` 时调用它，再临时放开。拆背包之后，快照应先复制背包，再放开背包锁，然后才读 `MapRuntime`。不要把两把锁合成一把。

`EmitAoi` 已经在锁外推送。新的地图门面要保持这一点。

## 4. 必须做和只是好看

必须做：

- 拆掉 `MailService` 和 `GameLogic` 的头文件环。现在改其中一边的私有成员，另一边会跟着重编，测试也很难只链邮件。
- 背包和 `MapRuntime` 不再共用 `GameLogic::mu_`。否则地图慢查询仍会挡住发奖和消耗。
- `Handle` 的分支改由 `CommandSpec` 指向处理函数。否则每加一条协议还是要改这个上帝对象。

只是好看：

- 把 `HandleMove` 再拆成多个文件，但锁和调用方向不变。
- 给每个 `*ForTest` 单独做测试专用子类。现有测试能直接调，就不必先动。
- 把已经独立的 `FriendService` 再改名或挪目录。
