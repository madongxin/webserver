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
/** 这一拍没有把位置写进内核：连接正在重传。 */
void NoteAoiSkipRetrans();
/** 这一拍没有把位置写进内核：内核里还没发出去的字节超过 16KB。 */
void NoteAoiSkipUnsent();
/** 在途降到 16KB 以内，写出了这一帧位置。 */
void NoteAoiWriteRoom();
/** 位置已经停够 1 秒，强制写出这一帧。 */
void NoteAoiWriteStarved();
void NoteMovePendingEntities(int n);

/** 打出上一个 10 秒窗口里最慢的包和发送队列水位，并刷新监控读到的最大值。 */
void PacketTimingLogWindow();

std::string PacketTimingPrometheus();
