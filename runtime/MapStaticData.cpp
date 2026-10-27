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

bool MapStaticData::ClampOntoWalkable(float from_x, float from_z, float to_x, float to_z, float *ox,
                                      float *oz) const {
    if (IsWalkable(to_x, to_z)) {
        if (ox)
            *ox = to_x;
        if (oz)
            *oz = to_z;
        return true;
    }
    if (IsWalkable(from_x, from_z)) {
        float lo = 0.f;
        float hi = 1.f;
        for (int i = 0; i < 8; ++i) {
            const float mid = (lo + hi) * 0.5f;
            const float x = from_x + (to_x - from_x) * mid;
            const float z = from_z + (to_z - from_z) * mid;
            if (IsWalkable(x, z))
                lo = mid;
            else
                hi = mid;
        }
        const float x = from_x + (to_x - from_x) * lo;
        const float z = from_z + (to_z - from_z) * lo;
        if (ox)
            *ox = x;
        if (oz)
            *oz = z;
        return IsWalkable(x, z);
    }
    int col = 0, row = 0;
    if (!WorldToWalkableCell(to_x, to_z, &col, &row) &&
        !WorldToWalkableCell(from_x, from_z, &col, &row))
        return false;
    const float step = nav_sample_step_ > 0.f ? nav_sample_step_ : 1.f;
    constexpr int kRadius = 2;
    float best = 0.f;
    float bx = 0.f, bz = 0.f;
    bool found = false;
    for (int dr = -kRadius; dr <= kRadius; ++dr) {
        for (int dc = -kRadius; dc <= kRadius; ++dc) {
            const int c = col + dc;
            const int r = row + dr;
            if (!CellWalkable(c, r))
                continue;
            const float cx = bounds_min_.x + (static_cast<float>(c) + 0.5f) * step;
            const float cz = bounds_min_.z + (static_cast<float>(r) + 0.5f) * step;
            const float dx = cx - to_x;
            const float dz = cz - to_z;
            const float d2 = dx * dx + dz * dz;
            if (!found || d2 < best) {
                best = d2;
                bx = cx;
                bz = cz;
                found = true;
            }
        }
    }
    if (!found)
        return false;
    if (ox)
        *ox = bx;
    if (oz)
        *oz = bz;
    return true;
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

bool MapStaticData::FindScatteredSpawn(float cx, float cy, float cz, float radius, uint64_t seed,
                                       float *ox, float *oy, float *oz) const {
    if (ox)
        *ox = cx;
    if (oy)
        *oy = cy;
    if (oz)
        *oz = cz;
    if (!(radius > 0.f) || !(nav_sample_step_ > 0.f))
        return IsWalkable(cx, cz);
    uint64_t s = seed != 0 ? seed : 1;
    const int tries = 24;
    for (int i = 0; i < tries; ++i) {
        s = s * 6364136223846793005ULL + 1;
        const float ang =
            static_cast<float>((s >> 11) % 360) * 3.14159265f / 180.f;
        s = s * 6364136223846793005ULL + 1;
        const float t = static_cast<float>((s >> 11) % 1000) / 1000.f;
        const float r = radius * std::sqrt(t);
        const float x = cx + std::cos(ang) * r;
        const float z = cz + std::sin(ang) * r;
        if (IsWalkable(x, z) && InBounds(x, cy, z)) {
            if (ox)
                *ox = x;
            if (oy)
                *oy = cy;
            if (oz)
                *oz = z;
            return true;
        }
    }
    return IsWalkable(cx, cz);
}

namespace {

uint64_t MixPlayer(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

}  // namespace

bool MapStaticData::FindNearbySpawn(uint64_t player_id, float cx, float cy, float cz, int cells,
                                    float *ox, float *oy, float *oz) const {
    if (cells < 0 || !(nav_sample_step_ > 0.f) || grid_width_ <= 0 || grid_height_ <= 0)
        return false;
    int scol = 0;
    int srow = 0;
    if (!WorldToWalkableCell(cx, cz, &scol, &srow)) {
        scol = static_cast<int>(std::floor((cx - bounds_min_.x) / nav_sample_step_));
        srow = static_cast<int>(std::floor((cz - bounds_min_.z) / nav_sample_step_));
        if (scol < 0)
            scol = 0;
        if (srow < 0)
            srow = 0;
        if (scol >= grid_width_)
            scol = grid_width_ - 1;
        if (srow >= grid_height_)
            srow = grid_height_ - 1;
    }
    std::vector<std::pair<float, float>> points;
    const float step = nav_sample_step_;
    for (int dr = -cells; dr <= cells; ++dr) {
        for (int dc = -cells; dc <= cells; ++dc) {
            const int col = scol + dc;
            const int row = srow + dr;
            if (!CellWalkable(col, row))
                continue;
            const float x = bounds_min_.x + (static_cast<float>(col) + 0.5f) * step;
            const float z = bounds_min_.z + (static_cast<float>(row) + 0.5f) * step;
            if (!InBounds(x, cy, z) || !IsWalkable(x, z))
                continue;
            points.emplace_back(x, z);
        }
    }
    if (points.empty())
        return false;
    const size_t n = points.size();
    const size_t index =
        static_cast<size_t>(MixPlayer(player_id == 0 ? 1 : player_id) % n);
    if (ox)
        *ox = points[index].first;
    if (oy)
        *oy = cy;
    if (oz)
        *oz = points[index].second;
    return true;
}

bool MapStaticData::FindSpreadSpawn(uint64_t player_id, float y,
                                    const std::vector<std::pair<float, float>> &occupied,
                                    float min_sep, float *ox, float *oy, float *oz) const {
    if (!(nav_sample_step_ > 0.f) || grid_width_ <= 0 || grid_height_ <= 0)
        return false;
    constexpr float kCell = 200.f;
    const float minx = bounds_min_.x;
    const float minz = bounds_min_.z;
    const float span_x = bounds_max_.x - minx;
    const float span_z = bounds_max_.z - minz;
    if (!(span_x > 0.f) || !(span_z > 0.f))
        return false;
    const int nx = std::max(1, static_cast<int>(std::ceil(span_x / kCell)));
    const int nz = std::max(1, static_cast<int>(std::ceil(span_z / kCell)));
    struct Slot {
        float x = 0.f;
        float z = 0.f;
        float d2 = 0.f;
        bool ok = false;
    };
    std::vector<Slot> coarse(static_cast<size_t>(nx * nz));
    const float step = nav_sample_step_;
    for (int row = 0; row < grid_height_; ++row) {
        for (int col = 0; col < grid_width_; ++col) {
            if (!CellWalkable(col, row))
                continue;
            const float x = minx + (static_cast<float>(col) + 0.5f) * step;
            const float z = minz + (static_cast<float>(row) + 0.5f) * step;
            int ci = static_cast<int>(std::floor((x - minx) / kCell));
            int ri = static_cast<int>(std::floor((z - minz) / kCell));
            if (ci < 0)
                ci = 0;
            if (ri < 0)
                ri = 0;
            if (ci >= nx)
                ci = nx - 1;
            if (ri >= nz)
                ri = nz - 1;
            const float ccx = minx + (static_cast<float>(ci) + 0.5f) * kCell;
            const float ccz = minz + (static_cast<float>(ri) + 0.5f) * kCell;
            const float dx = x - ccx;
            const float dz = z - ccz;
            const float d2 = dx * dx + dz * dz;
            Slot &slot = coarse[static_cast<size_t>(ri * nx + ci)];
            if (!slot.ok || d2 < slot.d2) {
                slot.ok = true;
                slot.d2 = d2;
                slot.x = x;
                slot.z = z;
            }
        }
    }
    std::vector<std::pair<float, float>> points;
    points.reserve(coarse.size());
    for (const Slot &slot : coarse) {
        if (slot.ok)
            points.emplace_back(slot.x, slot.z);
    }
    if (points.empty())
        return false;
    const float sep2 = min_sep > 0.f ? min_sep * min_sep : 0.f;
    auto clear_of = [&](float x, float z) {
        if (!(sep2 > 0.f))
            return true;
        for (const auto &p : occupied) {
            const float dx = x - p.first;
            const float dz = z - p.second;
            if (dx * dx + dz * dz < sep2)
                return false;
        }
        return InBounds(x, y, z) && IsWalkable(x, z);
    };
    const size_t n = points.size();
    const size_t start = static_cast<size_t>(MixPlayer(player_id == 0 ? 1 : player_id) % n);
    for (size_t i = 0; i < n; ++i) {
        const auto &p = points[(start + i) % n];
        if (!clear_of(p.first, p.second))
            continue;
        if (ox)
            *ox = p.first;
        if (oy)
            *oy = y;
        if (oz)
            *oz = p.second;
        return true;
    }
    // 200 米格点都离人太近时，按同样的起点沿可走格中心继续找，顺序固定。
    const size_t cells = static_cast<size_t>(grid_width_) * static_cast<size_t>(grid_height_);
    const size_t cell_start =
        static_cast<size_t>(MixPlayer(player_id == 0 ? 1 : player_id) % std::max<size_t>(cells, 1));
    for (size_t i = 0; i < cells; ++i) {
        const size_t idx = (cell_start + i) % cells;
        const int col = static_cast<int>(idx % static_cast<size_t>(grid_width_));
        const int row = static_cast<int>(idx / static_cast<size_t>(grid_width_));
        if (!CellWalkable(col, row))
            continue;
        const float x = minx + (static_cast<float>(col) + 0.5f) * step;
        const float z = minz + (static_cast<float>(row) + 0.5f) * step;
        if (!clear_of(x, z))
            continue;
        if (ox)
            *ox = x;
        if (oy)
            *oy = y;
        if (oz)
            *oz = z;
        return true;
    }
    return false;
}

std::shared_ptr<const MapStaticData> MapStaticData::CloneWithTemplateId(
    uint64_t map_template_id) const {
    auto copy = std::shared_ptr<MapStaticData>(new MapStaticData(*this));
    copy->map_template_id_ = map_template_id;
    return copy;
}
