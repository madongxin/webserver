#pragma once

/**
 * @file MailTypes.h
 * @brief 邮件枚举、错误码与状态机校验（无外部框架依赖）
 */

#include <cstdint>
#include <string>
#include <vector>

namespace mail {

// ---------- 错误码（写入 Rsp.error_code，message 为人读文案）----------
namespace err {
constexpr const char *kOk = "OK";
constexpr const char *kMailNotFound = "MAIL_NOT_FOUND";
constexpr const char *kMailExpired = "MAIL_EXPIRED";
constexpr const char *kMailRevoked = "MAIL_REVOKED";
constexpr const char *kAlreadyClaimed = "ALREADY_CLAIMED";
constexpr const char *kClaimInProgress = "CLAIM_IN_PROGRESS";
constexpr const char *kInventoryFull = "INVENTORY_FULL";
constexpr const char *kCurrencyCap = "CURRENCY_CAP";
constexpr const char *kInvalidAttachment = "INVALID_ATTACHMENT";
constexpr const char *kIdempotencyConflict = "IDEMPOTENCY_CONFLICT";
constexpr const char *kMailboxFull = "MAILBOX_FULL";
constexpr const char *kRateLimited = "RATE_LIMITED";
constexpr const char *kAssetTransactionFailed = "ASSET_TRANSACTION_FAILED";
constexpr const char *kPermissionDenied = "PERMISSION_DENIED";
constexpr const char *kInvalidState = "INVALID_STATE";
constexpr const char *kFavoriteLimit = "FAVORITE_LIMIT";
constexpr const char *kHasUnclaimed = "HAS_UNCLAIMED_ATTACHMENT";
constexpr const char *kInternal = "INTERNAL_ERROR";
constexpr const char *kInvalidArgument = "INVALID_ARGUMENT";
constexpr const char *kServerStopping = "SERVER_STOPPING";
constexpr const char *kStateSyncRequired = "STATE_SYNC_REQUIRED";
}  // namespace err

enum class OwnerScope { kRole, kAccount };
enum class Category { kSystem, kActivity, kSocial, kTrade };
enum class VisibleState { kActive, kSoftDeleted, kExpired, kRevoked };
enum class ReadState { kUnread, kRead };
enum class AttachmentState {
    kNone,
    kUnclaimed,
    kClaiming,
    kClaimed,
    kPartial,
    kReturned,
    kDestroyed
};

inline const char *ToString(OwnerScope s) {
    return s == OwnerScope::kAccount ? "ACCOUNT" : "ROLE";
}
inline const char *ToString(Category c) {
    switch (c) {
    case Category::kActivity:
        return "ACTIVITY";
    case Category::kSocial:
        return "SOCIAL";
    case Category::kTrade:
        return "TRADE";
    default:
        return "SYSTEM";
    }
}
inline const char *ToString(VisibleState s) {
    switch (s) {
    case VisibleState::kSoftDeleted:
        return "SOFT_DELETED";
    case VisibleState::kExpired:
        return "EXPIRED";
    case VisibleState::kRevoked:
        return "REVOKED";
    default:
        return "ACTIVE";
    }
}
inline const char *ToString(ReadState s) {
    return s == ReadState::kRead ? "READ" : "UNREAD";
}
inline const char *ToString(AttachmentState s) {
    switch (s) {
    case AttachmentState::kUnclaimed:
        return "UNCLAIMED";
    case AttachmentState::kClaiming:
        return "CLAIMING";
    case AttachmentState::kClaimed:
        return "CLAIMED";
    case AttachmentState::kPartial:
        return "PARTIAL";
    case AttachmentState::kReturned:
        return "RETURNED";
    case AttachmentState::kDestroyed:
        return "DESTROYED";
    default:
        return "NONE";
    }
}

inline bool ParseCategory(const std::string &s, Category *out) {
    if (!out)
        return false;
    if (s == "SYSTEM" || s.empty()) {
        *out = Category::kSystem;
        return true;
    }
    if (s == "ACTIVITY") {
        *out = Category::kActivity;
        return true;
    }
    if (s == "SOCIAL") {
        *out = Category::kSocial;
        return true;
    }
    if (s == "TRADE") {
        *out = Category::kTrade;
        return true;
    }
    return false;
}

inline VisibleState ParseVisibleState(const std::string &s) {
    if (s == "SOFT_DELETED")
        return VisibleState::kSoftDeleted;
    if (s == "EXPIRED")
        return VisibleState::kExpired;
    if (s == "REVOKED")
        return VisibleState::kRevoked;
    return VisibleState::kActive;
}

inline ReadState ParseReadState(const std::string &s) {
    return s == "READ" ? ReadState::kRead : ReadState::kUnread;
}

inline AttachmentState ParseAttachmentState(const std::string &s) {
    if (s == "UNCLAIMED")
        return AttachmentState::kUnclaimed;
    if (s == "CLAIMING")
        return AttachmentState::kClaiming;
    if (s == "CLAIMED")
        return AttachmentState::kClaimed;
    if (s == "PARTIAL")
        return AttachmentState::kPartial;
    if (s == "RETURNED")
        return AttachmentState::kReturned;
    if (s == "DESTROYED")
        return AttachmentState::kDestroyed;
    return AttachmentState::kNone;
}

/** 可见状态合法转换；相同状态视为合法（幂等） */
bool CanTransitVisible(VisibleState from, VisibleState to);

/** 附件状态合法转换（首期主路径：NONE/UNCLAIMED/CLAIMING/CLAIMED） */
bool CanTransitAttachment(AttachmentState from, AttachmentState to);

/** now_utc_sec >= expire_at 视为过期 */
inline bool IsExpiredAt(int64_t expire_at, int64_t now_utc_sec) {
    return expire_at > 0 && now_utc_sec >= expire_at;
}

struct MailAttachmentRow {
    uint64_t attachment_id = 0;
    uint64_t mail_id = 0;
    int slot_index = 0;
    std::string asset_type = "ITEM";
    uint64_t asset_id = 0;
    uint32_t count = 0;
    std::string bind_type = "NONE";
    std::string payload;
    std::string claim_state = "UNCLAIMED";
    std::string asset_transaction_id;
    int64_t claimed_at = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

struct MailInstanceRow {
    uint64_t mail_id = 0;
    std::string owner_scope = "ROLE";
    uint64_t receiver_id = 0;
    std::string sender_type = "SYSTEM";
    uint64_t sender_id = 0;
    std::string source_system;
    std::string business_key;
    std::string template_id;
    int template_version = 1;
    std::string category = "SYSTEM";
    int priority = 0;
    std::string sender_name_snapshot;
    std::string title_snapshot;
    std::string body_snapshot;
    std::string read_state = "UNREAD";
    int64_t read_at = 0;
    std::string visible_state = "ACTIVE";
    bool has_attachment = false;
    std::string attachment_state = "NONE";
    bool is_favorite = false;
    int64_t sent_at = 0;
    int64_t expire_at = 0;
    int64_t deleted_at = 0;
    int64_t row_version = 1;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

struct DeliverAttachment {
    std::string asset_type = "ITEM";
    uint64_t asset_id = 0;
    uint32_t count = 0;
    std::string bind_type = "NONE";
    std::string payload;
};

struct DeliverRequest {
    std::string source_system;
    std::string business_key;
    std::string receiver_type = "ROLE";  // ROLE|ACCOUNT；首期仅 ROLE
    uint64_t receiver_id = 0;
    std::string template_id;
    int template_version = 1;
    std::string category = "SYSTEM";
    int priority = 0;
    std::string sender_name = "System";
    std::string sender_type = "SYSTEM";
    uint64_t sender_id = 0;
    std::string title;
    std::string body;
    std::vector<DeliverAttachment> attachments;
    int64_t send_at = 0;    // 0 = now
    int64_t expire_at = 0;  // 0 = now + default_expire_days
    std::string trace_id;
};

}  // namespace mail
