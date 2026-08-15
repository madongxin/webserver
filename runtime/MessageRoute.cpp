#include "MessageRoute.h"

namespace gameproto {

bool IsMailBoundRequest(const game::GameRequest &req) {
    switch (req.body_case()) {
    case game::GameRequest::kMailboxSummary:
    case game::GameRequest::kMailList:
    case game::GameRequest::kMailGet:
    case game::GameRequest::kMailRead:
    case game::GameRequest::kMailClaim:
    case game::GameRequest::kMailBatchClaim:
    case game::GameRequest::kMailFavorite:
    case game::GameRequest::kMailBatchRead:
    case game::GameRequest::kMailBatchDelete:
    case game::GameRequest::kMailDeliver:
    case game::GameRequest::kPlayerMailSend:
        return true;
    default:
        return false;
    }
}

bool IsWorldBoundRequest(const game::GameRequest &req) {
    if (IsMailBoundRequest(req))
        return true;
    switch (req.body_case()) {
    case game::GameRequest::kChatSend:
    case game::GameRequest::kFriendList:
        return true;
    default:
        return false;
    }
}

bool IsLogicBoundRequest(const game::GameRequest &req) {
    return !IsWorldBoundRequest(req);
}

}  // namespace gameproto
