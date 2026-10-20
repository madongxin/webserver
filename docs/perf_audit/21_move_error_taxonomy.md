# 21 Move 错误分类

源码：`MapRuntime::Move`（压测二进制为 11:47：**超速整包拒绝**，无钳位；HEAD 已改钳位但**未重启**）。

| 错误码 | 本窗日志行 | 进程累计 metrics | 分类 | 说明 |
| --- | --- | --- | --- | --- |
| ERR_MOVE_TOO_FAST | 窗口文本几乎无（走 error_code 指标） | ~1.8e5 | BOT_INVALID_INPUT / EXPECTED_GUARD | dt×speed×1.25+0.5；拒时旧逻辑不回写坐标 → 客户端易连续打飞 |
| ERR_UNWALKABLE | 同上 | ~6.9e5 | BOT_INVALID_INPUT | 叠加点/乱走撞不可走 |
| ERR_STALE_SEQ | 0 | 0 | — | 未见乱序主因 |
| ERR_OUT_OF_BOUNDS | 未在窗口文本突出 | — | — | |
| ERR_NOT_ON_MAP | 未作主窗风暴 | — | STATE/时序 | 与 disc 相关时需个案 |
| PushBatch stale fence | 2270 批日志 | reject 累加 | EXPECTED_GUARD / PERFORMANCE_CASCADE 边缘 | 断线后旧 session 推送被拒；logout 分钟暴涨 |

每千次 Move 接收拒绝率：**NOT_OBSERVABLE**（无 move_ok 分母）。

## 与机器人关系

用户已确认：站立不动主因是走太快被拉回。本窗 TOO_FAST/UNWALKABLE 累计支持该判断（**不能**再把「看起来卡」主要记到 AOI 锁上，除非有锁等待时序）。
