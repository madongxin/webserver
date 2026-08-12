# Urgent Minimum Stable Fix — 阶段二 / 稳定候选结论

基线起点：`857d963`  
稳定候选 commit：`b0fecdc`  
Tag：`server-stable-v0.1.0-rc1`  
状态：**STABLE PASS**（`./scripts/stable_gate.sh --full`）

## 门禁（commit `b0fecdc`）

| 项 | 结果 |
|------|------|
| E2E 20× | PASS |
| Load 1800s / conc=32 | PASS（100%；先驱跑曾 94% 后修 client 等待） |
| Soak 7200s | PASS（100%） |
| `stable_gate --full` | **STABLE PASS — candidate server-stable-v0.1.0-rc1** |
| working tree | clean |
| Release manifest | `run/release/b0fecdc664213a9745e9b846469808337163fa60/manifest.json` |

## 为达 STABLE 追加的关键修复

1. Placement `ResolveOrCreate`：READY+lease 过期软续租；RECOVERING 硬 reclaim  
2. EnterMap 允许本地 lease 过期再 Claim；断线 Unbind 释放空地图  
3. E2E 冷启动等待 sess-1 / register-login  
4. e2e client 缩短 Push RecvFrame 空等，避免 dual-gw 压测落入 8s 失败带  

## 稳定支持范围（联调）

- 双 GW / 双 Session / 双 Logic / 双 GameDB 固定拓扑  
- 登录 Auth→Session→BindPlayer；跨 GW 重连；Session/GameDB failover  
- **不承诺**：实时地图无损迁移、动态扩容 gl-2、Placement 自动恢复（experimental 默认关）  

## 报告路径

- Load：`run/load/load_20260811T115948Z.json`（门禁内另有复跑报告）  
- Soak：`run/soak/soak_20260811T122958Z.json`  
- Gate：`run/stable_gate/summary_1786458599.json`  
- 长门禁日志：`run/long_gate/summary_stable.txt`
