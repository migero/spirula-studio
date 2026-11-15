// A person as rays: mask to rays, rays back into every mask that can see them,
// and the merge that keeps one person one person. Each property below is one a
// per-face mask cannot hold -- round trip, split at a face boundary, merge of
// one person seen twice, rejection of two people, and the view gate.

#include "app/PersonRays.h"

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
using app::Pano360View;
using app::PersonRays;

// A solid rectangle of "person" pixels on a view, as the mask convention has it:
// 255 = KEEP, so a person masked OUT is 0.
std::vector<uint8_t> person_box(int w, int h, int x0, int y0, int x1, int y1) {
    std::vector<uint8_t> m((size_t)w * h, 255);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (x >= 0 && y >= 0 && x < w && y < h) m[(size_t)y * w + x] = 0;
    return m;
}

// person_rays_to_mask marks the pixels a ray lands in, so a corner outside the
// lens must stay 0 whatever the person does: no ray can name it.
int outside_circle(const std::vector<uint8_t>& m, int w, int h) {
    int n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double dx = (x + 0.5 - w * 0.5) / (w * 0.5);
            const double dy = (y + 0.5 - h * 0.5) / (h * 0.5);
            // A raster quantises to pixel centres, so the rim itself is a band.
            if (std::sqrt(dx * dx + dy * dy) > 1.0 + 2.0 / w && m[(size_t)y * w + x]) ++n;
        }
    return n;
}

// Fisheye: two squares, the circle being the image edge. A person on the lens
// seam reaches the other lens, and no corner of either square -- which no ray
// reaches -- may ever be marked as him.
void fisheye(const Pano360Layout& l) {
    Pano360Options o;
    o.mode = app::Pano360Mode::Fisheye;
    // The geometry does not depend on the side, and a mask is per-pixel.
    o.size = 256;
    const std::vector<Pano360View> views = app::pano360_views(l, o);
    std::printf("\nFisheye (%zu views, %dx%d)\n", views.size(),
                views.empty() ? 0 : views[0].width,
                views.empty() ? 0 : views[0].height);
    check(views.size() == 2, "fisheye is one view per lens");
    if (views.size() != 2) return;
    const int w = views[0].width, h = views[0].height;

    // A person at cam0's lens axis.
    {
        const auto m = person_box(w, h, w / 2 - 12, h / 2 - 12, w / 2 + 12, h / 2 + 12);
        PersonRays axis{app::person_rays_from_mask(views[0], m.data(), w, h, 4, false), 0};
        check(!axis.empty(), "a fisheye mask becomes rays");
        std::vector<uint8_t> in0, in1;
        app::person_rays_to_mask(axis, views[0], w, h, in0, 4);
        app::person_rays_to_mask(axis, views[1], w, h, in1, 4);
        check(in0[(size_t)(h / 2) * w + w / 2] != 0,
              "the person is in the view he was found in");
        check(std::count(in1.begin(), in1.end(), (uint8_t)255) == 0,
              "and in neither other lens: that one looks the other way");
        check(outside_circle(in0, w, h) == 0 && outside_circle(in1, w, h) == 0,
              "a corner outside the circle is never the person");
    }

    // The lenses partition the sphere: the seam is 90 degrees from both, so only
    // what a mask marks PAST the circle -- a resampled frame's black corners --
    // reaches cam1, and it lands on cam1's own circle edge.
    {
        const int r = w / 2;
        const int x0 = w / 2 + (int)(0.55 * r), x1 = w / 2 + (int)(0.88 * r);
        const int y0 = h / 2 - (int)(0.90 * r), y1 = h / 2 - (int)(0.55 * r);
        const auto m = person_box(w, h, x0, y0, x1, y1);
        PersonRays seam{app::person_rays_from_mask(views[0], m.data(), w, h, 1, false), 0};
        check(!seam.empty(), "a person over the circle edge becomes rays");
        std::vector<uint8_t> in0, in1;
        // Stride 1: a stride's bridging reach would spill pixels over the rim,
        // which is the raster's business, not the ray space's.
        app::person_rays_to_mask(seam, views[0], w, h, in0, 1);
        app::person_rays_to_mask(seam, views[1], w, h, in1, 1);
        const int n0 = (int)std::count(in0.begin(), in0.end(), (uint8_t)255);
        const int n1 = (int)std::count(in1.begin(), in1.end(), (uint8_t)255);
        std::printf("        rim person: %d px in cam0, %d in cam1\n", n0, n1);
        check(n0 > 0, "the part inside the circle is in his own view");
        check(n1 > 0, "and the part past the seam is in the other lens");
        check(outside_circle(in0, w, h) == 0 && outside_circle(in1, w, h) == 0,
              "neither lens marks a corner for him");

        // What crossed lands at the angle the direction names in cam1: near its
        // rim, not anywhere in its square.
        int landed = 0, middle = 0;
        double closest = 1e9;
        for (size_t k = 0; k + 2 < seam.dirs.size(); k += 3) {
            float x, y;
            if (!app::pano360_ray_pixel(views[1], &seam.dirs[k], x, y)) continue;
            ++landed;
            const double dx = x + 0.5 - w * 0.5, dy = y + 0.5 - h * 0.5;
            const double rad = std::sqrt(dx * dx + dy * dy);
            closest = std::min(closest, rad / r);
            if (rad < 0.7 * r) ++middle;
        }
        std::printf("        %d rays in cam1, the nearest at %.3f of its radius\n", landed,
                    landed ? closest : 0.0);
        check(landed > 0 && middle == 0, "and lands on cam1's circle edge");
    }
}

}  // namespace

