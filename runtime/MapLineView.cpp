#include "MapLineView.h"

#include "MapCatalog.h"
#include "SceneKind.h"

#include <cstdio>
#include <sstream>
#include <unordered_map>

namespace MapLineView {

uint32_t EffectiveRealm(uint32_t realm_id) {
    return realm_id == 0 ? 1 : realm_id;
}

#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
void CopyLine(const MapLineInfo &src, game::MapLineInfo *dst) {
    if (!dst)
        return;
    dst->set_line_no(src.line_no);
    dst->set_map_instance_id(src.map_instance_id);
    dst->set_occupancy(src.occupancy);
    dst->set_soft_cap(src.soft_cap);
    dst->set_hard_cap(src.hard_cap);
    dst->set_state(src.state);
    dst->set_owner_logic_server_id(src.owner_logic_server_id);
}

bool FillLines(uint32_t realm_id, uint64_t map_template_id,
               google::protobuf::RepeatedPtrField<game::MapLineInfo> *out) {
    if (!out || map_template_id == 0)
        return false;
    out->Clear();
    if (!PlacementStore::Instance().Available())
        return false;
    std::vector<MapLineInfo> rows;
    if (!PlacementStore::Instance().ListLines(EffectiveRealm(realm_id), map_template_id, &rows))
        return false;
    for (const auto &r : rows)
        CopyLine(r, out->Add());
    return true;
}

bool FillQueryRsp(uint32_t realm_id, uint64_t map_template_id, game::QueryMapLinesRsp *out) {
    if (!out)
        return false;
    out->Clear();
    out->set_map_template_id(map_template_id);
    MapScenePolicy pol;
    if (MapCatalog::Instance().GetScenePolicy(map_template_id, &pol))
        out->set_kind(SceneKindToString(pol.kind));
    else
        out->set_kind("LEGACY_POOL");
    if (!FillLines(realm_id, map_template_id, out->mutable_lines())) {
        out->set_ok(false);
        out->set_message("list lines failed");
        out->set_error_code("UNAVAILABLE");
        return false;
    }
    out->set_ok(true);
    out->set_message("ok");
    return true;
}
#endif

namespace {

std::string PromEscape(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '"')
            o.push_back('\\');
        if (c != '\n')
            o.push_back(c);
    }
    return o;
}

}  // namespace

std::string PrometheusText(const std::string &owner_only) {
    if (!PlacementStore::Instance().Available())
        return "";
    std::ostringstream os;
    os << "# HELP gamemesh_map_line_occupancy Players reserved on a LINE instance.\n"
          "# TYPE gamemesh_map_line_occupancy gauge\n";
    std::ostringstream count_os;
    count_os << "# HELP gamemesh_map_line_count Ready LINE instances per owner/template.\n"
                "# TYPE gamemesh_map_line_count gauge\n";
    bool any = false;
    std::unordered_map<std::string, uint32_t> counts;
    const auto entries = MapCatalog::Instance().ManifestEntries();
    for (const auto &e : entries) {
        if (e.policy.kind != SceneKind::Line)
            continue;
        std::vector<MapLineInfo> rows;
        if (!PlacementStore::Instance().ListLines(1, e.map_template_id, &rows))
            continue;
        for (const auto &r : rows) {
            if (!owner_only.empty() && r.owner_logic_server_id != owner_only)
                continue;
            any = true;
            const std::string owner =
                r.owner_logic_server_id.empty() ? "unknown" : r.owner_logic_server_id;
            os << "gamemesh_map_line_occupancy{map_template_id=\"" << e.map_template_id
               << "\",line_no=\"" << r.line_no << "\",owner=\"" << PromEscape(owner)
               << "\",kind=\"LINE\",state=\"" << PromEscape(r.state) << "\"} " << r.occupancy
               << "\n";
            const std::string ck = owner + "\t" + std::to_string(e.map_template_id);
            counts[ck] += 1;
        }
    }
    if (!any)
        return "";
    os << "\n";
    for (const auto &kv : counts) {
        const auto tab = kv.first.find('\t');
        const std::string owner = kv.first.substr(0, tab);
        const std::string tpl = tab == std::string::npos ? "0" : kv.first.substr(tab + 1);
        count_os << "gamemesh_map_line_count{map_template_id=\"" << tpl << "\",owner=\""
                 << PromEscape(owner) << "\",kind=\"LINE\"} " << kv.second << "\n";
    }
    count_os << "\n";
    os << count_os.str();
    return os.str();
}

}  // namespace MapLineView
