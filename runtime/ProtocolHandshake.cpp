#include "ProtocolHandshake.h"

#include "FormalMode.h"
#include "PublicError.h"

#include <cstdlib>
#include <cstring>
#include <string>

#ifndef GAMEMESH_SCHEMA_SHA256
#define GAMEMESH_SCHEMA_SHA256 ""
#endif
#ifndef GAMEMESH_GIT_SHA
#define GAMEMESH_GIT_SHA "unknown"
#endif

namespace gameproto {
namespace {

uint32_t EnvU32(const char *name, uint32_t def) {
    const char *v = std::getenv(name);
    if (!v || !*v)
        return def;
    char *end = nullptr;
    const unsigned long n = std::strtoul(v, &end, 10);
    if (end == v)
        return def;
    if (n > 3600000UL)
        return def;
    return static_cast<uint32_t>(n);
}

int CmpDottedVersion(const std::string &a, const std::string &b) {
    size_t ia = 0, ib = 0;
    while (ia < a.size() || ib < b.size()) {
        unsigned long va = 0, vb = 0;
        while (ia < a.size() && a[ia] != '.') {
            if (a[ia] >= '0' && a[ia] <= '9')
                va = va * 10 + static_cast<unsigned>(a[ia] - '0');
            ++ia;
        }
        while (ib < b.size() && b[ib] != '.') {
            if (b[ib] >= '0' && b[ib] <= '9')
                vb = vb * 10 + static_cast<unsigned>(b[ib] - '0');
            ++ib;
        }
        if (va < vb)
            return -1;
        if (va > vb)
            return 1;
        if (ia < a.size() && a[ia] == '.')
            ++ia;
        if (ib < b.size() && b[ib] == '.')
            ++ib;
    }
    return 0;
}

}  // namespace

const char *CompiledSchemaSha256() { return GAMEMESH_SCHEMA_SHA256; }

std::string EffectiveSchemaSha256() {
    const char *env = std::getenv("GAMEMESH_SCHEMA_SHA256");
    if (env && *env)
        return env;
    return GAMEMESH_SCHEMA_SHA256;
}

std::string ServerBuildId() {
    const char *env = std::getenv("GAMEMESH_BUILD_ID");
    if (env && *env)
        return env;
    return GAMEMESH_GIT_SHA;
}

uint32_t HeartbeatIntervalMs() {
    return EnvU32("GAMEMESH_HEARTBEAT_INTERVAL_MS", kDefaultHeartbeatIntervalMs);
}

uint32_t IdleTimeoutMs() { return EnvU32("GAMEMESH_IDLE_TIMEOUT_MS", kDefaultIdleTimeoutMs); }

uint32_t HelloDeadlineMs() {
    const uint32_t idle = IdleTimeoutMs();
    const uint32_t hello = EnvU32("GAMEMESH_HELLO_DEADLINE_MS", kDefaultHelloDeadlineMs);
    return hello < idle ? hello : idle;
}

uint32_t HeartbeatMinIntervalMs() {
    return EnvU32("GAMEMESH_HEARTBEAT_MIN_INTERVAL_MS", 250);
}

bool ClientVersionTooOld(const std::string &client_version) {
    const char *minv = std::getenv("GAMEMESH_MIN_CLIENT_VERSION");
    if (!minv || !*minv)
        return false;
    if (client_version.empty())
        return true;
    return CmpDottedVersion(client_version, minv) < 0;
}

void HandleClientHello(const game::ClientHelloReq &req, uint64_t conn_id, game::ServerHelloRsp *hello,
                       game::GameResponse *outer) {
    if (!hello || !outer)
        return;
    const std::string schema = EffectiveSchemaSha256();
    hello->set_protocol_version(kPublicProtocolVersion);
    hello->set_min_supported_protocol_version(kMinSupportedProtocolVersion);
    hello->set_schema_sha256(schema);
    hello->set_server_build(ServerBuildId());
    hello->set_server_time_ms(PublicNowMs());
    hello->set_heartbeat_interval_ms(HeartbeatIntervalMs());
    hello->set_idle_timeout_ms(IdleTimeoutMs());
    hello->add_capabilities("hello.v1");
    hello->add_capabilities("heartbeat.v1");
    hello->add_capabilities("aoi.delta.v1");
    hello->add_capabilities("mailbox.changed.v1");
    hello->add_capabilities("session.replaced.v1");

    auto fail = [&](const char *code, const char *msg) {
        hello->set_ok(false);
        hello->set_error_code(code);
        hello->set_message(msg);
        FillPublicError(outer, code, msg, outer->seq(), conn_id);
        *outer->mutable_server_hello() = *hello;
    };

    if (req.protocol_version() < kMinSupportedProtocolVersion ||
        req.protocol_version() > kPublicProtocolVersion) {
        fail(kErrProtocolVersion, "protocol version not supported");
        return;
    }
    if (req.schema_sha256().empty() || req.schema_sha256() != schema) {
        fail(kErrSchemaMismatch, "schema hash mismatch");
        return;
    }
    if (ClientVersionTooOld(req.client_version())) {
        fail(kErrClientUpgradeRequired, "client upgrade required");
        return;
    }

    hello->set_ok(true);
    hello->set_error_code(kErrOk);
    hello->set_message("ok");
    outer->set_ok(true);
    outer->set_error_code(kErrOk);
    outer->set_message("ok");
    outer->set_retryable(false);
    outer->set_server_time_ms(hello->server_time_ms());
    if (outer->trace_id().empty())
        outer->set_trace_id(NewTraceId(conn_id, outer->seq()));
    *outer->mutable_server_hello() = *hello;
}

}  // namespace gameproto
