#pragma once

#include "game.pb.h"

#include <cstdint>
#include <string>

namespace gameproto {

constexpr uint32_t kPublicProtocolVersion = 1;
constexpr uint32_t kMinSupportedProtocolVersion = 1;
constexpr uint32_t kDefaultHeartbeatIntervalMs = 15000;
constexpr uint32_t kDefaultIdleTimeoutMs = 45000;
constexpr uint32_t kDefaultHelloDeadlineMs = 10000;

const char *CompiledSchemaSha256();
std::string EffectiveSchemaSha256();
std::string ServerBuildId();
uint32_t HeartbeatIntervalMs();
uint32_t IdleTimeoutMs();
uint32_t HelloDeadlineMs();
uint32_t HeartbeatMinIntervalMs();

bool ClientVersionTooOld(const std::string &client_version);

/** 处理 ClientHello；结果写入 *hello。不访问 Redis/MySQL。 */
void HandleClientHello(const game::ClientHelloReq &req, uint64_t conn_id, game::ServerHelloRsp *hello,
                       game::GameResponse *outer);

}  // namespace gameproto
