#include "RedisClient.h"

#include <hiredis/hiredis.h>

#include <arpa/inet.h>
#include <cstring>
#include <vector>

RedisClient::~RedisClient() {
    Disconnect();
}

bool RedisClient::Connect(const std::string &host, int port, const std::string &password) {
    Disconnect();
    timeval tv{2, 0};
    ctx_ = redisConnectWithTimeout(host.c_str(), port, tv);
    if (!ctx_ || static_cast<redisContext *>(ctx_)->err) {
        Disconnect();
        return false;
    }
    auto *c = static_cast<redisContext *>(ctx_);
    if (!password.empty()) {
        redisReply *auth =
            static_cast<redisReply *>(redisCommand(c, "AUTH %s", password.c_str()));
        if (!CheckReply(auth, "AUTH")) {
            Disconnect();
            return false;
        }
    }
    return Ping();
}

void RedisClient::Disconnect() {
    if (ctx_) {
        redisFree(static_cast<redisContext *>(ctx_));
        ctx_ = nullptr;
    }
}

bool RedisClient::CheckReply(void *reply, const char *) {
    redisReply *r = static_cast<redisReply *>(reply);
    if (!r)
        return false;
    const bool ok = !(r->type == REDIS_REPLY_ERROR);
    freeReplyObject(r);
    return ok;
}

bool RedisClient::Ping() {
    if (!ctx_)
        return false;
    redisReply *r =
        static_cast<redisReply *>(redisCommand(static_cast<redisContext *>(ctx_), "PING"));
    if (!r)
        return false;
    const bool ok = r->type == REDIS_REPLY_STATUS && r->str && std::strcmp(r->str, "PONG") == 0;
    freeReplyObject(r);
    return ok;
}

bool RedisClient::Exists(const std::string &key) {
    if (!ctx_)
        return false;
    redisReply *r = static_cast<redisReply *>(
        redisCommand(static_cast<redisContext *>(ctx_), "EXISTS %s", key.c_str()));
    if (!r || r->type != REDIS_REPLY_INTEGER) {
        if (r)
            freeReplyObject(r);
        return false;
    }
    const bool exists = r->integer == 1;
    freeReplyObject(r);
    return exists;
}

bool RedisClient::Del(const std::string &key) {
    if (!ctx_)
        return false;
    redisReply *r = static_cast<redisReply *>(
        redisCommand(static_cast<redisContext *>(ctx_), "DEL %s", key.c_str()));
    return CheckReply(r, "DEL");
}

bool RedisClient::Expire(const std::string &key, int ttl_sec) {
    if (!ctx_ || ttl_sec <= 0)
        return false;
    redisReply *r = static_cast<redisReply *>(redisCommand(static_cast<redisContext *>(ctx_),
                                                             "EXPIRE %s %d", key.c_str(), ttl_sec));
    if (!r || r->type != REDIS_REPLY_INTEGER) {
        if (r)
            freeReplyObject(r);
        return false;
    }
    const bool ok = r->integer == 1;
    freeReplyObject(r);
    return ok;
}

bool RedisClient::HSet(const std::string &key, const std::map<std::string, std::string> &fields) {
    if (!ctx_ || fields.empty())
        return false;
    std::vector<const char *> argv;
    std::vector<size_t> argvlen;
    argv.push_back("HSET");
    argvlen.push_back(4);
    argv.push_back(key.c_str());
    argvlen.push_back(key.size());
    for (const auto &kv : fields) {
        argv.push_back(kv.first.c_str());
        argvlen.push_back(kv.first.size());
        argv.push_back(kv.second.c_str());
        argvlen.push_back(kv.second.size());
    }
    redisReply *r = static_cast<redisReply *>(redisCommandArgv(
        static_cast<redisContext *>(ctx_), static_cast<int>(argv.size()), argv.data(), argvlen.data()));
    return CheckReply(r, "HSET");
}

bool RedisClient::HGetAll(const std::string &key, std::map<std::string, std::string> *fields) {
    if (!ctx_ || !fields)
        return false;
    fields->clear();
    redisReply *r = static_cast<redisReply *>(
        redisCommand(static_cast<redisContext *>(ctx_), "HGETALL %s", key.c_str()));
    if (!r)
        return false;
    if (r->type == REDIS_REPLY_NIL) {
        freeReplyObject(r);
        return true;
    }
    if (r->type != REDIS_REPLY_ARRAY || r->elements % 2 != 0) {
        freeReplyObject(r);
        return false;
    }
    for (size_t i = 0; i < r->elements; i += 2) {
        redisReply *k = r->element[i];
        redisReply *v = r->element[i + 1];
        if (k->type == REDIS_REPLY_STRING && v->type == REDIS_REPLY_STRING)
            (*fields)[std::string(k->str, k->len)] = std::string(v->str, v->len);
    }
    freeReplyObject(r);
    return true;
}
