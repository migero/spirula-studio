#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace app {

struct GpsPlanOptions {
    double spacing_m = 0;
    double fps = 0;
    int64_t frames = 0;
    int64_t start_frame = 0;
    int max_frames = 0;
    double time_offset_s = 0;
};

struct GpsPlanResult {
    std::vector<int64_t> ends;
    std::vector<float> distance;
    double path_m = 0;
};

bool plan_by_gps(const std::string& path, const GpsPlanOptions& options,
                GpsPlanResult& result, const std::function<void(int64_t, int64_t, float)>& measured,
                std::string& error);

}  // namespace app
