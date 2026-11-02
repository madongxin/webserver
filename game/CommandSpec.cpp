#include "CommandSpec.h"

namespace gameproto {

namespace {

const CommandSpec kTable[] = {
    {game::GameRequest::kConsumeItem, false, false, false, true, false, true},
    {game::GameRequest::kReleaseSkill, false, false, false, true, false, true},
    {game::GameRequest::kGrantItem, false, false, false, false, true, true},
    {game::GameRequest::kLogin, false, false, true, false, false, false},
    {game::GameRequest::kValidateSession, false, false, false, false, true, false},
    {game::GameRequest::kCheckOnline, false, false, false, false, true, false},
    {game::GameRequest::kLogout, false, false, false, true, false, false},
    {game::GameRequest::kReconnect, false, false, true, false, false, false},
    {game::GameRequest::kRegister, false, false, true, false, false, false},
    {game::GameRequest::kFlushBag, false, false, false, false, true, false},
    {game::GameRequest::kMailboxSummary, true, true, false, true, false, true},
    {game::GameRequest::kMailList, true, true, false, true, false, true},
    {game::GameRequest::kMailGet, true, true, false, true, false, true},
    {game::GameRequest::kMailRead, true, true, false, true, false, true},
    {game::GameRequest::kMailClaim, true, true, false, true, false, true},
    {game::GameRequest::kMailBatchClaim, true, true, false, true, false, true},
    {game::GameRequest::kMailFavorite, true, true, false, true, false, true},
    {game::GameRequest::kMailBatchRead, true, true, false, true, false, true},
    {game::GameRequest::kMailBatchDelete, true, true, false, true, false, true},
    {game::GameRequest::kMailDeliver, true, true, false, false, true, true},
    {game::GameRequest::kEnterMap, false, false, false, true, false, true},
    {game::GameRequest::kLeaveMap, false, false, false, true, false, true},
    {game::GameRequest::kMapPing, false, false, false, true, false, true},
    {game::GameRequest::kChatSend, true, false, false, true, false, true},
    {game::GameRequest::kFriendList, true, false, false, true, false, true},
    {game::GameRequest::kPushAck, false, false, false, true, false, false},
    {game::GameRequest::kGetSelfProfile, false, false, false, true, false, true},
    {game::GameRequest::kMove, false, false, false, true, false, true},
    {game::GameRequest::kPlayerMailSend, true, true, false, true, false, true},
    {game::GameRequest::kClientHello, false, false, true, false, false, false},
    {game::GameRequest::kHeartbeat, false, false, true, true, false, false},
    {game::GameRequest::kWorldSnapshot, false, false, false, true, false, true},
    {game::GameRequest::kRespawn, false, false, false, true, false, true},
    {game::GameRequest::kGetPlayerBrief, true, false, false, true, false, true},
    {game::GameRequest::kQueryOnlineState, true, false, false, true, false, true},
    {game::GameRequest::kQueryMapLines, false, false, false, true, false, false},
    {game::GameRequest::kCreateDungeon, false, false, false, true, false, false},
    {game::GameRequest::kSwitchLine, false, false, false, true, false, false},
    {game::GameRequest::kEnqueueMap, false, false, false, true, false, false},
    {game::GameRequest::kInteractPortal, false, false, false, true, false, true},
    {game::GameRequest::kFriendSearch, true, false, false, true, false, true},
    {game::GameRequest::kFriendApply, true, false, false, true, false, true},
    {game::GameRequest::kFriendAccept, true, false, false, true, false, true},
    {game::GameRequest::kFriendReject, true, false, false, true, false, true},
    {game::GameRequest::kFriendDelete, true, false, false, true, false, true},
    {game::GameRequest::kFriendRequestList, true, false, false, true, false, true},
    {game::GameRequest::kFriendBlock, true, false, false, true, false, true},
    {game::GameRequest::kFriendUnblock, true, false, false, true, false, true},
    {game::GameRequest::kFriendBlockList, true, false, false, true, false, true},
};

}  // namespace

const CommandSpec *FindCommandSpec(game::GameRequest::BodyCase body) {
    for (const CommandSpec &s : kTable) {
        if (s.body == body)
            return &s;
    }
    return nullptr;
}

const CommandSpec *CommandSpecTable(int *count) {
    if (count)
        *count = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
    return kTable;
}

}  // namespace gameproto
