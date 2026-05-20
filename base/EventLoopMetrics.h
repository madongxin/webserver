#pragma once

namespace EventLoopMetrics {

void RecordTick(double seconds);
double LastTickSeconds();
double PeakTickSeconds();

}  // namespace EventLoopMetrics
