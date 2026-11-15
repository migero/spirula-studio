// The ray-space pair: a view's pixels to the sphere and back, and what that
// buys. A person in rays is one thing wherever it is looked at from, so this
// holds three properties the mask path needs and pixels cannot give:
//
//   * round trip -- ray(ray_pixel(ray(p))) is the pixel it started at, in the
//     view that produced it;
//   * merge -- one ray set is found in every view that can see it, so a person
//     in two faces is ONE detection, not two to reconcile;
//   * split -- a ray set crossing a face boundary still covers every pixel of
//     the object in both faces, with no gap along the seam.
//
// Registered as `pano360_rays_test` in cmake/SsApps.cmake.

#include "app/Pano360.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

using app::Pano360Layout;
using app::Pano360Options;
using app::Pano360Remap;
using app::Pano360View;

Pano360Layout max_layout() {
    Pano360Layout l;
    l.packing = app::Pano360Packing::Eac;
    l.track_w = 4096;
    l.track_h = 1344;
    l.face = 1344;
    l.strip = 32;
    return l;
}

double dot3(const float a[3], const float b[3]) {
    return (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2];
}

// How many of `v`'s pixels `dir` lands inside. A 1 px slop, so a ray exactly on
// the edge is not a coin flip.
int covers(const Pano360View& v, const float dir[3]) {
    float x, y;
    if (!app::pano360_ray_pixel(v, dir, x, y)) return 0;
    const int ix = (int)std::lround(x), iy = (int)std::lround(y);
    if (ix < -1 || iy < -1 || ix > v.width || iy > v.height) return 0;
    return 1;
}

// The canvas is the two tracks reassembled: undo the slice cut to find the track
// coordinate pano360_direction reads, which is the one inverse of a remap.
bool canvas_dir(const Pano360Layout& l, float sx, float sy, float dir[3]) {
    const int row = (int)(sy / (float)l.face);
    if (row < 0 || row > 1) return false;
    for (const app::Eac360Slice& s : app::eac360_slices(l)) {
        const float x0 = (float)s.dst_x, x1 = (float)(s.dst_x + s.width);
        if (sx < x0 || sx >= x1) continue;
        return app::pano360_direction(l, row, (float)s.src_x + (sx - x0),
                                      sy - (float)(row * l.face), dir);
    }
    return false;
}

