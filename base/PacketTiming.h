#pragma once

#include <cstdint>
#include <string>

/** 按包种记录处理、RPC、回包排队耗时，以及发送队列水位。慢包写日志，水位给监控。 */

void NotePktStage(const char *kind, const char *stage, int64_t ms, uint64_t conn, uint64_t player,
                  uint64_t seq, int app_bytes, int unsent_bytes);

void NoteSendPending(int app_bytes, int unsent_bytes);

void NoteMoveOffer(bool scheduled_new);
void NoteMoveFlush(bool sent, int entities);
/** 内核未发送字节还多，最新移动帧只留在应用缓冲，没有 write。 */
void NoteMoveHold(int entities);
void NoteMovePendingEntities(int n);

/** 打出上一个 10 秒窗口里最慢的包和发送队列水位，并刷新监控读到的最大值。 */
void PacketTimingLogWindow();

std::string PacketTimingPrometheus();
