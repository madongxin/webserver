#include "MapStaticData.h"

#include "FileHash.h"

#include <json/json.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

void SetErr(std::string *err, const std::string &m) {
    if (err)
        *err = m;
}

bool AsFloat3(const Json::Value &v, MapVec3 *out) {
    if (!v.isArray() || v.size() != 3 || !out)
        return false;
    out->x = v[0].asFloat();
    out->y = v[1].asFloat();
    out->z = v[2].asFloat();
    return std::isfinite(out->x) && std::isfinite(out->y) && std::isfinite(out->z);
}

}  // namespace

std::string MapStaticData::NormalizeSha256Hex(std::string hex) {
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ' ||
                            hex.back() == '\t'))
        hex.pop_back();
    size_t i = 0;
    while (i < hex.size() && (hex[i] == ' ' || hex[i] == '\t'))
        ++i;
    hex = hex.substr(i);
    for (char &c : hex) {
        if (c >= 'A' && c <= 'F')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return hex;
}

bool MapStaticData::ReadSha256File(const std::string &path, std::string *hex, std::string *err) {
    std::ifstream in(path);
    if (!in) {
        SetErr(err, "sha256 file missing: " + path);
        return false;
    }
    std::string line;
    if (!std::getline(in, line)) {
        SetErr(err, "sha256 file empty: " + path);
        return false;
    }
    // 允许 "hash" 或 "hash  filename"
    const auto sp = line.find_first_of(" \t");
    if (sp != std::string::npos)
        line = line.substr(0, sp);
    line = NormalizeSha256Hex(line);
    if (line.size() != 64) {
        SetErr(err, "sha256 file not 64 hex chars");
        return false;
    }
    if (hex)
        *hex = line;
    return true;
}

bool MapStaticData::Parse(const std::string &json, MapStaticData *out, std::string *err) {
    if (!out)
        return false;
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string parse_err;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    if (!reader->parse(json.data(), json.data() + json.size(), &root, &parse_err)) {
        SetErr(err, "json parse: " + parse_err);
        return false;
    }
    if (!root.isObject()) {
        SetErr(err, "json root not object");
        return false;
    }
    out->schema_version_ = root.get("schema_version", 0).asInt();
    if (out->schema_version_ != 1) {
        SetErr(err, "unsupported schema_version");
        return false;
    }
    out->map_template_id_ = root.get("map_template_id", 0).asUInt64();
    out->data_version_ = root.get("data_version", 0).asUInt64();
    out->scene_name_ = root.get("scene_name", "").asString();
    if (out->map_template_id_ == 0 || out->data_version_ == 0) {
        SetErr(err, "map_template_id/data_version required");
        return false;
    }
    if (!AsFloat3(root["bounds_min"], &out->bounds_min_) ||
        !AsFloat3(root["bounds_max"], &out->bounds_max_)) {
        SetErr(err, "bounds_min/bounds_max must be [x,y,z]");
        return false;
    }
    if (out->bounds_max_.x <= out->bounds_min_.x || out->bounds_max_.z <= out->bounds_min_.z) {
        SetErr(err, "invalid X/Z bounds");
        return false;
    }
    out->aoi_cell_size_ = root.get("aoi_cell_size", 12.0).asFloat();
    out->nav_sample_step_ = root.get("nav_sample_step", 1.0).asFloat();
    if (!(out->aoi_cell_size_ > 0.f) || !(out->nav_sample_step_ > 0.f)) {
        SetErr(err, "aoi_cell_size/nav_sample_step must be > 0");
        return false;
    }
    out->grid_width_ = root.get("grid_width", 0).asInt();
    out->grid_height_ = root.get("grid_height", 0).asInt();
    if (out->grid_width_ <= 0 || out->grid_height_ <= 0) {
        SetErr(err, "grid_width/grid_height required");
        return false;
    }
    const Json::Value &rle = root["walkable_rle"];
    if (!rle.isArray() || rle.size() < 2 || (rle.size() % 2) != 0) {
        SetErr(err, "walkable_rle must be even-length [value,count,...]");
        return false;
    }
    const size_t cells = static_cast<size_t>(out->grid_width_) * static_cast<size_t>(out->grid_height_);
    out->walkable_.assign(cells, 0);
    size_t pos = 0;
    for (Json::ArrayIndex i = 0; i + 1 < rle.size(); i += 2) {
        const int value = rle[i].asInt();
        const int count = rle[i + 1].asInt();
        if (value != 0 && value != 1) {
            SetErr(err, "walkable_rle value must be 0 or 1");
            return false;
        }
        if (count <= 0) {
            SetErr(err, "walkable_rle count must be > 0");
            return false;
        }
        if (pos + static_cast<size_t>(count) > cells) {
            SetErr(err, "walkable_rle overflows grid");
            return false;
        }
        std::fill(out->walkable_.begin() + static_cast<std::ptrdiff_t>(pos),
                  out->walkable_.begin() + static_cast<std::ptrdiff_t>(pos + static_cast<size_t>(count)),
                  static_cast<uint8_t>(value));
        pos += static_cast<size_t>(count);
    }
    if (pos != cells) {
        SetErr(err, "walkable_rle cell count mismatch");
        return false;
    }
    const Json::Value &spawns = root["spawn_points"];
    if (!spawns.isArray() || spawns.empty()) {
        SetErr(err, "spawn_points required");
        return false;
    }
    out->spawns_.clear();
    for (const auto &s : spawns) {
        MapSpawnPoint sp;
        sp.id = s.get("id", "").asString();
        if (sp.id.empty())
            sp.id = "unnamed";
        if (!AsFloat3(s["position"], &sp.position)) {
            SetErr(err, "spawn position must be [x,y,z]");
            return false;
        }
        sp.yaw = s.get("yaw", 0).asFloat();
        if (!std::isfinite(sp.yaw)) {
            SetErr(err, "spawn yaw not finite");
            return false;
        }
        out->spawns_.push_back(sp);
    }
    out->default_spawn_ = out->spawns_.front();
    for (const auto &s : out->spawns_) {
        if (s.id == "default") {
            out->default_spawn_ = s;
            break;
        }
    }
    return out->ValidateSpawns(err);
}

