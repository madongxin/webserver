#pragma once

#include <string>

struct LaunchOpts {
    std::string role = "all";
    int http_port = 8080;
    int game_port = 0;              // gateway/all 玩家 TCP；0=默认 http+1
    int logic_port_override = -1;   // gamelogic listen 覆盖
    std::string log_basename = "server";
    /** 若非空，跳过 argv role 解析，强制该 role（多二进制入口用） */
    std::string force_role;
};

/** 解析 server / 角色二进制命令行；失败时已打印 usage，返回 false */
bool ParseLaunchArgs(int argc, char *argv[], LaunchOpts *opts);

/** 按 role 拉起 HTTP(/metrics) + 对应 brpc/TCP；阻塞在 EventLoop */
int RunServer(const LaunchOpts &opts);
