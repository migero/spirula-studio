// The sun's place in the sky from an instant and a position: azimuth clockwise
// from north and elevation above the horizon, degrees. NOAA's low-precision
// solar position (the one published as "General Solar Position Calculations"),
// good to 0.01 deg for the years this matters. map/SunHeading.h compares it
// with the direction a camera saw.
#pragma once

#include <cmath>

namespace sfm {

struct SunAzEl {
    bool valid = false;
    double azimuth_deg = 0;     // clockwise from north
    double elevation_deg = 0;   // above the horizon
};

// `unix_time` in seconds UTC, `lon_deg` east positive.
inline SunAzEl sunAzEl(double unix_time, double lat_deg, double lon_deg) {
    SunAzEl out;
    if (!std::isfinite(unix_time) || !std::isfinite(lat_deg) || !std::isfinite(lon_deg))
        return out;
    constexpr double kPi = 3.14159265358979323846;
    const double deg = kPi / 180.0;
    const double lat = lat_deg * deg;
    const double jd = unix_time / 86400.0 + 2440587.5;
    const double T = (jd - 2451545.0) / 36525.0;
    const double L0 = std::fmod(280.46646 + T * (36000.76983 + T * 0.0003032), 360.0);
    const double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
    const double Mr = M * deg;
    const double C = std::sin(Mr) * (1.914602 - T * (0.004817 + 0.000014 * T)) +
                     std::sin(2.0 * Mr) * (0.019993 - 0.000101 * T) +
                     std::sin(3.0 * Mr) * 0.000289;
    const double omega = 125.04 - 1934.136 * T;
    const double lambda = (L0 + C - 0.00569 - 0.00478 * std::sin(omega * deg)) * deg;
    const double eps0 =
        23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
    const double eps = (eps0 + 0.00256 * std::cos(omega * deg)) * deg;
    const double dec = std::asin(std::sin(eps) * std::sin(lambda));
    const double y = std::tan(eps / 2.0) * std::tan(eps / 2.0);
    const double eq =
        4.0 * (180.0 / kPi) *
        (y * std::sin(2.0 * L0 * deg) - 2.0 * e * std::sin(Mr) +
         4.0 * e * y * std::sin(Mr) * std::cos(2.0 * L0 * deg) -
         0.5 * y * y * std::sin(4.0 * L0 * deg) - 1.25 * e * e * std::sin(2.0 * Mr));
    const double minutes = std::fmod(unix_time, 86400.0) / 60.0;
    double ha = (minutes + eq + 4.0 * lon_deg) / 4.0 - 180.0;
    ha = std::fmod(ha, 360.0) * deg;
    const double cos_zen = std::sin(lat) * std::sin(dec) +
                           std::cos(lat) * std::cos(dec) * std::cos(ha);
    const double zen = std::acos(std::fmax(-1.0, std::fmin(1.0, cos_zen)));
    double az = std::atan2(std::sin(ha),
                           std::cos(ha) * std::sin(lat) - std::tan(dec) * std::cos(lat)) /
                    deg +
                180.0;
    az = std::fmod(az, 360.0);
    if (az < 0) az += 360.0;
    out.azimuth_deg = az;
    out.elevation_deg = 90.0 - zen / deg;
    out.valid = true;
    return out;
}

}  // namespace sfm