// Corners: four squares per lens and none on the axis. What matters is the
// invariant -- no pixel of any view may reach past its own lens's 90 degrees,
// or it samples the other lens and stops being a pinhole camera.
void corners(const Pano360Layout& l) {
    Pano360Options o;
    o.mode = app::Pano360Mode::Corners;
    const std::vector<Pano360View> views = app::pano360_views(l, o);
    std::printf("\nCorners (%zu views)\n", views.size());
    check(views.size() == 8, "corners unwraps into eight views");
    bool square = true, named = true;
    for (const Pano360View& v : views) {
        square = square && v.width == v.height;
        named = named && !v.dir.empty();
    }
    check(square, "every corner view is a square");
    check(named, "every corner view is a named view folder");

    // The seam is the plane 90 degrees from a lens's own axis, and each lens's
    // axis is +/-z. Measured over every pixel of every view.
    double worst = 0.0;
    for (size_t m = 0; m < views.size(); m++) {
        const Pano360View& v = views[m];
        // The view's own lens: the first four look down +z, the rest down -z.
        const float sign = m < 4 ? 1.0f : -1.0f;
        for (int j = 0; j <= v.height; j += 7) {
            for (int i = 0; i <= v.width; i += 7) {
                float d[3];
                app::pano360_view_ray(v, (float)i, (float)j, d);
                const double c = (double)d[2] * sign;
                worst = std::max(worst, std::acos(std::max(-1.0, std::min(1.0, c))) *
                                         180.0 / 3.14159265358979323846);
            }
        }
    }
    std::printf("        worst corner pixel: %.6f deg off its lens axis\n", worst);
    check(worst <= 90.0 + 1e-3, "no corner view crosses the lens seam");

    // Round trip, and the inner edge on the axis with the outer on the seam.
    {
        double rt = 0.0;
        for (const Pano360View& v : views)
            for (int j = 3; j < v.height; j += v.height / 5)
                for (int i = 3; i < v.width; i += v.width / 5) {
                    float d[3], px, py;
                    app::pano360_view_ray(v, (float)i, (float)j, d);
                    if (!app::pano360_ray_pixel(v, d, px, py)) {
                        std::printf("  FAIL %s (%d,%d) left the view\n",
                                    v.dir.c_str(), i, j);
                        ++g_failures;
                        continue;
                    }
                    rt = std::max(rt, (double)std::abs(px - (float)i));
                    rt = std::max(rt, (double)std::abs(py - (float)j));
                }
        std::printf("        worst corner round-trip error %.6g px\n", rt);
        check(rt < 0.05, "corner views round-trip through ray space");
    }
    {
        // cam0 is tilted 45 degrees toward +x, so its inner edge midpoint is
        // the lens axis itself and its outer edge and corners are the seam.
        const Pano360View& v = views[0];
        float d[3];
        const double k = 3.14159265358979323846 / 180.0;
        app::pano360_view_ray(v, 0.5f, (float)v.height * 0.5f, d);
        check(std::acos(std::min(1.0f, std::fabs(d[2]))) < 0.05f,
              "a corner view's inner edge is the lens axis");
        app::pano360_view_ray(v, (float)v.width - 0.5f, (float)v.height * 0.5f, d);
        check(std::acos(std::min(1.0f, std::fabs(d[2]))) / k > 89.9 &&
                  std::acos(std::min(1.0f, std::fabs(d[2]))) / k < 90.1,
              "a corner view's outer edge is the seam");
    }
}

