#include "AoiEvidence.h"

#include "MapRuntime.h"

#include <string>
#include <utility>
#include <vector>

std::string AoiVisibleJson() {
    std::vector<std::pair<uint64_t, std::vector<uint64_t>>> rows;
    MapRuntime::Instance().CopyVisibleSets(&rows);
    std::string out = "{\"viewers\":[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i)
            out.push_back(',');
        out += "{\"id\":" + std::to_string(rows[i].first) + ",\"entities\":[";
        for (size_t j = 0; j < rows[i].second.size(); ++j) {
            if (j)
                out.push_back(',');
            out += std::to_string(rows[i].second[j]);
        }
        out += "]}";
    }
    out += "]}\n";
    return out;
}
