#pragma once

#include <cstddef>

/**
 * GameLogic 健康快照刷新：RPC 前与周期刷新共用同一三态语义。
 *
 * - Registry 未就绪：保留启动静态配置 / 最后快照
 * - Discover 失败：保留快照，打错误日志与指标
 * - Discover 成功非空：原子替换 Session/Placement 健康列表
 * - Discover 成功零实例：若从未成功替换过且仍有启动静态快照则保留；
 *   一旦有过非空健康集，再空列表则清空（新登录/新地图 fail-closed）
 */
enum class HealthyLogicRefreshStatus {
    kNotReady = 0,
    kDiscoverFailed = 1,
    kApplied = 2,
};

struct HealthyLogicRefreshResult {
    HealthyLogicRefreshStatus status = HealthyLogicRefreshStatus::kNotReady;
    /** Discover 成功时的实例 id（可为空） */
    std::size_t instance_count = 0;
};

HealthyLogicRefreshResult RefreshHealthyLogicOwners(bool update_static_addrs = false);