// Fisheye: two squares, each lens's whole 180, equidistant. The circle is the
// lens and the square's corners are not, so the plan, the ray pair and the
// remap must all draw that edge in the same place.
void fisheye(const Pano360Layout& l) {
    Pano360Options o;
    o.mode = app::Pano360Mode::Fisheye;
    const std::vector<Pano360View> views = app::pano360_views(l, o);
    std::printf("\nFisheye (%zu views, %dx%d)\n", views.size(),
                views.empty() ? 0 : views[0].width,
                views.empty() ? 0 : views[0].height);
    check(views.size() == 2, "fisheye unwraps into one view per lens");
    if (views.size() != 2) return;
    bool tagged = true, square = true, named = true;
    for (const Pano360View& v : views) {
        tagged = tagged && v.proj == app::Pano360Projection::Fisheye &&
                 v.fov == 180.0f;
        square = square && v.width == v.height;
        named = named && !v.dir.empty();
    }
    check(tagged, "both views are 180 degree fisheyes");
    check(square, "a fisheye view is a square");
    check(named && views[0].width == l.face, "a fisheye view is the face side");

    // Lens 0 looks along +z and lens 1 along -z, the same axis convention as
    // Faces' two lens groups.
    {
        const float c = (views[0].width - 1) * 0.5f;
        float d[3];
        app::pano360_view_ray(views[0], c, c, d);
        check(d[2] > 0.999f && std::fabs(d[0]) < 1e-3f && std::fabs(d[1]) < 1e-3f,
              "cam0 looks down the +z axis");
        app::pano360_view_ray(views[1], c, c, d);
        check(d[2] < -0.999f, "cam1 looks down -z, the other lens");
    }

    // The mid-row edge of the square is the circle, so it is the lens's own 90
    // degrees and no more.
    {
        float d[3];
        app::pano360_view_ray(views[0], 0.0f, (views[0].height - 1) * 0.5f, d);
        const double deg =
            std::acos(std::min(1.0f, d[2])) * 180.0 / 3.14159265358979323846;
        std::printf("        the edge ray is %.4f deg off the axis\n", deg);
        check(deg < 90.0 && deg > 89.5, "the square's edge is the lens's 90 degrees");
    }

    // Round trip inside the circle, refusal outside it: the corners of the
    // square are outside the projection, not a wrapped far edge.
    {
        double worst = 0.0;
        int inside = 0, refused = 0;
        for (const Pano360View& v : views) {
            for (int j = 0; j < v.height; j += 9) {
                for (int i = 0; i < v.width; i += 9) {
                    const double cx = (i + 0.5 - v.width * 0.5) / (v.width * 0.5);
                    const double cy = (j + 0.5 - v.height * 0.5) / (v.height * 0.5);
                    const bool in = std::sqrt(cx * cx + cy * cy) <= 1.0 + 1e-9;
                    float d[3], px, py;
                    app::pano360_view_ray(v, (float)i, (float)j, d);
                    const bool hit = app::pano360_ray_pixel(v, d, px, py);
                    if (in != hit) {
                        std::printf("  FAIL %s (%d,%d) is %s the circle\n",
                                    v.dir.c_str(), i, j, in ? "inside" : "outside");
                        ++g_failures;
                        continue;
                    }
                    if (!hit) {
                        ++refused;
                        continue;
                    }
                    ++inside;
                    worst = std::max(worst, (double)std::abs(px - (float)i));
                    worst = std::max(worst, (double)std::abs(py - (float)j));
                }
            }
        }
        std::printf("        %d facing, %d refused; worst round-trip %.6g px\n",
                    inside, refused, worst);
        check(inside > 0 && worst < 1e-2, "every pixel in the circle round-trips");
        check(refused > 0, "a corner of the square has no ray in the view");
    }

    // The remap and the ray space must name the same sphere: resample each
    // pixel's canvas coordinate back to a direction and compare.
    {
        Pano360Remap map;
        app::pano360_remap(l, views[0], map);
        double worst = 0.0;
        int compared = 0, invalid = 0, wrong = 0;
        for (int j = 1; j < views[0].height; j += 11) {
            for (int i = 1; i < views[0].width; i += 11) {
                const size_t k = (size_t)j * views[0].width + i;
                if (map.x[k] < 0.0f || map.y[k] < 0.0f) {
                    ++invalid;
                    const double cx = (i + 0.5 - views[0].width * 0.5) /
                                      (views[0].width * 0.5);
                    const double cy = (j + 0.5 - views[0].height * 0.5) /
                                      (views[0].height * 0.5);
                    if (std::sqrt(cx * cx + cy * cy) <= 1.0) ++wrong;
                    continue;
                }
                float d[3], back[3];
                app::pano360_view_ray(views[0], (float)i, (float)j, d);
                // The canvas pixel the remap names, not its fraction: that is
                // what the resampler reads.
                if (!canvas_dir(l, std::floor(map.x[k] + 0.5f),
                                std::floor(map.y[k] + 0.5f), back)) {
                    ++wrong;
                    continue;
                }
                ++compared;
                worst = std::max(worst, std::acos(std::min(1.0, dot3(d, back))));
            }
        }
        const double worst_deg = worst * 180.0 / 3.14159265358979323846;
        // A canvas pixel is 90/face degrees; the row clamp at a cell edge is a second.
        const double pitch = 90.0 / l.face;
        std::printf("        remap vs ray: %d compared, %d refused; worst %.6g deg\n",
                    compared, invalid, worst_deg);
        check(wrong == 0, "only pixels outside the circle are refused");
        check(compared > 100 && worst_deg < 2.5 * pitch,
              "the remap lands on the canvas pixel the ray names");
    }

    // What a resampled frame looks like: black outside the circle, the canvas
    // inside it.
    {
        Pano360Remap map;
        app::pano360_remap(l, views[0], map);
        const int cw = l.canvasW(), ch = l.canvasH(), w = views[0].width;
        std::vector<uint8_t> canvas((size_t)cw * ch * 3, 200);
        std::vector<uint8_t> out((size_t)w * w * 3, 7);
        app::pano360_apply(map, canvas.data(), cw, ch, 1, out.data());
        check(out[0] == 0 && out[((size_t)(w - 1) * w + w - 1) * 3] == 0,
              "the square's corners are black");
        check(out[((size_t)(w / 2) * w + w / 2) * 3] == 200,
              "the lens axis reads the canvas");
    }

    // A MAX 2 packs the sphere as a panorama per track; the fisheye plan and its
    // ray space are the packing's business no more than a pinhole face's.
    {
        Pano360Layout s;
        s.packing = app::Pano360Packing::Sphere;
        s.face = 64;
        s.margin = 16;
        s.track_w = 4 * 64 + 32;
        s.track_h = 120;
        const std::vector<Pano360View> sv = app::pano360_views(s, o);
        check(sv.size() == 2 && sv[0].proj == app::Pano360Projection::Fisheye,
              "a panorama pair gets the same two fisheye views");
        if (sv.size() == 2) {
            Pano360Remap map;
            app::pano360_remap(s, sv[0], map);
            double worst = 0.0;
            int inside = 0, refused = 0;
            for (int j = 0; j < sv[0].height; j += 5) {
                for (int i = 0; i < sv[0].width; i += 5) {
                    float d[3], px, py;
                    app::pano360_view_ray(sv[0], (float)i, (float)j, d);
                    if (!app::pano360_ray_pixel(sv[0], d, px, py)) {
                        ++refused;
                        continue;
                    }
                    ++inside;
                    worst = std::max(worst, (double)std::abs(px - (float)i));
                    worst = std::max(worst, (double)std::abs(py - (float)j));
                }
            }
            std::printf("        MAX 2 fisheye: %d facing, %d refused, worst %.6g px\n",
                        inside, refused, worst);
            check(inside > 0 && refused > 0 && worst < 1e-2,
                  "a fisheye's ray space does not care how the sphere is packed");
            check(map.x[0] < 0.0f && map.y[0] < 0.0f,
                  "a panorama's fisheye corners are refused too");
        }
    }
}

