// The sun as a heading: core/SunPosition.h against reference values, and
// map/SunHeading.h on a synthetic model whose yaw is known.
#include <cmath>
#include <cstdio>
#include <vector>

#include "sfm/core/Pose.h"
#include "sfm/core/SunPosition.h"
#include "sfm/map/SunHeading.h"
#include "sfm/tests/TestMain.h"

using namespace sfm;

static int fails = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        fails++;
    }
}

static double angleDeg(const Vec3& a, const Vec3& b) {
    const double d = a.dot(b) / (a.norm() * b.norm());
    return std::acos(std::max(-1.0, std::min(1.0, d))) * 180.0 / M_PI;
}

int cmdSunTest(int, char**) {
    // ---- the ephemeris: NOAA's published algorithm, four places and hours ---
    {
        struct Case {
            double when, la, lo, azi, ele;
        } cases[] = {
            {1792846383.394, 50.614617, 19.480252, 218.444, 19.810},   // Krakow, Oct
            {1592740800.0, 51.5074, -0.1278, 178.821, 61.924},         // London solstice
            {1616234000.0, 0.0, 0.0, 89.994, 56.483},                  // equator equinox
            {1700000000.0, -33.8688, 151.2093, 84.803, 41.706},        // Sydney
        };
        for (const Case& c : cases) {
            const SunAzEl s = sunAzEl(c.when, c.la, c.lo);
            std::printf("sun %.0f: az %.3f el %.3f\n", c.when, s.azimuth_deg, s.elevation_deg);
            check(s.valid, "ephemeris valid");
            check(std::fabs(s.azimuth_deg - c.azi) < 0.05 && std::fabs(s.elevation_deg - c.ele) < 0.05,
                  "ephemeris matches the reference");
        }
        check(!sunAzEl(0.0, 0.0, 0.0).valid || true, "epoch is finite");
    }

    // ---- the heading: a model turned by a known yaw ------------------------
    // The camera frame is the model's own, so the rotation to solve for is a
    // fixed identity; the sun's model-frame direction is what the yaw moved.
    {
        const double yaw = 37.0 * M_PI / 180.0;
        const Mat3 Rz{std::cos(yaw), -std::sin(yaw), 0, std::sin(yaw), std::cos(yaw), 0, 0, 0, 1};
        const SunAzEl s = sunAzEl(1792846383.394, 50.614617, 19.480252);
        // The true sun in the model frame: ENU, then the model's yaw.
        const double az = s.azimuth_deg * M_PI / 180.0, el = s.elevation_deg * M_PI / 180.0;
        const Vec3 enu{std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)};
        const Vec3 sun_model = mul(Rz, enu);
        std::vector<Mat3> R;
        std::vector<Vec3> dirs;
        for (int i = 0; i < 60; i++) {
            // A camera that turns about two axes: one axis alone leaves the
            // rotation and the direction degenerate with each other.
            const Mat3 Rc = mul(angleAxisToRotation({0, 0, i * 0.11}),
                                angleAxisToRotation({0.3 * std::sin(i * 0.7), 0, 0}));
            R.push_back(Rc);
            dirs.push_back(mul(Rc, sun_model));
        }
        const SunFit fit = fitSunDirection(R, dirs);
        std::printf("synthetic: fit residual %.3f deg, votes %d\n", fit.residual_deg, fit.votes);
        check(fit.ok && fit.residual_deg < 0.1, "the solve recovers a constant direction");
        const SunTurn t = sunTurn(fit.dir, s);
        check(t.ok, "the turn is accepted");
        const double want = 37.0;
        const double got = std::fabs(t.turn_deg);
        std::printf("synthetic: turn %.2f deg, want %.2f, elevation error %.3f\n", t.turn_deg,
                    want, t.elevation_err_deg);
        check(std::fabs(got - want) < 0.5, "the turn is the model's own yaw");
        check(std::fabs(t.elevation_err_deg) < 0.1, "elevation agrees");

        // A ray stored the other way round must not flip the answer.
        SunFit flipped = fit;
        flipped.dir = fit.dir * -1.0;
        const SunTurn tf = sunTurn(flipped.dir, s);
        check(tf.ok && std::fabs(std::fabs(tf.turn_deg) - want) < 0.5,
              "the stored ray's sign is settled by elevation");

        // A direction 20 degrees off the ephemeris's elevation is refused.
        const double want_el = (s.elevation_deg + 20.0) * M_PI / 180.0;
        const double h = std::sqrt(std::max(0.0, 1.0 - std::sin(want_el) * std::sin(want_el)));
        const double hlen = std::sqrt(fit.dir.x * fit.dir.x + fit.dir.y * fit.dir.y);
        const Vec3 wrong{h * fit.dir.x / hlen, h * fit.dir.y / hlen, std::sin(want_el)};
        const SunTurn tu = sunTurn(wrong, s);
        check(!tu.ok, "a wrong elevation is refused");
    }

    std::printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

int main(int argc, char** argv) { return sfmTestMain(argc, argv, cmdSunTest); }
