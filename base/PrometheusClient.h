#pragma once

#include <string>

struct PrometheusClientResult {
    int http_status = 0;
    std::string body;
    std::string error;
};

PrometheusClientResult PrometheusInstantQuery(const std::string &query);
PrometheusClientResult PrometheusQueryRange(const std::string &query, const std::string &start,
                                            const std::string &end, const std::string &step);
