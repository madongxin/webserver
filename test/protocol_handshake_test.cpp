/**
 * S1：Hello 策略、schema 校验、统一错误码。
 */
#include "FormalMode.h"
#include "ProtocolHandshake.h"
#include "PublicError.h"
#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    }
}

}  // namespace

int main() {
    ::unsetenv("GAMEMESH_ALLOW_LEGACY_NO_HELLO");
    ::unsetenv("GAMEMESH_FORMAL");
    Expect(!ClientHelloRequired(), "non-formal default no hello");
    ::setenv("GAMEMESH_FORMAL", "1", 1);
    Expect(ClientHelloRequired(), "formal requires hello");
    ::setenv("GAMEMESH_ALLOW_LEGACY_NO_HELLO", "1", 1);
    Expect(!ClientHelloRequired(), "legacy switch");
    ::unsetenv("GAMEMESH_ALLOW_LEGACY_NO_HELLO");
    ::unsetenv("GAMEMESH_FORMAL");

    Expect(gameproto::ErrorCodeRetryable("ERR_RATE_LIMITED"), "retryable rate");
    Expect(!gameproto::ErrorCodeRetryable("ERR_UNAUTHENTICATED"), "unauth not retryable");
    Expect(gameproto::SanitizePublicMessage("mysql_query failed innodb") == "dependency unavailable",
           "sanitize mysql");

    const std::string schema = gameproto::EffectiveSchemaSha256();
    Expect(!schema.empty() && schema.size() == 64, "compiled schema sha256");

    game::ClientHelloReq okh;
    okh.set_protocol_version(1);
    okh.set_schema_sha256(schema);
    okh.set_client_version("1.0.0");
    game::ServerHelloRsp hello;
    game::GameResponse outer;
    outer.set_seq(1);
    gameproto::HandleClientHello(okh, 9, &hello, &outer);
    Expect(hello.ok() && outer.ok() && hello.error_code() == "OK", "hello ok");
    Expect(outer.error_code() == "OK", "outer ok code");
    Expect(hello.heartbeat_interval_ms() > 0 && hello.idle_timeout_ms() > 0, "timers");

    game::ClientHelloReq badv = okh;
    badv.set_protocol_version(99);
    hello.Clear();
    outer.Clear();
    gameproto::HandleClientHello(badv, 9, &hello, &outer);
    Expect(!hello.ok() && hello.error_code() == "ERR_PROTOCOL_VERSION", "bad version");
    Expect(outer.error_code() == "ERR_PROTOCOL_VERSION", "outer version");
    Expect(hello.schema_sha256().size() == 64, "server hash present");

    game::ClientHelloReq badh = okh;
    badh.set_schema_sha256("deadbeef");
    hello.Clear();
    outer.Clear();
    gameproto::HandleClientHello(badh, 9, &hello, &outer);
    Expect(!hello.ok() && hello.error_code() == "ERR_SCHEMA_MISMATCH", "bad schema");
    Expect(hello.schema_sha256() == schema, "mismatch carries server hash");

    ::setenv("GAMEMESH_MIN_CLIENT_VERSION", "2.0.0", 1);
    game::ClientHelloReq oldc = okh;
    oldc.set_client_version("1.0.0");
    hello.Clear();
    outer.Clear();
    gameproto::HandleClientHello(oldc, 9, &hello, &outer);
    Expect(!hello.ok() && hello.error_code() == "ERR_CLIENT_UPGRADE_REQUIRED", "upgrade");
    ::unsetenv("GAMEMESH_MIN_CLIENT_VERSION");

    game::GameResponse leak;
    leak.set_ok(false);
    leak.set_message("MySQL update failed: Duplicate entry");
    gameproto::PromotePublicError(&leak, 1);
    Expect(leak.error_code() == "ERR_DEPENDENCY_UNAVAILABLE", "promote mysql");
    Expect(leak.message() == "dependency unavailable", "message sanitized");
    Expect(leak.retryable(), "dep retryable");

    game::GameResponse cred;
    cred.set_ok(false);
    cred.set_message("invalid credential");
    gameproto::PromotePublicError(&cred, 1);
    Expect(cred.error_code() == std::string(gameproto::kErrBadCredential), "guess credential");

    game::GameResponse pw;
    pw.set_ok(false);
    pw.set_message("device_id and password(>=6) required");
    gameproto::PromotePublicError(&pw, 1);
    Expect(pw.error_code() == std::string(gameproto::kErrInvalidArgument), "guess password arg");

    game::GameResponse mapped;
    mapped.set_ok(false);
    mapped.set_error_code("BAD_CREDENTIAL");
    mapped.set_message("invalid credential");
    gameproto::PromotePublicError(&mapped, 1);
    Expect(mapped.error_code() == std::string(gameproto::kErrBadCredential), "normalize BAD_CREDENTIAL");

    if (fails) {
        std::printf("protocol_handshake_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK protocol_handshake_test schema=%s\n", schema.c_str());
    return 0;
}
