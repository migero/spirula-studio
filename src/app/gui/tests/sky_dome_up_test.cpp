// sky_dome_up_test -- the dome's base is level in the FILE's frame, which is
// the frame the trainer reads and the only one with a ground truth. A dome
// built from the mean of a multi-lens rig's up axes is level in neither.

#include "app/gui/edit/PointsDoc.h"
#include "data/DatasetParser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using gui::PointsDoc;
constexpr double kRad = 3.14159265358979323846;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

template <typename T>
void put(std::string& o, T v) { o.append((const char*)&v, sizeof v); }

void put_c3(double a[3], double x, double y, double z) { a[0] = x; a[1] = y; a[2] = z; }

// Cameras on a ring around a ground plane at z = 0, one mounting attitude for
// the whole rig, and optionally a gauge saying +Z is up. The gauge is what the
// dome must obey: the camera mean alone is only a fallback.
void write_rig_model(const fs::path& dir, int members, bool with_gauge) {
    fs::create_directories(dir / "images");
    for (int i = 0; i < members; i++) {
        std::ofstream img(dir / "images" / ("im" + std::to_string(i) + ".jpg"),
                          std::ios::binary);
        img << "\xff\xd8\xff\xe0";
    }
    {
        std::string s;
        put(s, (uint64_t)1);
        put(s, (uint32_t)1);
        put(s, (int32_t)1);
        put(s, (uint64_t)640);
        put(s, (uint64_t)480);
        for (int i = 0; i < 4; i++) put(s, 500.0);
        std::ofstream f(dir / "cameras.bin", std::ios::binary);
        f.write(s.data(), (std::streamsize)s.size());
    }
    {
        // Each member rotated about its own forward axis by i*45 degrees, so
        // the mean of their up vectors cancels instead of agreeing.
        std::string s;
        put(s, (uint64_t)(members * 8));
        for (int m = 0; m < members; m++) {
            for (int rep = 0; rep < 8; rep++) {
                // Aimed outward and pitched down, so the mean up is tilted
                // off the gauge's +Z: level cameras leave it singular.
                const double yaw = m * kRad / (double)members;
                const double pit = 20.0 * kRad / 180.0;      // pitched down
                const double cy = std::cos(yaw * 0.5), sy = std::sin(yaw * 0.5);
                const double cp = std::cos(pit * 0.5), sp = std::sin(pit * 0.5);
                const double qw = cy * cp, qy = sy * cp, qx = 0.0, qz = sy * sp;
                put(s, (uint32_t)(m * 8 + rep));
                put(s, qw); put(s, qx); put(s, qy); put(s, qz);
                const double r = 20.0;
                put(s, -r * std::cos(yaw)); put(s, -r * std::sin(yaw)); put(s, 2.0);
                put(s, (uint32_t)1);
                std::string nm = "im" + std::to_string(m) + ".jpg";
                s += nm; s += '\0';
                put(s, (uint64_t)1);
                put(s, 320.0); put(s, 240.0);
                put(s, (uint64_t)(m * 8 + rep + 1));
            }
        }
        std::ofstream f(dir / "images.bin", std::ios::binary);
        f.write(s.data(), (std::streamsize)s.size());
    }
    {
        // Ground points at z = 0, on a disc.
        std::string s;
        put(s, (uint64_t)1200);
        for (int i = 0; i < 1200; i++) {
            const double a = i * 2.399963;
            const double r = 30.0 * std::sqrt((i + 1) / 1200.0);
            put(s, (uint64_t)i + 1);
            put(s, r * std::cos(a)); put(s, r * std::sin(a)); put(s, 0.0);
            s.append("\x80\xc0\x40", 3);
            put(s, 0.5);
            put(s, (uint64_t)2);
            put(s, (uint32_t)(i % (members * 8)));
            put(s, (uint32_t)0);
            put(s, (uint32_t)((i + 1) % (members * 8)));
            put(s, (uint32_t)0);
        }
        std::ofstream f(dir / "points3D.bin", std::ios::binary);
        f.write(s.data(), (std::streamsize)s.size());
    }
    if (with_gauge) {
        std::ofstream g(dir / "gauge.txt");
        g << "# test\noriented 1\nmetric 1\nup sensors\nscale gps\n";
    }
}

}  // namespace

