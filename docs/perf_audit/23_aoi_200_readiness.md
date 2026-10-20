# 23 AOI=200 就绪度

## 本次实际 cap

- 配置 1002：`aoi_max_visible=200`（OBSERVED）。
- 运行映像推断按配置冻结（INFERRED）。
- **没有**「200 vs 50」对照实验，**没有** Move P95 在 cap=200 下的实测曲线。

**结论**：本次 **不能验收**「AOI=200 是否会卡」的性能目标；只能说功能配置已指向 200。

## 下轮隔离复测（同一硬件 4C/3.6G）

固定：server_sha、bot_sha/seed、405 或更小阶梯、move_hz、scatter（建议对比 scatter=0 vs >0）、双 GW。

矩阵：

1. 单图 100 / 200 / 400（硬上限研究）人，cap=200，原地微动 vs 跨 cell  
2. 多线分散 vs 单线热点  
3. 开启新二进制（100ms 抽样 + 速度钳位）A/B  
4. 采集：accepted_move_rps、aoi_candidate 均值、lock_wait、handle P95、push bytes、CPU  

成功标准示例：移动实体对观察者推送 ≥10Hz 抽样；TOO_FAST 钳位后客户端无大跳；无进程 OOM。
