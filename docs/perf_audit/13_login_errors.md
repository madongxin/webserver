# 13 登录相关错误

| 错误/现象 | 本窗 | 首发 | 分类 | 反证 |
| --- | --- | --- | --- | --- |
| login_fail | 0 | — | — | — |
| ERR_OVERLOADED | 0 日志 | — | 正常保护未触发 | — |
| ERR_MAP / NO_LINE | 0 | — | — | 分线创建成功 |
| ERR_UNAUTHENTICATED | Gateway 累计有（全日） | 多在 disc 后请求 | EXPECTED_GUARD / 时序 | 非登录洪峰主因 |
| KickConnection | 需抽样；非风暴 | — | — | 非本窗主导 |
| PushBatch stale fence | 2270 批日志，reject 消息和 3301 | ~13:40:30 | 断线/换会话后旧推送 | 见 AOI 报告；登录成功之后 |
| missing sticky gamelogic | disconnect flush 副作用 | 随 disc | 非路由丢失证明 | 与 disc 1:1 历史模式 |
| idle timeout 日志 | 3 行 | hold | 少量 | 累计 gauge 含全日 |

关联：提前 disconnect 176 独立玩家；lifetime 桶见 `summary.json`（15–25m 的 89 人对齐收工 Logout）。