bool MapStaticData::ValidateSpawns(std::string *err) const {
    if (spawns_.empty()) {
        SetErr(err, "no spawn points");
        return false;
    }
    for (const auto &s : spawns_) {
        if (!InBounds(s.position.x, s.position.y, s.position.z)) {
            SetErr(err, "spawn out of bounds: " + s.id);
            return false;
        }
        if (!IsWalkable(s.position.x, s.position.z)) {
            SetErr(err, "spawn not walkable: " + s.id);
            return false;
        }
    }
    return true;
}

bool MapStaticData::LoadFromJson(const std::string &json, std::shared_ptr<const MapStaticData> *out,
                                 std::string *err) {
    auto data = std::shared_ptr<MapStaticData>(new MapStaticData());
    if (!Parse(json, data.get(), err))
        return false;
    {
        FileHasher h(FileHasher::Algo::Sha256);
        if (!h.Update(json)) {
            SetErr(err, "hash json failed");
            return false;
        }
        data->sha256_ = h.FinalizeHex();
    }
    if (out)
        *out = std::move(data);
    return true;
}

bool MapStaticData::LoadFromFile(const std::string &path, const std::string &expected_sha256_hex,
                                 std::shared_ptr<const MapStaticData> *out, std::string *err) {
    const std::string got = FileHasher::HashFile(path);
    if (got.empty()) {
        SetErr(err, "hash file failed: " + path);
        return false;
    }
    const std::string expect = NormalizeSha256Hex(expected_sha256_hex);
    if (expect.size() == 64 && got != expect) {
        SetErr(err, "ERR_MAP_DATA_MISMATCH file hash");
        return false;
    }
    std::ifstream in(path);
    if (!in) {
        SetErr(err, "open map json failed: " + path);
        return false;
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    auto data = std::shared_ptr<MapStaticData>(new MapStaticData());
    if (!Parse(oss.str(), data.get(), err))
        return false;
    data->sha256_ = got;
    if (out)
        *out = std::move(data);
    return true;
}

bool MapStaticData::WorldToWalkableCell(float x, float z, int *col, int *row) const {
    if (!(nav_sample_step_ > 0.f))
        return false;
    const int c = static_cast<int>(std::floor((x - bounds_min_.x) / nav_sample_step_));
    const int r = static_cast<int>(std::floor((z - bounds_min_.z) / nav_sample_step_));
    if (col)
        *col = c;
    if (row)
        *row = r;
    return c >= 0 && r >= 0 && c < grid_width_ && r < grid_height_;
}

bool MapStaticData::CellWalkable(int col, int row) const {
    if (col < 0 || row < 0 || col >= grid_width_ || row >= grid_height_)
        return false;
    const size_t idx = static_cast<size_t>(row) * static_cast<size_t>(grid_width_) +
                       static_cast<size_t>(col);
    return idx < walkable_.size() && walkable_[idx] != 0;
}

bool MapStaticData::IsWalkable(float x, float z) const {
    int col = 0, row = 0;
    return WorldToWalkableCell(x, z, &col, &row) && CellWalkable(col, row);
}

bool MapStaticData::InBounds(float x, float y, float z) const {
    const float y_slop = 2.f;
    return x >= bounds_min_.x && x <= bounds_max_.x && z >= bounds_min_.z && z <= bounds_max_.z &&
           y >= bounds_min_.y - y_slop && y <= bounds_max_.y + y_slop;
}

void MapStaticData::WorldToAoiCell(float x, float z, int *cell_x, int *cell_z) const {
    const float step = aoi_cell_size_ > 0.f ? aoi_cell_size_ : 12.f;
    if (cell_x)
        *cell_x = static_cast<int>(std::floor((x - bounds_min_.x) / step));
    if (cell_z)
        *cell_z = static_cast<int>(std::floor((z - bounds_min_.z) / step));
}
