// sky_dome_doc_test -- the sky dome as the Edit screen performs it
// (src/app/gui/edit/PointsDoc.h): a document over a real reconstruction on disk,
// the dome measured and appended, undo taking it off again, and a save that
// leaves a reconstruction the parser reads back with the dome in it.

#include "app/gui/edit/PointsDoc.h"
#include "data/DatasetParser.h"
#include "data/SceneCenter.h"
#include "data/SparseEdit.h"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <array>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using gui::PointsDoc;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

template <typename T>
void put(std::string& o, T v) {
    o.append(reinterpret_cast<const char*>(&v), sizeof v);
}

// Cameras on a small ring, so the geometric median sits in the middle of them.
constexpr int kCams = 60;
constexpr int kPoints = 900;

void write_model(const fs::path& dir) {
    fs::create_directories(dir / "images");
    for (int i = 0; i < kCams; i++) {   // the parser insists on real files
        std::ofstream img(dir / "images" / ("im" + std::to_string(i) + ".jpg"),
                          std::ios::binary);
        img << "\xff\xd8\xff\xe0";
    }
    {
        std::string s;
        put(s, (uint64_t)1);
        put(s, (uint32_t)1);
        put(s, (int32_t)1);        // PINHOLE
        put(s, (uint64_t)640);
        put(s, (uint64_t)480);
        for (int i = 0; i < 4; i++) put(s, 500.0);
        std::ofstream f(dir / "cameras.bin", std::ios::binary);
        f.write(s.data(), (std::streamsize)s.size());
    }
    {
        // Every camera at a ring position, looking along -Z with identity
        // rotation, so its centre is the translation column.
        std::string s;
        put(s, (uint64_t)kCams);
        for (int i = 0; i < kCams; i++) {
            // A jittered ring. A PERFECT ring would be degenerate: the
            // geometric median of a circle is any point on it, so the centre
            // would land on the ring rather than in the middle of the scene.
            const double a = i * 2.0 * 3.14159265358979 / kCams;
            const double r = 20.0 + 6.0 * std::sin(i * 1.7);
            put(s, (uint32_t)i + 1);
            put(s, 1.0);            // qw
            put(s, 0.0); put(s, 0.0); put(s, 0.0);
            put(s, -r * std::cos(a)); put(s, -r * std::sin(a)); put(s, 2.0);
            put(s, (uint32_t)1);
            std::string name = "im" + std::to_string(i) + ".jpg";
            s += name;
            s += '\0';
            put(s, (uint64_t)1);     // one 2D feature
            put(s, 320.0);
            put(s, 240.0);
            put(s, (uint64_t)(i + 1));
        }
        std::ofstream f(dir / "images.bin", std::ios::binary);
        f.write(s.data(), (std::streamsize)s.size());
    }
    {
        // Ground points at z = 0 inside 30 m, each seen by two images. One
        // floater farthest out, which must not set the radius.
        std::string s;
        put(s, (uint64_t)(kPoints + 1));
        for (int i = 0; i < kPoints; i++) {
            const double a = i * 2.399963;
            const double r = 30.0 * std::sqrt((i + 1) / (double)kPoints);
            put(s, (uint64_t)i + 1);
            put(s, r * std::cos(a));
            put(s, r * std::sin(a));
            put(s, 0.0);
            s.append("\x20\x30\x40", 3);
            put(s, 0.5);
            put(s, (uint64_t)2);
            put(s, (uint32_t)(i % kCams));
            put(s, (uint32_t)0);
            put(s, (uint32_t)((i + 1) % kCams));
            put(s, (uint32_t)0);
        }
        put(s, (uint64_t)(kPoints + 1));
        put(s, 900.0); put(s, 0.0); put(s, 5.0);      // the floater
        s.append("\xff\xff\xff", 3);
        put(s, 1.0);
        put(s, (uint64_t)2);
        put(s, (uint32_t)0); put(s, (uint32_t)0);
        put(s, (uint32_t)1); put(s, (uint32_t)0);
        std::ofstream f(dir / "points3D.bin", std::ios::binary);
        f.write(s.data(), (std::streamsize)s.size());
    }
}

