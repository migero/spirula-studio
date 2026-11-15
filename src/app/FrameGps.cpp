#include "app/FrameGps.h"

#ifdef SS_TOOL_SFM
#include "sfm/core/Telemetry.h"
#endif

#include <algorithm>
#include <cmath>
#include <climits>

namespace app {

#ifdef SS_TOOL_SFM
namespace {

bool interpolate(const std::vector<sfm::TelemetryGps>& gps, double t, double& lat,
                   double& lon) {
    if (gps.empty()) return false;
    if (t <= gps.front().t) {
        lat = gps.front().lat;
        lon = gps.front().lon;
        return true;
    }
    if (t >= gps.back().t) {
        lat = gps.back().lat;
        lon = gps.back().lon;
        return true;
    }
    auto it = std::upper_bound(gps.begin(), gps.end(), t,
                               [](double v, const sfm::TelemetryGps& g) {
                                   return v < g.t;
                               });
    const sfm::TelemetryGps& b = *it;
    const sfm::TelemetryGps& a = *(it - 1);
    const double f = (t - a.t) / std::max(b.t - a.t, 1e-9);
    lat = a.lat + (b.lat - a.lat) * f;
    lon = a.lon + (b.lon - a.lon) * f;
    return true;
}

double distance_m(double lat0, double lon0, double lat1, double lon1) {
    return sfm::telemetry_haversine_m(lat0, lon0, lat1, lon1);
}

}  // namespace
#endif

bool plan_by_gps(const std::string& path, const GpsPlanOptions& options,
                 GpsPlanResult& result,
                 const std::function<void(int64_t, int64_t, float)>& measured,
                 std::string& error) {
    result = {};
    if (!(options.spacing_m > 0) || options.frames <= 0 || !(options.fps > 0)) {
        error = "GPS spacing requires a positive distance, frame rate and frame count";
        return false;
    }
#ifndef SS_TOOL_SFM
    (void)path;
    (void)measured;
    error = "GPS frame spacing requires the structure-from-motion module";
    return false;
#else
    sfm::Telemetry telemetry;
    if (!sfm::telemetry_read(path, telemetry, error)) return false;
    std::vector<sfm::TelemetryGps> gps;
    sfm::telemetry_gps_filter(telemetry, gps);
    if (gps.size() < 2) {
        error = "the video has no usable GPS path";
        return false;
    }

    const int64_t first = std::max<int64_t>(0, options.start_frame);
    double lat = 0, lon = 0;
    if (!interpolate(gps, options.time_offset_s + (double)first / options.fps, lat, lon)) {
        error = "the GPS stream has no frame-aligned samples";
        return false;
    }
    double accumulated = 0;
    double previous_lat = lat, previous_lon = lon;
    const int cap = options.max_frames > 0 ? options.max_frames : INT32_MAX;
    result.ends.push_back(first);
    if ((int)result.ends.size() >= cap) return true;
    for (int64_t frame = first; frame < options.frames; frame++) {
        if (!interpolate(gps, options.time_offset_s + (double)frame / options.fps,
                         lat, lon))
            continue;
        const float step = (float)distance_m(previous_lat, previous_lon, lat, lon);
        accumulated += step;
        result.path_m += step;
        previous_lat = lat;
        previous_lon = lon;
        result.distance.push_back(step);
        if (accumulated >= options.spacing_m || frame + 1 == options.frames) {
            result.ends.push_back(frame);
            accumulated = 0;
            if ((int)result.ends.size() >= cap) break;
        }
        if (measured) measured(frame, options.frames, step);
    }
    return true;
#endif
}

}  // namespace app
