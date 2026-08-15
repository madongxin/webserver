#pragma once

#include "game.pb.h"

#include <cstdint>
#include <string>

namespace gameproto {

constexpr const char *kErrOk = "OK";
constexpr const char *kErrInvalidArgument = "ERR_INVALID_ARGUMENT";
constexpr const char *kErrUnauthenticated = "ERR_UNAUTHENTICATED";
constexpr const char *kErrFenceStale = "ERR_FENCE_STALE";
constexpr const char *kErrSessionExpired = "ERR_SESSION_EXPIRED";
constexpr const char *kErrProtocolVersion = "ERR_PROTOCOL_VERSION";
constexpr const char *kErrSchemaMismatch = "ERR_SCHEMA_MISMATCH";
constexpr const char *kErrClientUpgradeRequired = "ERR_CLIENT_UPGRADE_REQUIRED";
constexpr const char *kErrRateLimited = "ERR_RATE_LIMITED";
constexpr const char *kErrOverloaded = "ERR_OVERLOADED";
constexpr const char *kErrDependencyUnavailable = "ERR_DEPENDENCY_UNAVAILABLE";
constexpr const char *kErrMapFull = "ERR_MAP_FULL";
constexpr const char *kErrMapDataMismatch = "ERR_MAP_DATA_MISMATCH";
constexpr const char *kErrNotOnMap = "ERR_NOT_ON_MAP";
constexpr const char *kErrStaleSeq = "ERR_STALE_SEQ";
constexpr const char *kErrMoveTooFast = "ERR_MOVE_TOO_FAST";
constexpr const char *kErrAoiResyncRequired = "ERR_AOI_RESYNC_REQUIRED";
constexpr const char *kErrSnapshotTooLarge = "ERR_SNAPSHOT_TOO_LARGE";
constexpr const char *kErrPlayerDead = "ERR_PLAYER_DEAD";
constexpr const char *kErrInternal = "ERR_INTERNAL";
constexpr const char *kErrCommandForbidden = "ERR_COMMAND_FORBIDDEN";
constexpr const char *kErrNameAmbiguous = "ERR_NAME_AMBIGUOUS";
constexpr const char *kErrNotFound = "ERR_NOT_FOUND";
constexpr const char *kErrChannelForbidden = "ERR_CHANNEL_FORBIDDEN";

bool ErrorCodeRetryable(const std::string &code);
int64_t PublicNowMs();
std::string NewTraceId(uint64_t conn_id, uint64_t seq);

/** 去掉 MySQL/Redis/brpc 等底层文本。 */
std::string SanitizePublicMessage(const std::string &raw);

void FillPublicError(game::GameResponse *rsp, const char *error_code, const char *safe_message,
                     uint64_t seq = 0, uint64_t conn_id = 0);

/** 从子响应提升 error_code；补齐 retryable/server_time_ms/trace_id；计数。 */
void PromotePublicError(game::GameResponse *rsp, uint64_t conn_id = 0);

bool EncodePublicErrorFrame(const char *error_code, const char *safe_message, uint64_t seq,
                            uint64_t conn_id, std::string *out_frame);

}  // namespace gameproto