ParsedDataset load(const std::string& dir) {
    DatasetParserConfig pc;
    return parse_dataset(dir, pc, "colmap");
}

// The dome is built in the VIEWPORT's frame -- train_to_normalized_inverse --
// not the parser's centred one, so a test that wants the radius has to measure
// in that same frame or it is comparing two different units.
std::vector<double> to_view(const ParsedDataset& ds, const double* p) {
    double A[16];
    dsparse::train_to_normalized_inverse(ds, A);
    std::vector<double> v(3);
    for (int r = 0; r < 3; r++)
        v[(size_t)r] = A[r*4+0]*p[0] + A[r*4+1]*p[1] + A[r*4+2]*p[2] + A[r*4+3];
    return v;
}

// The live camera centres in the view frame, which is what the dome's centre
// is taken from.
std::vector<double> to_view_points(const ParsedDataset& ds) {
    std::vector<double> out;
    for (int64_t i = 0; i < ds.num_cameras; i++) {
        const double c[3] = {ds.c2w[(size_t)i * 12 + 3], ds.c2w[(size_t)i * 12 + 7],
                             ds.c2w[(size_t)i * 12 + 11]};
        const std::vector<double> v = to_view(ds, c);
        out.insert(out.end(), v.begin(), v.end());
    }
    return out;
}

double percentile_of(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    const double idx = p / 100.0 * (double)(v.size() - 1);
    const size_t lo = (size_t)std::floor(idx), hi = (size_t)std::ceil(idx);
    const double f = idx - (double)lo;
    return v[lo] * (1.0 - f) + v[hi] * f;
}

}  // namespace

