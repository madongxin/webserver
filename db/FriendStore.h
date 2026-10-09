#pragma once

#include "gamedb.pb.h"

#include <string>

/** GameDB 侧好友事务仓储。仅 GameDB 进程访问 MySQL。 */
class FriendStore {
public:
    static FriendStore &Instance();

    bool EnsureTables();
    void Execute(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    /** 把已过期的 PENDING 标成 EXPIRED，单轮最多 limit 行。 */
    int ExpireStaleRequests(int limit);

private:
    FriendStore() = default;

    void ListFriends(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    void Search(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    void Apply(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    void RequestList(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    void Accept(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    void Reject(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    void DeleteFriend(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    void Block(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    void BlockGate(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    void Unblock(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
    void BlockList(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp);
};