int main() {
    Pano360Layout l;
    l.packing = app::Pano360Packing::Eac;
    l.track_w = 4096;
    l.track_h = 1344;
    l.face = 1344;
    l.strip = 32;
    Pano360Options o;
    o.mode = app::Pano360Mode::Faces;
    const std::vector<Pano360View> views = app::pano360_views(l, o);

    std::printf("Person rays over %zu views\n", views.size());

    // ---- round trip ----
    {
        const Pano360View& v = views[0];
        const int w = 240, h = 240;
        const auto m = person_box(w, h, 100, 100, 139, 139);
        const std::vector<float> rays =
            app::person_rays_from_mask(v, m.data(), w, h, 4, false);
        check(!rays.empty(), "a masked box becomes rays");

        std::vector<uint8_t> back;
        app::person_rays_to_mask({rays, 0}, v, w, h, back, 4);
        // every masked pixel back, and nothing far away
        // Every pixel the box covered must be covered again.
        int missed = 0, total = 0;
        for (int y = 100; y <= 139; y++)
            for (int x = 100; x <= 139; x++) {
                ++total;
                if (!back[(size_t)y * w + x]) ++missed;
            }
        check(missed == 0, "every masked pixel comes back covered");
        // And nothing far away does.
        int spilled = 0;
        for (int y = 0; y < 60; y++)
            for (int x = 0; x < 60; x++)
                if (back[(size_t)y * w + x]) ++spilled;
        check(spilled == 0, "and nothing outside it does");
    }

    // ---- split across a face boundary ----
    {
        // Every view's centre pixel is exclusive to it (measured, all ten), and only
        // SIDE views share: cam1/cam2 with cam3/cam4, cam6/cam7 with cam8/cam9,
        // while the axial cam0/cam5 share with nothing. Searched from cam1.
        const size_t a = 1;
        int other = -1, best_x = -1;
        for (size_t b = 0; b < views.size() && other < 0; b++) {
            if (b == a) continue;
            for (int i = 0; i < views[a].width; i += 4) {
                float d[3];
                app::pano360_view_ray(views[a], (float)i, views[a].height * 0.5f, d);
                float x, y;
                if (app::pano360_ray_pixel(views[b], d, x, y)) {
                    other = (int)b;
                    best_x = i;
                    break;
                }
            }
        }
        std::printf("        cam%zu shares a band with cam%d at x=%d\n", a, other,
                    best_x);
        check(other >= 0, "a side view shares part of the sphere with another");
        if (other >= 0) {
            // The shared band is a run at one END of the row -- measured, cam1's
            // right 239 px, which land in cam3's own pixels -- so find its centre
            // rather than assume where it is, and keep the box inside it.
            int lo = views[a].width, hi = -1;
            for (int i = 0; i < views[a].width; i++) {
                float d[3];
                app::pano360_view_ray(views[a], (float)i, views[a].height * 0.5f, d);
                float x, y;
                if (app::pano360_ray_pixel(views[other], d, x, y)) {
                    lo = std::min(lo, i);
                    hi = std::max(hi, i);
                }
            }
            check(hi >= lo, "the shared band has a width");
            // Native size: person_rays_from_mask reads mask coordinates AS
            // the view's, so a 400 px mask over a 1628 px view describes only
            // its left quarter and the far band is empty.
            const int w = views[a].width, h = views[a].height;
            const int half = std::max(8, (hi - lo) / 4);
            const int mid = (lo + hi) / 2;
            const auto m = person_box(w, h, mid - half, h / 2 - half, mid + half,
                                      h / 2 + half);
            PersonRays p{
                app::person_rays_from_mask(views[a], m.data(), w, h, 4, false), 0};
            std::vector<uint8_t> inA, inB;
            app::person_rays_to_mask(p, views[a], w, h, inA, 4);
            app::person_rays_to_mask(p, views[other], w, h, inB, 4);
            const int nA = (int)std::count(inA.begin(), inA.end(), (uint8_t)255);
            const int nB = (int)std::count(inB.begin(), inB.end(), (uint8_t)255);
            std::printf("        band x[%d,%d] of %d: %d px in cam%zu, %d in cam%d\n",
                        lo, hi, w, nA, a, nB, other);
            check(nA > 0, "the person is in the view he was found in");
            check(nB > 0, "and in the other view that can see him: not half a mask");
        }
    }

    // ---- merge, and the rejection that keeps it honest ----
    {
        const int w = 200, h = 200;
        // Two people on cam0: one at the centre, one in a corner far away.
        const auto m = person_box(w, h, 90, 90, 109, 109);
        PersonRays a{app::person_rays_from_mask(views[0], m.data(), w, h, 4, false), 0};

        // The same person, seen again from the other view it appears in.
        std::vector<uint8_t> in0;
        app::person_rays_to_mask(a, views[0], w, h, in0, 4);
        std::vector<float> again = app::person_rays_from_mask(views[0], in0.data(), w, h,
                                                             4, false);
        PersonRays b{again, 0};
        const size_t before = a.size();
        check(app::person_rays_merge(a, b, 0.98), "one person twice is one person");
        check(a.size() > before, "and the merge unions the rays rather than dropping them");

        // A different person, far away in direction: must NOT merge.
        const auto m2 = person_box(w, h, 5, 5, 25, 25);
        PersonRays other{app::person_rays_from_mask(views[5], m2.data(), w, h, 4, false),
                         0};
        const size_t keep = a.size();
        check(!app::person_rays_merge(a, other, 0.98),
              "two people apart stay two: no invented person");
        check(a.size() == keep, "and the rejected one adds nothing");
    }

    // ---- gate: which views a person can be in ----
    {
        const int w = 400, h = 400;
        // A person at the centre of cam0, and one at the nadir.
        const auto top = person_box(w, h, 190, 190, 209, 209);
        PersonRays axis{app::person_rays_from_mask(views[0], top.data(), w, h, 4, false), 0};
        const std::vector<int> sees = app::person_visible_views(axis, views);
        std::printf("        a person on cam0's axis is in %zu views\n", sees.size());
        check(!sees.empty(), "some view sees him");
        check(std::find(sees.begin(), sees.end(), 0) != sees.end(),
              "including the one he was found in");

        // A person at the nadir, where the operator of a handheld camera is.
        const Pano360View& down = views[2];
        const auto low = person_box(down.width, down.height, down.width / 2 - 10,
                                    down.height / 2 - 10, down.width / 2 + 10,
                                    down.height / 2 + 10);
        PersonRays nadir{
            app::person_rays_from_mask(down, low.data(), down.width, down.height, 4,
                                       false),
            0};
        const std::vector<int> lowsees = app::person_visible_views(nadir, views);
        std::printf("        a nadir person is in %zu views: ", lowsees.size());
        for (int k : lowsees) std::printf("cam%d ", k);
        std::printf("\n");
        check(!lowsees.empty(), "the nadir person is found somewhere");
        check(std::find(lowsees.begin(), lowsees.end(), 2) != lowsees.end(),
              "including the downward view he was found in");
    }

    fisheye(l);

    std::printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}