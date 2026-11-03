# 回包延迟分位数的窗口

`gamemesh_reply_latency_p50_ms`、`p95_ms`、`p99_ms` 是进程启动以来累计直方图的桶上界，不是滑动窗口。样本只在进程重启时清空。两个 Gateway 的这两个 gauge 不能相加，也不能求平均：它们各自是桶边界，不是同一段时间里的延迟。

可以跨实例聚合的是 `gamemesh_reply_latency_ms_bucket`。监控页用 `histogram_quantile` 对 `rate(...[窗口])` 或 `increase` 之后的桶做 p50 / p95 / p99。窗口由页面时间范围换算，规则与 Grafana `$__rate_interval` 相同：`max(采集间隔 × 4, 时间范围 / 100)`，采集间隔按 15 秒。

`gamemesh_pushbatch_last_ms` 只是最近一次调用的耗时。分位数看 `gamemesh_pushbatch_duration_ms` 直方图。旧 gauge 保留一个版本做对照。

`gamemesh_control_outbox_peak_frames` 和 `peak_bytes` 是启动以来的单调峰值。页面只显示当前值卡片，不画成曲线。当前排队量看 `gamemesh_control_outbox_current_frames`。

上面这些阈值在页面常量 `THRESHOLDS` 里，标注为待用压测基线校准，不是已经验证过的数字。
