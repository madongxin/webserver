#include "MailTypes.h"

namespace mail {

bool CanTransitVisible(VisibleState from, VisibleState to) {
    if (from == to)
        return true;
    if (from != VisibleState::kActive)
        return false;
    return to == VisibleState::kSoftDeleted || to == VisibleState::kExpired ||
           to == VisibleState::kRevoked;
}

bool CanTransitAttachment(AttachmentState from, AttachmentState to) {
    if (from == to)
        return true;
    if (from == AttachmentState::kNone && to == AttachmentState::kUnclaimed)
        return true;
    if (from == AttachmentState::kUnclaimed && to == AttachmentState::kClaiming)
        return true;
    if (from == AttachmentState::kClaiming &&
        (to == AttachmentState::kClaimed || to == AttachmentState::kUnclaimed))
        return true;
    // 预留扩展路径
    if (from == AttachmentState::kUnclaimed && to == AttachmentState::kDestroyed)
        return true;
    if (from == AttachmentState::kClaimed && to == AttachmentState::kReturned)
        return true;
    return false;
}

}  // namespace mail
