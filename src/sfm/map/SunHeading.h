// Where the sun was, in a reconstructed model's frame.
//
// A GoPro player stores the sun's direction in each frame of the camera-locked
// track it writes beside a 360 video. The direction is fixed in the world and
// only the camera turns, so the frames' poses and those directions together say
// which world direction the sun is -- provided the fixed rotation from the
// video's camera frame into the model's is solved for too. That rotation is the
// yaw the gravity gauge cannot see, and `core/SunPosition.h` supplies the sun's
// true azimuth, so the model's north follows. The solve is the same alternating
// Procrustes the gravity consensus uses; `docs/notes/imu-gps-for-sfm.md`.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "sfm/core/Pose.h"
#include "sfm/core/SunPosition.h"
#include "sfm/geometry/LinAlg.h"
#include "sfm/map/ImuExtrinsic.h"

namespace sfm {

struct SunFit {
    bool ok = false;
    Vec3 dir{0, 0, 1};         // toward the sun, model frame
    double residual_deg = 0;   // median frame-to-consensus angle
    int votes = 0;
};

// `R` is world -> camera for the frames the sun directions belong to, `sun` the
// direction in that camera frame (either sense; the sign falls out of the fit).
inline SunFit fitSunDirection(const std::vector<Mat3>& R, const std::vector<Vec3>& sun) {
    SunFit out;
    const size_t n = std::min(R.size(), sun.size());
    if (n < 20) return out;
    Vec3 u{0, 0, 1};
    Mat3 E = mat3Identity();
    for (int it = 0; it < 100; it++) {
        Mat3 M{};   // E maps the camera frame's sun onto R u
        for (size_t i = 0; i < n; i++) {
            const Vec3 a = mul(R[i], u);
            const double av[3] = {a.x, a.y, a.z};
            const double bv[3] = {sun[i].x, sun[i].y, sun[i].z};
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) M[3 * r + c] += av[r] * bv[c];
        }
        E = extrinsic_detail::nearestOrthogonal(M);
        Vec3 sum{0, 0, 0};
        for (size_t i = 0; i < n; i++) sum = sum + mul(transpose(R[i]), mul(E, sun[i]));
        const double norm = sum.norm();
        if (!(norm > 1e-12)) return out;
        const Vec3 next = sum * (1.0 / norm);
        const bool done = next.dot(u) > 1.0 - 1e-12;
        u = next;
        if (done) break;
    }
    std::vector<double> residuals;
    residuals.reserve(n);
    for (size_t i = 0; i < n; i++)
        residuals.push_back(extrinsic_detail::angleDeg(mul(transpose(R[i]), mul(E, sun[i])), u));
    std::nth_element(residuals.begin(), residuals.begin() + (long)(residuals.size() / 2),
                     residuals.end());
    out.dir = u;
    out.residual_deg = residuals[residuals.size() / 2];
    out.votes = (int)n;
    out.ok = std::isfinite(out.residual_deg);
    return out;
}

// What a fitted model-frame sun says about the model's yaw. The sign of the
// stored ray is settled by elevation, which is the only part a turn cannot
// change; `|elevation| < 5 deg` leaves it unsettled and is refused.
struct SunTurn {
    bool ok = false;
    double turn_deg = 0;           // about +Z, to apply to the model
    double elevation_err_deg = 0;  // model minus ephemeris
};

inline SunTurn sunTurn(const Vec3& dir, const SunAzEl& s,
                       double max_elevation_err_deg = 4.0) {
    SunTurn out;
    if (!s.valid || std::fabs(s.elevation_deg) < 5.0) return out;
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const Vec3 u = (dir.z * std::sin(s.elevation_deg * kDeg) < 0) ? dir * -1.0 : dir;
    out.elevation_err_deg =
        std::asin(std::max(-1.0, std::min(1.0, u.z))) / kDeg - s.elevation_deg;
    if (std::fabs(out.elevation_err_deg) > max_elevation_err_deg) return out;
    double az = std::atan2(u.x, u.y) / kDeg;
    if (az < 0) az += 360.0;
    double turn = std::fmod(s.azimuth_deg - az + 540.0, 360.0) - 180.0;
    out.turn_deg = turn;
    out.ok = true;
    return out;
}

}  // namespace sfm