int main() {
    const std::string root = "/tmp/sky_dome_doc_test";
    fs::remove_all(root);
    write_model(root);

    ParsedDataset ds = load(root);
    check(ds.num_cameras == kCams, "parsed " + std::to_string(ds.num_cameras) +
                                       " cameras");
    check(ds.points.num() == kPoints + 1, "parsed " +
                                            std::to_string(ds.points.num()) +
                                            " points");
    check((int64_t)ds.points.trackless.size() == ds.points.num(),
          "every point says whether it was triangulated");
    if (!ds.points.trackless.empty()) {
        int64_t none = 0;
        for (uint8_t t : ds.points.trackless) if (!t) none++;
        check(none == ds.points.num(), "all of these have a track");
    }

    const int64_t before_points = ds.points.num();
    const double before_center[3] = {ds.center[0], ds.center[1], ds.center[2]};
    PostSplitCameras post = bake_post_split(ds, false, false);
    // Kept for the expectations below: the document takes its own copy.
    const ParsedDataset expect = ds;
    PointsDoc doc(std::move(ds), post, root + "/sparse/0/points3D.bin", root,
                  nullptr);

    // 1. Measuring changes nothing, and the radius is the p99 of the view-frame
    //    distances -- which is the whole point of the percentile: the 900 m
    //    floater is the maximum and must not be the radius.
    std::string error;
    const std::array<double, 3> centre =
        dsparse::geometric_median(to_view_points(expect).data(), kCams, 3, 0);
    std::vector<double> dists;
    for (int64_t i = 0; i < expect.points.num(); i++) {
        const std::vector<double> v = to_view(expect, &expect.points.xyz[(size_t)i * 3]);
        const double dx = v[0] - centre[0], dy = v[1] - centre[1], dz = v[2] - centre[2];
        dists.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    const double p99 = percentile_of(dists, 99.0);
    const double farthest = *std::max_element(dists.begin(), dists.end());

    PointsDoc::DomeFit probe = doc.measure_sky_dome(5, 99.0, &error);
    check(probe.present, "the dome measures (" + error + ")");
    check(probe.points == 5185, "level 5 carries 5185 points (" +
                                    std::to_string(probe.points) + ")");
    check(std::fabs(probe.radius - p99) < 1e-6 * std::max(1.0, p99),
          "the radius is p99 of the keypoint distances (" +
              std::to_string(probe.radius) + " vs " + std::to_string(p99) + ")");
    check(farthest > 20.0 * p99,
          "and the floater is farthest beyond it (" + std::to_string(farthest) +
              " vs " + std::to_string(p99) + ")");
    check(doc.count() == before_points, "measuring added nothing");
    check(doc.dataset()->points.num() == before_points,
          "measuring left _ds alone");

    // The literal maximum is reachable, and it is the floater.
    PointsDoc::DomeFit at_max = doc.measure_sky_dome(5, 100.0, &error);
    check(at_max.present && std::fabs(at_max.radius - farthest) < 1e-6 * farthest,
          "percentile 100 does reach the floater (" +
              std::to_string(at_max.radius) + " vs " + std::to_string(farthest) + ")");

    // 2. Adding it: one step in the history, and both copies of the cloud grew.
    PointsDoc::DomeFit fit = doc.add_sky_dome(5, 99.0, &error);
    check(fit.present, "the dome was added (" + error + ")");
    check(doc.count() == before_points + fit.points,
          "the layer holds the dome (" + std::to_string(doc.count()) + ")");
    check(doc.dataset()->points.num() == before_points + fit.points,
          "and so does the dataset the save writes");
    check(doc.appended_on(0) == fit.points,
          "the dome is the whole of what was appended");
    check(doc.can_undo(), "it is one undo step");
    check(std::string(doc.undo_name()).find("dome") != std::string::npos ||
              !doc.undo_name().empty(),
          "the step is named (" + doc.undo_name() + ")");

    // Every appended row sits exactly on a sphere and none is below its base.
    // From the LAYER (the frame the dome was built in); height is along the
    // dome's own axis, which is file-up in view coords, not view +Z.
    {
        const float* pos = doc.positions_of(0);
        const std::array<double, 3> ax = doc.file_up_in_view();
        double worst = 0.0, min_z = 1e30;
        int64_t below = 0;
        for (int64_t i = before_points; i < doc.count(); i++) {
            const double dx = pos[i*3+0] - centre[0];
            const double dy = pos[i*3+1] - centre[1];
            const double dz = pos[i*3+2] - centre[2];
            const double h = dx*ax[0] + dy*ax[1] + dz*ax[2];
            worst = std::max(worst, std::fabs(std::sqrt(dx*dx + dy*dy + dz*dz) -
                                              fit.radius));
            min_z = std::min(min_z, h);
            if (h < -1e-6) below++;
        }
        check(below == 0, "no dome point is below the base plane");
        check(worst < 1e-4,
              "the dome is exactly the sphere it measured (" +
                  std::to_string(worst) + ")");
        check(min_z >= -1e-6, "the base is at the centre's height (" +
                                  std::to_string(min_z) + ")");
    }
    // The colour is the sky blue the Metashape script falls back to.
    {
        const ParsedDataset& p = *doc.dataset();
        bool blue = !p.points.rgb.empty();
        for (int64_t i = before_points; blue && i < p.points.num(); i++)
            blue = p.points.rgb[(size_t)i * 3] == 135 &&
                   p.points.rgb[(size_t)i * 3 + 1] == 180 &&
                   p.points.rgb[(size_t)i * 3 + 2] == 220;
        check(blue, "the dome is sky blue");
    }

    // 3. Undo takes it off both the layer and _ds, and leaves the rest alone.
    doc.undo();
    check(doc.count() == before_points, "undo restored the layer (" +
                                            std::to_string(doc.count()) + ")");
    check(doc.dataset()->points.num() == before_points,
          "undo restored the dataset too");
    check(doc.appended_on(0) == 0, "and the appended count");
    doc.redo();
    check(doc.count() == before_points + fit.points, "redo brought it back");
    doc.undo();

    // 4. Save: the reconstruction on disk grows by the dome, with no track.
    doc.redo();
    doc.save(0, root, nullptr);          // target 0 is the folder
    ParsedDataset after = load(root);
    check(after.points.num() == before_points + fit.points,
          "the parser reads back the dome (" +
              std::to_string(after.points.num()) + ")");
    {
        int64_t trackless = 0;
        for (uint8_t t : after.points.trackless)
            if (t) trackless++;
        check(trackless == fit.points,
              "and knows exactly which points are the dome (" +
                  std::to_string(trackless) + ")");
    }
    // The dome landed where the scene is: un-centring must put it back.
    {
        int64_t near_scene = 0;
        for (int64_t i = 0; i < after.points.num(); i++) {
            const double x = after.points.xyz[(size_t)i * 3] + after.center[0];
            const double y = after.points.xyz[(size_t)i * 3 + 1] + after.center[1];
            const double z = after.points.xyz[(size_t)i * 3 + 2] + after.center[2];
            if (std::fabs(z) < 60.0 && std::sqrt(x * x + y * y) < 60.0) near_scene++;
        }
        // The ground points and most of the dome are within 60 m of the file's
        // own origin, which is where the ring was written.
        check(near_scene > kPoints,
              "the dome was written in the file's frame, not the view's");
    }
    // The cameras were not disturbed.
    check(after.num_cameras == kCams, "the cameras are untouched (" +
                                          std::to_string(after.num_cameras) + ")");

    // 5. The same again with a scene centre: a dome in the wrong place still
    //    counts as "5185 points added", and only the radius gives it away.
    {
        // A fresh model: the one above already has a dome in it, and adding a
        // second would be a different test.
        const std::string fresh = root + "_centred";
        fs::remove_all(fresh);
        write_model(fresh);
        // Re-centre by a known amount, so a lost shift cannot cancel out.
        DatasetParserConfig pc;
        pc.center_mode = "point-median";
        ParsedDataset moved = parse_dataset(fresh, pc, "colmap");
        check(std::fabs(moved.center[0]) + std::fabs(moved.center[1]) > 1e-6,
              "the parser centred this scene (" +
                  std::to_string(moved.center[0]) + ")");
        PostSplitCameras mp = bake_post_split(moved, false, false);
        PointsDoc mdoc(std::move(moved), mp, fresh + "/sparse/0/points3D.bin",
                       fresh, nullptr);
        std::string merror;
        PointsDoc::DomeFit mfit = mdoc.add_sky_dome(5, 99.0, &merror);
        check(mfit.present, "the dome is added to a centred scene too (" +
                                merror + ")");
        // In the layer, the dome must be exactly the sphere it measured.
        const float* pos = mdoc.positions_of(0);
        const ParsedDataset& md = *mdoc.dataset();
        const int64_t first = md.num_cameras >= 0 ? before_points : 0;
        (void)first;
        const int64_t base = md.points.num() - mfit.points;
        const std::array<double, 3> mc =
            dsparse::geometric_median(to_view_points(md).data(),
                                      md.num_cameras, 3, 0);
        double worst = 0.0;
        int64_t below = 0;
        // Along the dome's own axis again, not view +Z.
        const std::array<double, 3> m_ax = mdoc.file_up_in_view();
        for (int64_t i = base; i < mdoc.count(); i++) {
            const double dx = pos[i*3+0] - mc[0];
            const double dy = pos[i*3+1] - mc[1];
            const double dz = pos[i*3+2] - mc[2];
            worst = std::max(worst, std::fabs(std::sqrt(dx*dx+dy*dy+dz*dz) -
                                              mfit.radius));
            if (dx * m_ax[0] + dy * m_ax[1] + dz * m_ax[2] < -1e-6) below++;
        }
        check(worst < 1e-4, "and it is still the sphere it measured (" +
                                 std::to_string(worst) + ")");
        check(below == 0, "still entirely above its base");
        // And it survives a save, in the file's frame.
        mdoc.save(0, fresh, nullptr);
        ParsedDataset reread = load(fresh);
        fs::remove_all(fresh);
        check(reread.points.num() == before_points + mfit.points,
              "the centred scene saves the dome (" +
                  std::to_string(reread.points.num()) + ")");
    }

    fs::remove_all(root);
    std::printf("%s\n", g_failures ? "FAILURES" : "all checks passed");
    return g_failures ? 1 : 0;
}