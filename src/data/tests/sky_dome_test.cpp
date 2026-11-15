// sky_dome_test -- the synthetic sky hemisphere (data/SkyDome.h) against
// inputs whose answer is known: the icosphere's own vertex count, the radius
// it was measured from, and a planted floater that must not set the radius.

#include "data/SkyDome.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

// Cameras walking a ring, the shape a 360 rig's path actually has: it keeps
// the geometric median at the middle instead of near an edge.
std::vector<double> ring_cameras(int n, double radius, double height) {
    std::vector<double> c;
    for (int i = 0; i < n; i++) {
        const double a = i * 0.0157;
        const double r = radius + 3.0 * std::sin(i * 0.07);
        c.push_back(r * std::cos(a));
        c.push_back(r * std::sin(a));
        c.push_back(height + 0.3 * std::sin(i * 0.11));
    }
    return c;
}

// Flat ground points inside `radius` of the origin.
std::vector<double> ground_points(int n, double radius) {
    std::vector<double> p;
    for (int i = 0; i < n; i++) {
        const double a = i * 2.399963;  // the golden angle
        const double r = radius * std::sqrt((i % 1000 + 1) / 1000.0);
        p.push_back(r * std::cos(a));
        p.push_back(r * std::sin(a));
        p.push_back(0.0);
    }
    return p;
}

SkyDomeFit build(const SkyDomeConfig& cfg, const std::vector<double>& cam,
                 const std::vector<double>& pts, const double up[3],
                 ColmapPoints3D* out) {
    SkyDomeFit fit;
    *out = sky_dome_points(cfg, cam, nullptr, pts.data(),
                           (int64_t)(pts.size() / 3), up, &fit);
    return fit;
}

}  // namespace

