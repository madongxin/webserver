#pragma once

#include "PlacementStore.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
#include "game.pb.h"
#endif

/** 分线列表：进图/切线回包、QueryMapLines、Prometheus。 */
namespace MapLineView {

uint32_t EffectiveRealm(uint32_t realm_id);

#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
void CopyLine(const MapLineInfo &src, game::MapLineInfo *dst);
bool FillLines(uint32_t realm_id, uint64_t map_template_id,
               google::protobuf::RepeatedPtrField<game::MapLineInfo> *out);
bool FillQueryRsp(uint32_t realm_id, uint64_t map_template_id, game::QueryMapLinesRsp *out);
#endif

/** Redis 权威分线/副本占用。owner_only 非空时只导出该 GameLogic 的实例。 */
std::string PrometheusText(const std::string &owner_only);

}  // namespace MapLineView
