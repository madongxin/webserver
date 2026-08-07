#include "MessageRoute.h"

namespace gameproto {

bool IsWorldBoundRequest(const game::GameRequest &req) {
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