// The mask runs on the canvas, so its two failure modes are a halo across a
// face or lens seam and an edge that clamps into the border.
void sharpen(const Pano360Layout& l) {
    std::printf("\nSharpen\n");
    const int w = l.canvasW(), h = l.canvasH();
    std::vector<uint8_t> px((size_t)w * h * 3, 90);
    app::pano360_sharpen(px.data(), w, h, l, 100, 2);
    bool flat = true;
    for (uint8_t v : px) flat = flat && v == 90;
    check(flat, "a flat canvas survives the mask");

    auto fill = [&](std::vector<uint8_t>& c, int from, int to, uint8_t v) {
        for (int y = 0; y < h; y++)
            for (int x = from; x < to; x++)
                for (int k = 0; k < 3; k++)
                    c[((size_t)y * w + x) * 3 + k] = v;
    };
    auto at = [&](const std::vector<uint8_t>& c, int x, int y) {
        return c[((size_t)y * w + x) * 3];
    };

    // An edge inside one face: both sides move, and the clamp keeps them in
    // range.
    std::vector<uint8_t> edge(px.size(), 40);
    fill(edge, w / 2, w, 200);
    app::pano360_sharpen(edge.data(), w, h, l, 100, 2);
    check(at(edge, w / 2, h / 4) == 255 && at(edge, w / 2 - 1, h / 4) == 0,
          "a face's inner edge is boosted on both sides");

    // An edge exactly on a face seam: the tap must not cross it.
    std::vector<uint8_t> seam(px.size(), 40);
    fill(seam, l.face, 2 * l.face, 200);
    app::pano360_sharpen(seam.data(), w, h, l, 100, 2);
    check(at(seam, 2 * l.face, h / 4) == 40 && at(seam, 2 * l.face - 1, h / 4) == 200,
          "a face seam is a tile boundary, not a halo");

    // A panorama wraps, so the first column sees the last one as its left
    // neighbour. At 5% the detail is not clamped and the sum is exact.
    Pano360Layout s;
    s.packing = app::Pano360Packing::Sphere;
    s.face = 64;
    s.track_w = 4 * 64;
    s.track_h = 64;
    const int sw = s.canvasW();
    std::vector<uint8_t> ring((size_t)sw * s.track_h * 3, 40);
    for (int y = 0; y < s.track_h; y++)
        for (int k = 0; k < 3; k++)
            ring[((size_t)y * sw + sw - 1) * 3 + k] = 200;
    app::pano360_sharpen(ring.data(), sw, s.track_h, s, 5, 1);
    check(ring[0] < 40, "a panorama's azimuth seam wraps");
}

}  // namespace

