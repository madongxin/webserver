/**
 * @file BrpcGameServer.cpp
 * @brief brpc Server 启动 / 停止
 */

#include "BrpcGameServer.h"

#include "BindAddressPolicy.h"
#include "BrpcConfigPath.h"
#include "FormalMode.h"
#include "Logging.h"
#include "MailBrpcServiceImpl.h"

#include <brpc/server.h>

#include <cstdlib>
#include <fstream>
#include <string>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool ParseConfig(const std::string &path, std::string *listen_addr, int *idle_timeout_sec,
                 bool *allow_public_bind) {
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (key == "listen_addr")
            *listen_addr = val;
        else if (key == "idle_timeout_sec")
            *idle_timeout_sec = std::atoi(val.c_str());
        else if (key == "allow_public_bind" && allow_public_bind)
            *allow_public_bind = ConfigFlagTrue(val);
    }
    return !listen_addr->empty();
}

}  // namespace

BrpcGameServer &BrpcGameServer::Instance() {
    static BrpcGameServer g;
    return g;
}

BrpcGameServer::~BrpcGameServer() {
    Stop();
}

bool BrpcGameServer::StartFromConfig() {
    std::string addr = "127.0.0.1:8181";
    int idle = 30;
    bool allow_public = AllowPublicBindFromEnv();
    const std::string &cnf = BrpcConfigPath::Cnf();
    if (!ParseConfig(cnf, &addr, &idle, &allow_public))
        LOG_WARN << "BrpcGameServer: use default listen_addr, cannot parse " << cnf;
    const ListenBindDecision bind = EvaluateListenBind(addr, FormalModeEnabled(), allow_public);
    if (!bind.ok) {
        LOG_ERROR << "BrpcGameServer: " << bind.error;
        return false;
    }
    return Start(addr, idle, allow_public);
}

bool BrpcGameServer::Start(const std::string &listen_addr, int idle_timeout_sec,
                           bool allow_public_bind) {
    const ListenBindDecision bind =
        EvaluateListenBind(listen_addr, FormalModeEnabled(),
                           allow_public_bind || AllowPublicBindFromEnv());
    if (!bind.ok) {
        LOG_ERROR << "BrpcGameServer: " << bind.error;
        return false;
    }
    if (running_) {
        LOG_WARN << "BrpcGameServer already running at " << listen_addr_;
        return true;
    }
    server_.reset(new brpc::Server());
    service_.reset(new MailBrpcServiceImpl());

    // SERVER_DOESNT_OWN_SERVICE：由我们持有 service_ 生命周期
    if (server_->AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "BrpcGameServer: AddService failed";
        server_.reset();
        service_.reset();
        return false;
    }

    brpc::ServerOptions options;
    options.idle_timeout_sec = idle_timeout_sec;
    if (server_->Start(listen_addr.c_str(), &options) != 0) {
        LOG_ERROR << "BrpcGameServer: Start failed addr=" << listen_addr;
        server_.reset();
        service_.reset();
        return false;
    }

    listen_addr_ = listen_addr;
    running_ = true;
    LOG_INFO << "BrpcGameServer listening on " << listen_addr_
             << " (MailBrpcService dual-stack with GameTcpGateway)";
    return true;
}

void BrpcGameServer::Stop() {
    if (!server_)
        return;
    server_->Stop(0);
    server_->Join();
    server_.reset();
    service_.reset();
    running_ = false;
    LOG_INFO << "BrpcGameServer stopped";
}
