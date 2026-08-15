#include "MapCatalog.h"

#include "GameMeshPaths.h"
#include "Logging.h"

#include <json/json.h>

#include <fstream>
#include <memory>
#include <sstream>

namespace {

std::string ReadAll(const std::string &path) {
    std::ifstream in(path);
    if (!in)
        return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

}  // namespace

MapCatalog &MapCatalog::Instance() {
    static MapCatalog g;
    return g;
}

void MapCatalog::SetMapDataDir(std::string dir) {
    std::lock_guard<std::mutex> lk(mu_);
    map_data_dir_ = std::move(dir);
    loaded_ = false;
}

void MapCatalog::SetPublicMapCapacity(uint32_t n) {
    if (n > 0)
        public_capacity_ = n;
}

void MapCatalog::SetAoiViewRadiusCells(int n) {
    if (n >= 0)
        aoi_view_radius_ = n;
}

void MapCatalog::SetMapTickHz(int hz) {
    if (hz > 0)
        map_tick_hz_ = hz;
}

bool MapCatalog::LoadDirectory(const std::string &dir, std::string *err) {
    const std::string manifest_path = dir + "/map_manifest.json";
    const std::string body = ReadAll(manifest_path);
    if (body.empty()) {
        if (err)
            *err = "missing " + manifest_path;
        return false;
    }
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string parse_err;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &root, &parse_err)) {
        if (err)
            *err = "manifest json: " + parse_err;
        return false;
    }
    const Json::Value &maps = root["maps"];
    if (!maps.isArray() || maps.empty()) {
        if (err)
            *err = "manifest maps[] empty";
        return false;
    }
    std::unordered_map<uint64_t, std::shared_ptr<const MapStaticData>> loaded;
    for (const auto &m : maps) {
        const uint64_t tid = m.get("map_template_id", 0).asUInt64();
        const std::string file = m.get("file", "").asString();
        std::string sha_file = m.get("sha256_file", "").asString();
        if (tid == 0 || file.empty()) {
            if (err)
                *err = "manifest entry missing template/file";
            return false;
        }
        if (sha_file.empty())
            sha_file = file + ".sha256";
        const std::string json_path = dir + "/" + file;
        const std::string sha_path = dir + "/" + sha_file;
        std::string expect;
        std::string herr;
        if (!MapStaticData::ReadSha256File(sha_path, &expect, &herr)) {
            if (err)
                *err = herr;
            return false;
        }
        std::shared_ptr<const MapStaticData> data;
        if (!MapStaticData::LoadFromFile(json_path, expect, &data, err))
            return false;
        if (data->map_template_id() != tid) {
            if (err)
                *err = "template id mismatch in " + file;
            return false;
        }
        loaded[tid] = std::move(data);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        maps_ = std::move(loaded);
        map_data_dir_ = dir;
        loaded_ = true;
    }
    LOG_INFO << "MapCatalog loaded dir=" << dir << " templates=" << maps_.size();
    return true;
}

bool MapCatalog::EnsureDefault(std::string *err) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (loaded_ && !maps_.empty())
            return true;
    }
    std::string dir = map_data_dir_;
    if (dir.empty()) {
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/maps", &resolved))
            dir = resolved;
        else
            dir = "config/maps";
    }
    return LoadDirectory(dir, err);
}

std::shared_ptr<const MapStaticData> MapCatalog::Get(uint64_t map_template_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_template_id);
    if (it == maps_.end())
        return nullptr;
    return it->second;
}

bool MapCatalog::empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return maps_.empty();
}

void MapCatalog::ClearForTest() {
    std::lock_guard<std::mutex> lk(mu_);
    maps_.clear();
    loaded_ = false;
}

void MapCatalog::PutForTest(std::shared_ptr<const MapStaticData> data) {
    if (!data)
        return;
    std::lock_guard<std::mutex> lk(mu_);
    maps_[data->map_template_id()] = std::move(data);
    loaded_ = true;
}