int main() {
    const double up[3] = {0, 0, 1};
    const std::vector<double> cam = ring_cameras(400, 26.0, 2.6);
    const std::vector<double> pts = ground_points(5000, 40.0);

    // 1. Vertex count. The icosahedron this starts from has four vertices
    //    exactly on the equator and is not top/bottom symmetric, so the split
    //    is a shade over half rather than exactly half.
    {
        ColmapPoints3D dome;
        int64_t previous = 0;
        for (int sub : {2, 3, 4, 5, 6}) {
            SkyDomeConfig cfg;
            cfg.subdivisions = sub;
            SkyDomeFit fit = build(cfg, cam, pts, up, &dome);
            const int64_t total = 10 * (1 << (2 * sub)) + 2;
            check(dome.num() > total / 2 && dome.num() < (total * 3) / 5,
                  "subdivisions " + std::to_string(sub) + ": " +
                      std::to_string(dome.num()) + " of " +
                      std::to_string(total) + ", just over half");
            check(dome.num() > previous, "  and it grows with the level");
            check(fit.points == dome.num(), "  fit count agrees with the cloud");
            previous = dome.num();
        }
        // The default level is the one a user gets.
        SkyDomeConfig cfg;
        cfg.subdivisions = 5;
        SkyDomeFit fit = build(cfg, cam, pts, up, &dome);
        check(dome.num() == 5185, "default level gives 5185 (" +
                                     std::to_string(dome.num()) + ")");
    }

    // 2. Every point on the sphere, none below the base plane.
    {
        SkyDomeConfig cfg;
        ColmapPoints3D dome;
        SkyDomeFit fit = build(cfg, cam, pts, up, &dome);
        double worst = 0.0, min_z = 1e30;
        int64_t below = 0;
        for (int64_t i = 0; i < dome.num(); i++) {
            const double dx = dome.xyz[i * 3] - fit.center[0];
            const double dy = dome.xyz[i * 3 + 1] - fit.center[1];
            const double dz = dome.xyz[i * 3 + 2] - fit.center[2];
            worst = std::max(worst, std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) -
                                               fit.radius));
            min_z = std::min(min_z, dz);
            if (dz < -1e-9) below++;
        }
        check(worst < 1e-9, "every point is exactly on the sphere (worst " +
                                std::to_string(worst) + ")");
        check(below == 0, "no point below the base (" + std::to_string(below) + ")");
        check(std::fabs(min_z) < 1e-9 || min_z >= 0.0,
              "the base sits at the centre's height");
    }

    // 3. The centre is the cameras' geometric median, so it lands in the middle
    //    of the ring rather than off to one side.
    {
        SkyDomeConfig cfg;
        ColmapPoints3D dome;
        SkyDomeFit fit = build(cfg, cam, pts, up, &dome);
        check(std::fabs(fit.center[0]) < 2.0 && std::fabs(fit.center[1]) < 2.0,
              "centre x/y is at the middle of the camera ring");
        check(fit.center[2] > 2.0 && fit.center[2] < 3.2,
              "centre z is at the cameras' height");
    }

    // 4. The radius is a percentile, so floaters cannot set it -- and the
    //    literal maximum stays reachable when asked for.
    {
        SkyDomeConfig cfg;
        ColmapPoints3D dome;
        SkyDomeFit clean = build(cfg, cam, pts, up, &dome);

        std::vector<double> floater = pts;
        for (int k = 0; k < 5; k++) {
            floater.push_back(1400.0);
            floater.push_back(0.0);
            floater.push_back(10.0);
        }
        ColmapPoints3D dome2;
        SkyDomeFit dirty = build(cfg, cam, floater, up, &dome2);

        check(dirty.max_radius > 1000.0, "the floater really is the maximum");
        check(std::fabs(dirty.radius - clean.radius) < 1.0,
              "p99 radius is unmoved by the floater (" +
                  std::to_string(clean.radius) + " -> " +
                  std::to_string(dirty.radius) + ")");
        check(dirty.percentile_radius < 100.0, "p99 ignores it too");

        SkyDomeConfig all = cfg;
        all.percentile = 100.0;
        ColmapPoints3D dome3;
        SkyDomeFit at_max = build(all, cam, floater, up, &dome3);
        check(std::fabs(at_max.radius - dirty.max_radius) < 1e-6,
              "percentile 100 gives the literal maximum");
    }
    // The percentile tracks the data rather than being a fixed number.
    {
        SkyDomeConfig cfg;
        ColmapPoints3D dome;
        SkyDomeFit fit = build(cfg, cam, pts, up, &dome);
        check(fit.radius > 30.0 && fit.radius < 45.0,
              "p99 of points inside 40 m lands near it (" +
                  std::to_string(fit.radius) + ")");
        check(fit.radius <= fit.max_radius + 1e-9, "never larger than the maximum");
    }

    // 5. A live-point mask is honoured, so a deleted point cannot hold the dome.
    {
        SkyDomeConfig cfg;
        const int64_t n = (int64_t)(pts.size() / 3);
        std::vector<uint8_t> alive((size_t)n, 1);
        SkyDomeFit all_live;
        sky_dome_points(cfg, cam, &alive, pts.data(), n, up, &all_live);
        check(all_live.center[2] > 2.0, "an all-live mask is the plain case");
        // Kill everything past 10 m: the radius must follow.
        for (int64_t i = 0; i < n; i++)
            if (std::fabs(pts[(size_t)i * 3]) > 10.0 ||
                std::fabs(pts[(size_t)i * 3 + 1]) > 10.0)
                alive[(size_t)i] = 0;
        SkyDomeFit smaller;
        sky_dome_points(cfg, cam, &alive, pts.data(), n, up, &smaller);
        // What is left lies in a 20 m box, so its farthest point is 14.14 m
        // from the middle of it.
        check(smaller.radius < 14.2 && smaller.radius > 12.0,
              "deleting the far points pulls the radius in (" +
                  std::to_string(smaller.radius) + ")");
    }

    // 6. The colour is the sky blue, on every point.
    {
        SkyDomeConfig cfg;
        cfg.rgb[0] = 135; cfg.rgb[1] = 180; cfg.rgb[2] = 220;
        ColmapPoints3D dome;
        build(cfg, cam, pts, up, &dome);
        bool all_blue = dome.rgb.size() == (size_t)dome.num() * 3;
        for (int64_t i = 0; all_blue && i < dome.num(); i++)
            all_blue = dome.rgb[(size_t)i * 3] == 135 &&
                       dome.rgb[(size_t)i * 3 + 1] == 180 &&
                       dome.rgb[(size_t)i * 3 + 2] == 220;
        check(all_blue, "every dome point carries the configured sky blue");
    }

    // 7. A tilted up axis still levels the base, and nothing lands below it.
    {
        SkyDomeConfig cfg;
        const double tilted[3] = {0.3, 0.1, 0.9487};
        ColmapPoints3D dome;
        SkyDomeFit fit = build(cfg, cam, pts, tilted, &dome);
        // Base plane normal is `tilted`: every offset must be non-negative
        // along it.
        double worst = 1e30;
        for (int64_t i = 0; i < dome.num(); i++) {
            const double d = (dome.xyz[i * 3] - fit.center[0]) * tilted[0] +
                             (dome.xyz[i * 3 + 1] - fit.center[1]) * tilted[1] +
                             (dome.xyz[i * 3 + 2] - fit.center[2]) * tilted[2];
            worst = std::min(worst, d);
        }
        check(worst > -1e-9, "a tilted dome is still entirely above its base");
    }

    // 8. Refusals, rather than a dome of nothing.
    {
        SkyDomeConfig cfg;
        const double* up3 = up;
        bool threw = false;
        try {  // Two cameras cannot give a median worth trusting.
            std::vector<double> two = {0, 0, 0, 1, 0, 0};
            sky_dome_points(cfg, two, nullptr, pts.data(),
                            (int64_t)(pts.size() / 3), up3, nullptr);
        } catch (const std::exception&) { threw = true; }
        check(threw, "too few cameras is refused");

        threw = false;
        try {  // One point has no distribution to take a percentile of.
            std::vector<double> one = {0, 0, 0};
            sky_dome_points(cfg, cam, nullptr, one.data(), 1, up3, nullptr);
        } catch (const std::exception&) { threw = true; }
        check(threw, "too few keypoints is refused");

        threw = false;
        try {
            SkyDomeConfig bad;
            bad.subdivisions = 9;
            sky_dome_points(bad, cam, nullptr, pts.data(),
                            (int64_t)(pts.size() / 3), up3, nullptr);
        } catch (const std::exception&) { threw = true; }
        check(threw, "an impossible subdivision count is refused");
    }

    std::printf("%s\n", g_failures ? "FAILURES" : "all checks passed");
    return g_failures ? 1 : 0;
}