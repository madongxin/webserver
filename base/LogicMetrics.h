#pragma once

/**
 * @file LogicMetrics.h
 * @brief GameLogic::Handle 单次业务耗时（进程内全局 last/peak）
 *
 * 当前架构下业务在 IO 线程同步执行；拆出独立逻辑线程后可在 loop 内复用 RecordHandle。
 */

namespace LogicMetrics {

void RecordHandle(double seconds);
double LastHandleSeconds();
double PeakHandleSeconds();

}  // namespace LogicMetrics
