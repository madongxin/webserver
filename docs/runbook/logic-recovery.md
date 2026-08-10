# GameLogic / Placement 恢复

## 状态机

```text
READY → (lease 过期) RECOVERING → Migrate(新 Owner, epoch+1) → READY
```

多 Session 恢复调度器用 Redis `placement:recovery:leader` lease 互斥。

## 处置

1. 确认 MapLeaseKeeper 续租与 PlacementRecoveryScheduler Tick。
2. kill Owner 后等待自动接管；**不要**依赖手工 `map_lease_drill`（仅调试）。
3. 旧 epoch 写被拒绝；玩家按策略**重新进图**（非实时无损）。
4. 观察 audit 与 owner/epoch。

演练：`AUTO_RECOVER=1 scripts/test_logic_auto_recovery.sh`。
