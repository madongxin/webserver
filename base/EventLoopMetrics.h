#pragma once

namespace EventLoopMetrics {

void RecordTick(double seconds);
double LastTickSeconds();
double PeakTickSeconds();

/** 跨线程任务队列深度，以及一轮 DoToDoList 的批大小和耗时。 */
void RecordTodoDepth(int depth);
void RecordBatch(int n, double seconds);
int LastTodoDepth();
int LastBatchSize();
double LastBatchSeconds();
/** 取走自上次调用以来的最大值，供 10 秒窗口打点。 */
int ExchangeRecentTodoMax();
int ExchangeRecentBatchMax();
int ExchangeRecentBatchMsMax();

}  // namespace EventLoopMetrics
