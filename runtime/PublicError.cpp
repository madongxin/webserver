#include "PublicError.h"

#include "OpsMetrics.h"
#include "ProtoFraming.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace gameproto {
namespace {

std::atomic<uint64_t> g_trace_seq{1};

bool ContainsFold(const std::string &hay, const char *needle) {
    if (!needle || !*needle)
        return false;
    const size_t n = std::strlen(needle);
    if (hay.size() < n)
        return false;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            const unsigned char a = static_cast<unsigned char>(hay[i + j]);
            const unsigned char b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                ok = false;
                break;
            }
        }
        if (ok)
            return true;
    }
    return false;
}

std::string InnerErrorCode(const game::GameResponse &rsp) {
    const auto *refl = rsp.GetReflection();
    const auto *desc = rsp.GetDescriptor();
    const auto *oneof = desc->FindOneofByName("body");
    if (!oneof)
        return {};
    const auto *field = refl->GetOneofFieldDescriptor(rsp, oneof);
    if (!field || field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
        return {};
    const auto &inner = refl->GetMessage(rsp, field);
    const auto *ecode = inner.GetDescriptor()->FindFieldByName("error_code");
    if (!ecode || ecode->type() != google::protobuf::FieldDescriptor::TYPE_STRING)
        return {};
    return inner.GetReflection()->GetString(inner, ecode);
}

std::string GuessFromMessage(const std::string &msg) {
    if (msg.size() >= 4 && msg.compare(0, 4, "ERR_") == 0)
        return msg;
    if (msg == "ERR_OVERLOAD" || msg == "ERR_OVERLOADED")
        return kErrOverloaded;
    if (ContainsFold(msg, "invalid credential") || ContainsFold(msg, "bad_credential") ||
        ContainsFold(msg, "bad credential"))
        return kErrBadCredential;
    if (ContainsFold(msg, "account not registered") || ContainsFold(msg, "account not found"))
        return kErrAccountNotFound;
    if (ContainsFold(msg, "banned"))
        return kErrBanned;
    if ((ContainsFold(msg, "password") &&
         (ContainsFold(msg, "required") || ContainsFold(msg, ">=6"))) ||
        ContainsFold(msg, "invalid login payload") || ContainsFold(msg, "invalid register payload") ||
        ContainsFold(msg, "invalid_arg"))
        return kErrInvalidArgument;
    if (ContainsFold(msg, "unauthenticated") || ContainsFold(msg, "hello required"))
        return kErrUnauthenticated;
    if (ContainsFold(msg, "not ready") || ContainsFold(msg, "no logic assigned") ||
        ContainsFold(msg, "mysql") || ContainsFold(msg, "redis") || ContainsFold(msg, "brpc") ||
        ContainsFold(msg, "hiredis") || ContainsFold(msg, "innodb"))
        return kErrDependencyUnavailable;
    if (ContainsFold(msg, "overload") || ContainsFold(msg, "overloaded"))
        return kErrOverloaded;
    if (ContainsFold(msg, "rate limit"))
        return kErrRateLimited;
    if (ContainsFold(msg, "fence"))
        return kErrFenceStale;
    if (msg.empty())
        return kErrInternal;
    return kErrInternal;
}

std::string NormalizePublicErrorCode(const std::string &code) {
    if (code.empty() || code == "OK")
        return code;
    if (code == "INVALID_ARG" || code == "HASH_FAILED" || code == "PASSWORD_REQUIRED")
        return kErrInvalidArgument;
    if (code == "BAD_CREDENTIAL")
        return kErrBadCredential;
    if (code == "ACCOUNT_NOT_FOUND")
        return kErrAccountNotFound;
    if (code == "BANNED")
        return kErrBanned;
    if (code == "ACCOUNT_LOOKUP_FAILED" || code == "GAMEDB_REQUIRED" || code == "REGISTER_FAILED")
        return kErrDependencyUnavailable;
    if (code == "ERR_OVERLOAD")
        return kErrOverloaded;
    return code;
}

}  // namespace

bool ErrorCodeRetryable(const std::string &code) {
    return code == kErrRateLimited || code == kErrOverloaded || code == kErrDependencyUnavailable ||
           code == kErrAoiResyncRequired;
}

int64_t PublicNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string NewTraceId(uint64_t conn_id, uint64_t seq) {
    char buf[64];
    const uint64_t n = g_trace_seq.fetch_add(1, std::memory_order_relaxed);
    std::snprintf(buf, sizeof(buf), "gw-%llx-%llx-%llx",
                  static_cast<unsigned long long>(conn_id), static_cast<unsigned long long>(seq),
                  static_cast<unsigned long long>(n));
    return buf;
}

std::string SanitizePublicMessage(const std::string &raw) {
    if (ContainsFold(raw, "mysql") || ContainsFold(raw, "redis") || ContainsFold(raw, "brpc") ||
        ContainsFold(raw, "hiredis") || ContainsFold(raw, "innodb") || ContainsFold(raw, "sqlstate"))
        return "dependency unavailable";
    if (raw.size() > 256)
        return raw.substr(0, 256);
    return raw;
}

void FillPublicError(game::GameResponse *rsp, const char *error_code, const char *safe_message,
                     uint64_t seq, uint64_t conn_id) {
    if (!rsp)
        return;
    const char *code = error_code && *error_code ? error_code : kErrInternal;
    rsp->set_seq(seq);
    rsp->set_ok(false);
    rsp->set_error_code(code);
    rsp->set_message(SanitizePublicMessage(safe_message ? safe_message : code));
    rsp->set_retryable(ErrorCodeRetryable(code));
    rsp->set_server_time_ms(PublicNowMs());
    if (rsp->trace_id().empty())
        rsp->set_trace_id(NewTraceId(conn_id, seq));
    OpsMetrics::Instance().IncErrorCode(code);
}

void PromotePublicError(game::GameResponse *rsp, uint64_t conn_id) {
    if (!rsp)
        return;
    rsp->set_server_time_ms(PublicNowMs());
    if (rsp->trace_id().empty())
        rsp->set_trace_id(NewTraceId(conn_id, rsp->seq()));
    if (rsp->ok()) {
        if (rsp->error_code().empty())
            rsp->set_error_code(kErrOk);
        rsp->set_retryable(false);
        rsp->set_message(SanitizePublicMessage(rsp->message()));
        return;
    }
    std::string code = rsp->error_code();
    if (code == "ERR_CLIENT_SEQ_OUT_OF_ORDER")
        code = kErrStaleSeq;
    if (code.empty())
        code = InnerErrorCode(*rsp);
    code = NormalizePublicErrorCode(code);
    if (code.empty())
        code = GuessFromMessage(rsp->message());
    if (code == "ERR_CLIENT_SEQ_OUT_OF_ORDER")
        code = kErrStaleSeq;
    if (code.empty())
        code = kErrInternal;
    code = NormalizePublicErrorCode(code);
    rsp->set_error_code(code);
    rsp->set_retryable(ErrorCodeRetryable(code));
    rsp->set_message(SanitizePublicMessage(rsp->message().empty() ? code : rsp->message()));
    OpsMetrics::Instance().IncErrorCode(code);
}

bool EncodePublicErrorFrame(const char *error_code, const char *safe_message, uint64_t seq,
                            uint64_t conn_id, std::string *out_frame) {
    if (!out_frame)
        return false;
    game::GameResponse rsp;
    FillPublicError(&rsp, error_code, safe_message, seq, conn_id);
    std::string body;
    if (!rsp.SerializeToString(&body))
        return false;
    return EncodeFrame(body, out_frame);
}

}  // namespace gameproto