int main() {
    const Pano360Layout l = max_layout();
    Pano360Options o;
    o.mode = app::Pano360Mode::Faces;
    const std::vector<Pano360View> views = app::pano360_views(l, o);

    std::printf("Pano360 ray space (%zu views)\n", views.size());
    check(views.size() == 10, "a .360 unwraps into ten views");

    // ---- round trip, every view ----
    {
        double worst = 0.0;
        for (const Pano360View& v : views) {
            for (int j = 4; j < v.height; j += v.height / 7) {
                for (int i = 4; i < v.width; i += v.width / 7) {
                    float dir[3];
                    app::pano360_view_ray(v, (float)i, (float)j, dir);
                    float x, y;
                    if (!app::pano360_ray_pixel(v, dir, x, y)) {
                        std::printf("  FAIL %s (%d,%d) left the view\n",
                                    v.dir.c_str(), i, j);
                        ++g_failures;
                        continue;
                    }
                    worst = std::max(worst, (double)std::abs(x - (float)i));
                    worst = std::max(worst, (double)std::abs(y - (float)j));
                }
            }
        }
        std::printf("        worst round-trip error %.6g px\n", worst);
        check(worst < 1e-2, "ray -> pixel -> ray returns the same pixel");
    }

    // Which views can see part of which other view. Not hardcoded: the plan
    // moves with --360-size, and a test that names the wrong pair would be
    // testing the wrong thing.
    auto overlapping_pair = [&](int& a_out, int& b_out) {
        for (size_t a = 0; a < views.size(); a++) {
            for (size_t b = 0; b < views.size(); b++) {
                if (a == b) continue;
                const Pano360View& va = views[a];
                const Pano360View& vb = views[b];
                // A run of `a`'s centre row must land in `b`, not one stray pixel: a
                // sharing band is 5 of 33, the 5.9% docs/datasets.md records.
                // Which pairs share moves with --360-size, so search.
                const int step = std::max(1, va.width / 32);
                int hits = 0, span = 0;
                for (int i = 0; i < va.width; i += step) {
                    float dir[3];
                    app::pano360_view_ray(va, (float)i, va.height * 0.5f, dir);
                    float x, y;
                    ++span;
                    if (app::pano360_ray_pixel(vb, dir, x, y)) ++hits;
                }
                if (hits >= 3) {
                    a_out = (int)a;
                    b_out = (int)b;
                    return true;
                }
            }
        }
        return false;
    };

    int pa = -1, pb = -1;
    const bool found = overlapping_pair(pa, pb);
    if (found)
        std::printf("        cam%d and cam%d share part of the sphere\n", pa, pb);
    check(found, "two views overlap, as the 5.9% figure says");

    // ---- one ray set, two views: the merge ----
    if (found) {
        const Pano360View& a = views[pa];
        const Pano360View& b = views[pb];
        // The patch has to sit where the two views actually meet, so it is
        // found by scanning rather than assumed to be the centre.
        std::vector<float> rays;
        int in_b = 0;
        const int half_patch = std::min(a.width, a.height) / 24;
        for (int i = 0; i < a.width && rays.empty(); i += 4) {
            for (int j = 0; j < a.height; j += 4) {
                std::vector<float> cand;
                for (int jj = j - half_patch; jj <= j + half_patch; jj += 4)
                    for (int ii = i - half_patch; ii <= i + half_patch; ii += 4) {
                        if (ii < 0 || jj < 0 || ii >= a.width || jj >= a.height)
                            continue;
                        float d[3];
                        app::pano360_view_ray(a, (float)ii, (float)jj, d);
                        cand.push_back(d[0]);
                        cand.push_back(d[1]);
                        cand.push_back(d[2]);
                    }
                int seen = 0;
                for (size_t k = 0; k + 2 < cand.size(); k += 3)
                    seen += covers(b, &cand[k]);
                // Needs rays in both, and a good share of them in `b`, or the
                // patch is against `a`'s edge rather than across the boundary.
                if (seen > 0 && seen * 2 >= (int)(cand.size() / 3)) {
                    rays = cand;
                    in_b = seen;
                    break;
                }
            }
        }
        check(!rays.empty() && in_b > 0, "a patch across the boundary exists");
        check(rays.size() % 3 == 0 && !rays.empty(), "a patch becomes rays");

        std::printf("        %d of %zu rays land in cam%d\n", in_b, rays.size() / 3,
                    pb);
        check(in_b > 0, "one ray set is found in the other view: no re-detection");

        // ---- split: the same set is whole in BOTH ----
        // Every ray must land in at least one of the two, and neither may be
        // handed a pixel the ray does not own.
        int lost = 0, outside = 0;
        for (size_t k = 0; k + 2 < rays.size(); k += 3) {
            // Separate outputs: `b` would otherwise overwrite the pixel `a`
            // returned, and the bounds below would be checked against it.
            float ax, ay, bx, by;
            const bool ha = app::pano360_ray_pixel(a, &rays[k], ax, ay);
            const bool hb = app::pano360_ray_pixel(b, &rays[k], bx, by);
            if (!ha && !hb) ++lost;
            if (ha && (ax < -0.5f || ay < -0.5f || ax > a.width - 0.5f ||
                       ay > a.height - 0.5f))
                ++outside;
            if (hb && (bx < -0.5f || by < -0.5f || bx > b.width - 0.5f ||
                       by > b.height - 0.5f))
                ++outside;
        }
        check(lost == 0, "no ray is dropped between the two views");
        check(outside == 0, "no ray is wrapped onto a far edge");
    }

    // ---- behind the view, and the sphere seam ----
    {
        const Pano360View& v = views[0];
        float dir[3];
        app::pano360_view_ray(views[5], 0.0f, 0.0f, dir);   // the opposite lens
        float x, y;
        const bool behind = app::pano360_ray_pixel(v, dir, x, y);
        std::printf("        the far lens's axis in cam0: %s\n",
                    behind ? "visible" : "behind cam0");
        // Either answer is defensible for a single ray; what must never happen
        // is a wrapped pixel at the image border.
        if (behind) {
            const int ix = (int)std::lround(x), iy = (int)std::lround(y);
            check(ix >= 0 && iy >= 0 && ix < v.width && iy < v.height,
                  "a ray from the other lens is in frame or refused, never wrapped");
        }

        // The equirect plan, where the longitude seam is real and u wraps.
        Pano360Options eq;
        eq.mode = app::Pano360Mode::Equirect;
        const std::vector<Pano360View> eqv = app::pano360_views(l, eq);
        check(eqv.size() == 1 && eqv[0].fov == 0.0f, "equirect is one view");
        if (!eqv.empty()) {
            double worst = 0.0;
            for (int j = 2; j < eqv[0].height; j += 17)
                for (int i = 0; i < eqv[0].width; i += 53) {
                    float d[3], px, py;
                    app::pano360_view_ray(eqv[0], (float)i, (float)j, d);
                    if (!app::pano360_ray_pixel(eqv[0], d, px, py)) {
                        std::printf("  FAIL equirect (%d,%d) left the view\n", i, j);
                        ++g_failures;
                        continue;
                    }
                    worst = std::max(worst, (double)std::abs(px - (float)i));
                    worst = std::max(worst, (double)std::abs(py - (float)j));
                }
            std::printf("        worst equirect round-trip error %.6g px\n", worst);
            // Float asin near the pole, where its derivative is unbounded: in
            // double this round trip is 6e-11 px. A mask is boolean per pixel.
            check(worst < 0.05, "equirect round-trips across the seam too");
        }
    }

    corners(l);
    fisheye(l);
    sharpen(l);

    std::printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}