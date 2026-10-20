# 31 排序发现（Top 5）

证据卡格式：问题ID | 级别 | 证据类 | …

## F1 | P1 | OBSERVED | 旧二进制 MOVE 积压「只留最后一帧」

- 现象：AOI 移动推送在队列积压时合并成大跳，客户端插值顿挫。  
- 首次：策略随 11:47 映像；本窗 hold 段 Push 量大。  
- 影响：所有观察者在逻辑忙时。  
- 日志/源码：`game/GameLogic.cpp` 旧 `CollapseMoves`；`docs/aoi200_subscription_contract.md` 原文。  
- 因果点：DrainAoiQueue 整段取出后合并。  
- 反证：无证明单次 VisibleIds 已达数十 ms。  
- 置信度：**高**。  
- 动作：已在 HEAD 改为 100ms 抽样；**重启正式集群**后复测。  
- 验证：观察者侧 MOVE 间隔直方图 ≥10Hz。

## F2 | P1 | OBSERVED+DERIVED | 出生点 scatter=0 + radius=1 造成极高局部密度

- 现象：一线百人叠同一坐标；AOI 候选/MOVE 事件量巨大（累计百万级）。  
- 源码/配置：`map_manifest` 1002 scatter=0，view_radius_cells=1。  
- 反证：无 cell 时序；不能给出精确 hotspot 人数曲线。  
- 置信度：**中高**。  
- 动作：压测对比 scatter>0；勿与 soft_cap 混淆。  
- 验证：同 CCU 下 aoi_move_events/s、push bytes/s 下降。

## F3 | P1 | OBSERVED | 机器人超速/不可走拒绝占满 Move 错误面

- 现象：累计 TOO_FAST≈1.8e5、UNWALKABLE≈6.9e5；用户确认「拉回」。  
- 旧逻辑拒写坐标放大拉回；HEAD 钳位未上线。  
- 分类：BOT_INVALID_INPUT / EXPECTED_GUARD，附带无效 CPU。  
- 置信度：**高（错误性质）**；对「服务器算力根因」置信度低。  
- 动作：重启钳位二进制 + 机器人降速/贴地。  

## F4 | P2 | OBSERVED | Hold 期大量 ClientHello 无 Login

- 现象：13:41–14:00 Hello 持续，login=0。  
- 影响：空连接与重试噪音。  
- 根因细节：机器人侧 NOT_OBSERVABLE。  
- 动作：客户端修重连状态机。  

## F5 | P2 | OBSERVED | 分线 occupancy 与 ONLINE 口径差（曾 407 vs 404）

- 现象：occ 含断线宽限期占位。  
- 非多登录。监控解读需分口径。  

未进 Top：MySQL/Redis 登录根因——**本轮无法证明**。
