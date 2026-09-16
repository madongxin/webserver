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

const google::protobuf::Message *InnerBody(const game::GameResponse &rsp) {
    const auto *refl = rsp.GetReflection();
    const auto *desc = rsp.GetDescriptor();
    const auto *oneof = desc->FindOneofByName("body");
    if (!oneof)
        return nullptr;
    const auto *field = refl->GetOneofFieldDescriptor(rsp, oneof);
    if (!field || field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
        return nullptr;
    return &refl->GetMessage(rsp, field);
}

google::protobuf::Message *MutableInnerBody(game::GameResponse *rsp) {
    if (!rsp)
        return nullptr;
    const auto *refl = rsp->GetReflection();
    const auto *desc = rsp->GetDescriptor();
    const auto *oneof = desc->FindOneofByName("body");
    if (!oneof)
        return nullptr;
    const auto *field = refl->GetOneofFieldDescriptor(*rsp, oneof);
    if (!field || field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
        return nullptr;
    return refl->MutableMessage(rsp, field);
}

std::string InnerStringField(const game::GameResponse &rsp, const char *name) {
    const auto *inner = InnerBody(rsp);
    if (!inner || !name)
        return {};
    const auto *f = inner->GetDescriptor()->FindFieldByName(name);
    if (!f || f->type() != google::protobuf::FieldDescriptor::TYPE_STRING)
        return {};
    return inner->GetReflection()->GetString(*inner, f);
}

void SetInnerStringField(game::GameResponse *rsp, const char *name, const std::string &value) {
    auto *inner = MutableInnerBody(rsp);
    if (!inner || !name)
        return;
    const auto *f = inner->GetDescriptor()->FindFieldByName(name);
    if (!f || f->type() != google::protobuf::FieldDescriptor::TYPE_STRING)
        return;
    inner->GetReflection()->SetString(inner, f, value);
}

std::string InnerErrorCode(const game::GameResponse &rsp) {
    return InnerStringField(rsp, "error_code");
}

std::string InnerErrorMessage(const game::GameResponse &rsp) {
    return InnerStringField(rsp, "message");
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
    if (ContainsFold(msg, "session not found"))
        return kErrSessionExpired;
    if (ContainsFold(msg, "map line not found"))
        return kErrMapNoLine;
    if (ContainsFold(msg, "map instance not found"))
        return kErrMapNotReady;
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
    if (ContainsFold(msg, "not bound") || ContainsFold(msg, "not on a line"))
        return kErrNotOnMap;
    if (ContainsFold(msg, "lease_expired") || ContainsFold(msg, "lease_missing") ||
        ContainsFold(msg, "lease expired") || ContainsFold(msg, "lease missing") ||
        ContainsFold(msg, "placement not ready"))
        return kErrMapNotReady;
    if (msg.empty())
        return {};
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
    if (code == "ERR_LEASE_EXPIRED" || code == "ERR_LEASE_MISSING" ||
        code == "ERR_PLACEMENT_NOT_READY" || code == "ERR_PLACEMENT_UNAVAILABLE" ||
        code == "ERR_PLACEMENT_REQUIRED")
        return kErrMapNotReady;
    if (code == "NOT_BOUND")
        return kErrNotOnMap;
    if (code == "NOT_FOUND")
        return {};  // 交给 Promote 按 message 区分 session / map，禁止裸 NOT_FOUND 出公网
    if (code == "FENCE_REJECT")
        return kErrFenceStale;
    if (code == "STALE_ROUTE")
        return kErrAoiResyncRequired;
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
    const std::string orig_code = rsp->error_code();
    const std::string inner_code = InnerErrorCode(*rsp);
    const std::string inner_msg = InnerErrorMessage(*rsp);
    std::string code = orig_code;
    if (code == "ERR_CLIENT_SEQ_OUT_OF_ORDER")
        code = kErrStaleSeq;
    if (code.empty())
        code = inner_code;
    code = NormalizePublicErrorCode(code);
    if (code.empty()) {
        code = GuessFromMessage(rsp->message());
        if (code.empty())
            code = GuessFromMessage(inner_msg);
    }
    if (code == "ERR_CLIENT_SEQ_OUT_OF_ORDER")
        code = kErrStaleSeq;
    if (code.empty()) {
        // 会话 Lua 的 NOT_FOUND 常无稳定公网码；缺线是 ERR_MAP_NO_LINE，不要把会话缺失当成缺线。
        if (ContainsFold(rsp->message(), "session not found") ||
            ContainsFold(inner_msg, "session not found") || orig_code == "NOT_FOUND" ||
            inner_code == "NOT_FOUND")
            code = kErrSessionExpired;
        else
            code = kErrInternal;
    }
    code = NormalizePublicErrorCode(code);
    rsp->set_error_code(code);
    rsp->set_retryable(ErrorCodeRetryable(code));
    if (rsp->message().empty() && !inner_msg.empty())
        rsp->set_message(SanitizePublicMessage(inner_msg));
    else
        rsp->set_message(SanitizePublicMessage(rsp->message().empty() ? code : rsp->message()));
    // Unity 读 EnterMapRsp.error_code；必须和信封一致，否则仍会把 NOT_FOUND 当缺线重试。
    SetInnerStringField(rsp, "error_code", code);
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