int main() {
    const fs::path root = "/tmp/sky_dome_up_test";
    fs::remove_all(root);

    // 1. With a gauge: the dome's base must be level with the ground plane.
    {
        const fs::path d = root / "gauge";
        write_rig_model(d, 8, true);
        DatasetParserConfig pc;
        ParsedDataset ds = parse_dataset(d.string(), pc, "colmap");
        check(ds.gauge_oriented, "the model reports itself oriented");

        // The gauge's up, in the view frame, is what up_hint must now return.
        PostSplitCameras post = bake_post_split(ds, false, false);
        PointsDoc doc(std::move(ds), post, (d / "sparse/0/points3D.bin").string(),
                      d.string(), nullptr);
        float up[3] = {9, 9, 9};
        const bool got = doc.up_hint(up);
        check(got, "up_hint answers with a gauge present and no live-camera need");
        const double len = std::sqrt((double)up[0]*up[0] + up[1]*up[1] + up[2]*up[2]);
        check(std::fabs(len - 1.0) < 1e-5, "and it is a unit vector");

        std::string err;
        const PointsDoc::DomeFit fit = doc.add_sky_dome(4, 99.0, &err);
        check(fit.present, "the dome was added (" + err + ")");

        // The ground's own frame, which is what a trainer reads. _ds holds
        // parsed coordinates, so the file position is parsed + ds.center.
        const ParsedDataset& pds = *doc.dataset();
        std::vector<double> cen = {pds.center[0], pds.center[1], pds.center[2]};
        const ParsedDataset& p = *doc.dataset();
        const int64_t base = p.points.num() - fit.points;

        // Lowest points of the dome, in the file frame: the base ring.
        double lo = 1e30, hi = -1e30;
        std::vector<double> zs;
        for (int64_t i = base; i < p.points.num(); i++) {
            const double q[3] = {p.points.xyz[(size_t)i * 3] + cen[0],
                                 p.points.xyz[(size_t)i * 3 + 1] + cen[1],
                                 p.points.xyz[(size_t)i * 3 + 2] + cen[2]};
            zs.push_back(q[2]);
            lo = std::min(lo, q[2]);
            hi = std::max(hi, q[2]);
        }
        // The lowest tenth of the dome, which is its base band: an exact
        // minimum would collect one vertex.
        const double band = lo + 0.02 * (hi - lo);
        std::vector<std::array<double, 2>> ring;
        std::vector<double> file_z;
        for (int64_t i = base; i < p.points.num(); i++) {
            const double q[3] = {p.points.xyz[(size_t)i * 3] + cen[0],
                                 p.points.xyz[(size_t)i * 3 + 1] + cen[1],
                                 p.points.xyz[(size_t)i * 3 + 2] + cen[2]};
            if (q[2] <= band) { ring.push_back({q[0], q[1]}); file_z.push_back(q[2]); }
        }
        check(ring.size() >= 8, "the base ring has points (" +
                                     std::to_string(ring.size()) + ")");

        // Tilt only, so this survives an anisotropic normalization (which
        // turns the base circle into an ellipse, but not a tilted one): least
        // squares z = a*x + b*y + c over the band, tilt = atan|grad z|.
        double cx = 0, cy = 0;
        const size_t n = ring.size();
        for (const auto& q : ring) { cx += q[0] / (double)n; cy += q[1] / (double)n; }
        double Sxx = 0, Sxy = 0, Syy = 0, Sxz = 0, Syz = 0;
        for (size_t i = 0; i < n; i++) {
            const double dx = ring[i][0] - cx, dy = ring[i][1] - cy;
            const double dz = file_z[i];
            Sxx += dx * dx; Sxy += dx * dy; Syy += dy * dy;
            Sxz += dx * dz; Syz += dy * dz;
        }
        const double det = Sxx * Syy - Sxy * Sxy;
        const double ga = det > 0 ? (Syy * Sxz - Sxy * Syz) / det : 0.0;
        const double gb = det > 0 ? (Sxx * Syz - Sxy * Sxz) / det : 0.0;
        const double tilt =
            std::atan(std::sqrt(ga * ga + gb * gb)) * 180.0 / kRad;
        check(tilt < 1.0, "the base is level with the ground (" +
                              std::to_string(tilt) + " deg tilt)");
    }

    // 2. Without a gauge: up_hint still answers, from the cameras.
    {
        const fs::path d = root / "nogauge";
        write_rig_model(d, 8, false);
        DatasetParserConfig pc;
        ParsedDataset ds = parse_dataset(d.string(), pc, "colmap");
        check(!ds.gauge_oriented, "this model has no gauge");
        PostSplitCameras post = bake_post_split(ds, false, false);
        PointsDoc doc(std::move(ds), post, (d / "sparse/0/points3D.bin").string(),
                      d.string(), nullptr);
        float up[3] = {9, 9, 9};
        check(doc.up_hint(up), "up_hint falls back to the cameras");
        const double len = std::sqrt((double)up[0]*up[0] + up[1]*up[1] + up[2]*up[2]);
        check(std::fabs(len - 1.0) < 1e-5, "the fallback is a unit vector too");
    }

    fs::remove_all(root);
    std::printf("%s\n", g_failures ? "FAILURES" : "all checks passed");
    return g_failures ? 1 : 0;
}
