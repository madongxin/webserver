#include "GameLogicBrpcServer.h"

#include "BrpcSslUtil.h"
#include "GameLogicForwardServiceImpl.h"
#include "GameLogicServiceImpl.h"
#include "GameMeshPaths.h"
#include "Logging.h"
#include "MapCatalog.h"
#include "MapInstanceRegistry.h"
#include "MapRuntime.h"
#include "PlayerSerialQueue.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "PlacementStore.h"
#endif

#include <brpc/server.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
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

bool ParseLogicConfig(const std::string &path, std::string *listen_addr, int *idle_timeout_sec,
                      std::string *instance_id, std::string *map_data_dir, uint32_t *capacity,
                      int *aoi_radius, int *tick_hz) {
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
        else if (key == "instance_id" && instance_id)
            *instance_id = val;
        else if (key == "map_data_dir" && map_data_dir)
            *map_data_dir = val;
        else if (key == "public_map_capacity" && capacity)
            *capacity = static_cast<uint32_t>(std::atoi(val.c_str()));
        else if (key == "aoi_view_radius_cells" && aoi_radius)
            *aoi_radius = std::atoi(val.c_str());
        else if (key == "map_tick_hz" && tick_hz)
            *tick_hz = std::atoi(val.c_str());
    }
    return !listen_addr->empty();
}

const std::string &LogicCnfPath() {
    static std::string path = "../config/gamelogic.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/gamelogic.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

}  // namespace

GameLogicBrpcServer &GameLogicBrpcServer::Instance() {
    static GameLogicBrpcServer g;
    return g;
}

GameLogicBrpcServer::~GameLogicBrpcServer() {
    Stop();
}

void GameLogicBrpcServer::LoadMapRuntimeFromConfig() {
    std::string addr = "0.0.0.0:8201";
    int idle = 30;
    std::string instance_id = "gl-0";
    std::string map_data_dir;
    uint32_t capacity = 50;
    int aoi_radius = 2;
    int tick_hz = 10;
    if (!ParseLogicConfig(LogicCnfPath(), &addr, &idle, &instance_id, &map_data_dir, &capacity,
                          &aoi_radius, &tick_hz))
        LOG_WARN << "GameLogicBrpcServer: map config defaults, cannot parse " << LogicCnfPath();
    (void)addr;
    (void)idle;
    (void)instance_id;
    if (!map_data_dir.empty())
        MapCatalog::Instance().SetMapDataDir(map_data_dir);
    MapCatalog::Instance().SetPublicMapCapacity(capacity);
    MapCatalog::Instance().SetAoiViewRadiusCells(aoi_radius);
    MapCatalog::Instance().SetMapTickHz(tick_hz);
    MapRuntime::Instance().SetViewRadiusCells(aoi_radius);
#ifdef WEBSERVER_ENABLE_REDIS
    PlacementStore::Instance().SetPublicMapCapacity(capacity);
#endif
    std::string merr;
    if (!MapCatalog::Instance().EnsureDefault(&merr))
        LOG_WARN << "MapCatalog load failed: " << merr;
}

bool GameLogicBrpcServer::StartFromConfig() {
    std::string addr = "0.0.0.0:8201";
    int idle = 30;
    std::string instance_id = "gl-0";
    std::string map_data_dir;
    uint32_t capacity = 50;
    int aoi_radius = 2;
    int tick_hz = 10;
    if (!ParseLogicConfig(LogicCnfPath(), &addr, &idle, &instance_id, &map_data_dir, &capacity,
                          &aoi_radius, &tick_hz))
        LOG_WARN << "GameLogicBrpcServer: use default listen, cannot parse " << LogicCnfPath();
    MapInstanceRegistry::Instance().SetLocalInstanceId(instance_id);
    LoadMapRuntimeFromConfig();
#ifdef WEBSERVER_ENABLE_BRPC
    // FormalMode.h 轻量；避免循环依赖时仅 env
#endif
    {
        const char *v = std::getenv("GAMEMESH_FORMAL");
        if (v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0))
            MapInstanceRegistry::Instance().SetRequireLease(true);
    }
    LOG_INFO << "GameLogicBrpcServer instance_id=" << instance_id
             << " require_lease=" << MapInstanceRegistry::Instance().require_lease();
    return Start(addr, idle);
}

bool GameLogicBrpcServer::Start(const std::string &listen_addr, int idle_timeout_sec) {
    if (running_)
        return true;

    PlayerSerialQueue::Instance().Start(0);

    server_.reset(new brpc::Server());
    service_.reset(new GameLogicForwardServiceImpl());
    gl_service_.reset(new GameLogicServiceImpl());
    if (server_->AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "GameLogicBrpcServer: AddService Forward failed";
        server_.reset();
        service_.reset();
        gl_service_.reset();
        return false;
    }
    if (server_->AddService(gl_service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "GameLogicBrpcServer: AddService GameLogicService failed";
        server_.reset();
        service_.reset();
        gl_service_.reset();
        return false;
    }
    brpc::ServerOptions options;
    options.idle_timeout_sec = idle_timeout_sec;
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::LoadFromCnf(LogicCnfPath(), &ssl);
    if (BrpcSslUtil::ApplyServer(&options, ssl))
        LOG_INFO << "GameLogicBrpcServer SSL enabled cert=" << ssl.cert;
    if (server_->Start(listen_addr.c_str(), &options) != 0) {
        LOG_ERROR << "GameLogicBrpcServer: Start failed addr=" << listen_addr;
        server_.reset();
        service_.reset();
        gl_service_.reset();
        return false;
    }
    listen_addr_ = listen_addr;
    running_ = true;
    LOG_INFO << "GameLogicBrpcServer listening on " << listen_addr_
             << " (GameLogicForward + GameLogicService)";
    return true;
}

void GameLogicBrpcServer::Stop() {
    if (!server_)
        return;
    // 先排空玩家串行队列，再停 brpc，避免 callback 在 Stop 后改状态
    PlayerSerialQueue::Instance().BeginDrain(std::chrono::milliseconds(3000));
    server_->Stop(0);
    server_->Join();
    PlayerSerialQueue::Instance().Stop();
    server_.reset();
    service_.reset();
    gl_service_.reset();
    running_ = false;
    LOG_INFO << "GameLogicBrpcServer stopped";
}
