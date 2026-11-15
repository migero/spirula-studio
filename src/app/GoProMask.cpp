// app/GoProMask.cpp -- World-Lock polygons into view pixels: densify, project
// through Pano360's exact inverse, fill scanlines inside each visible run.

#include "app/GoProMask.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace app {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxStep = kPi / 120;   // 1.5 degrees of great-circle slerp
constexpr double kJumpFrac = 0.125;      // of a side, where a seam cut may split an edge
constexpr double kCornerOut = 0.25;      // walk corners, this far past the rect edge

double dot3(const GoProVec3& a, const GoProVec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double len3(const GoProVec3& a) { return std::sqrt(dot3(a, a)); }

void unit3(GoProVec3& a) {
    const double n = len3(a);
    if (n > 1e-12) a = GoProVec3{a.x / n, a.y / n, a.z / n};
}

GoProVec3 cross3(const GoProVec3& a, const GoProVec3& b) {
    return GoProVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                     a.x * b.y - a.y * b.x};
}

void sph_lerp(const GoProVec3& a, const GoProVec3& b, double t, GoProVec3& out) {
    const double d = std::fmax(-1., std::fmin(1., dot3(a, b)));
    const double angle = std::acos(d);
    if (angle < 1e-12 || angle > kPi - 1e-7) {
        out = a;
        return;
    }
    const double sa = std::sin(angle);
    const GoProVec3 part{a.x * std::sin((1 - t) * angle) / sa +
                             b.x * std::sin(t * angle) / sa,
                         a.y * std::sin((1 - t) * angle) / sa +
                             b.y * std::sin(t * angle) / sa,
                         a.z * std::sin((1 - t) * angle) / sa +
                             b.z * std::sin(t * angle) / sa};
    out = part;
    unit3(out);
}

void densify_loop(const std::vector<GoProVec3>& loop, std::vector<GoProVec3>& chain) {
    chain.clear();
    for (size_t i = 0; i < loop.size(); i++) {
        GoProVec3 a = loop[i], b = loop[(i + 1) % loop.size()];
        unit3(a);
        unit3(b);
        chain.push_back(a);
        const double angle = std::acos(std::fmax(-1., std::fmin(1., dot3(a, b))));
        if (angle > kMaxStep && angle < kPi) {
            const int steps = (int)std::ceil(angle / kMaxStep);
            GoProVec3 mid;
            for (int k = 1; k < steps; k++) {
                sph_lerp(a, b, (double)k / (double)steps, mid);
                chain.push_back(mid);
            }
        }
    }
}

struct Pixel {
    double x, y;
    bool ok;
};

Pixel project(const Pano360View& view, const double rot[9], const GoProVec3& w) {
    const GoProVec3 r{rot[0] * w.x + rot[1] * w.y + rot[2] * w.z,
                      rot[3] * w.x + rot[4] * w.y + rot[5] * w.z,
                      rot[6] * w.x + rot[7] * w.y + rot[8] * w.z};
    Pixel out{0, 0, false};
    float dir[3] = {(float)r.x, (float)r.y, (float)r.z};
    float x, y;
    out.ok = pano360_ray_pixel(view, dir, x, y);
    if (out.ok) {
        out.x = x;
        out.y = y;
    }
    return out;
}

GoProVec3 rotated(const double rot[9], const GoProVec3& w) {
    return GoProVec3{rot[0] * w.x + rot[1] * w.y + rot[2] * w.z,
                     rot[3] * w.x + rot[4] * w.y + rot[5] * w.z,
                     rot[6] * w.x + rot[7] * w.y + rot[8] * w.z};
}

void push_edge(std::vector<double>& edges, double x0, double y0, double x1, double y1) {
    edges.push_back(x0);
    edges.push_back(y0);
    edges.push_back(x1);
    edges.push_back(y1);
}

void fill_edges(uint8_t* mask, int w, int h, uint8_t value, const std::vector<double>& e) {
    std::vector<double> xs;
    for (int j = 0; j < h; j++) {
        const double yc = j + 0.5;
        xs.clear();
        for (size_t k = 0; k + 3 < e.size(); k += 4) {
            const double y0 = e[k + 1], y1 = e[k + 3];
            if ((y0 <= yc) == (y1 <= yc)) continue;
            xs.push_back(e[k] + (yc - y0) / (y1 - y0) * (e[k + 2] - e[k]));
        }
        if (xs.size() < 2) continue;
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const double lo = xs[k], hi = xs[k + 1];
            if (hi <= lo) continue;
            for (int r = (int)std::floor(-(hi + 0.5) / w) - 1;
                 r <= (int)std::floor((w - lo) / w) + 1; r++) {
                const int i0 = std::min(std::max(0, (int)std::ceil(lo + r * w - 0.5)), w);
                const int i1 = std::min(std::max(0, (int)std::ceil(hi + r * w - 0.5)), w);
                if (i1 > i0)
                    std::memset(mask + (size_t)j * w + i0, value, (size_t)(i1 - i0));
            }
        }
    }
}

// A view's x wraps only where pano360_ray_pixel itself does: equirect (fov 0)
// reads azimuth modulo the width, a pinhole projects injectively over its
// rect -- whatever plan built it, its border is a cut, not a seam.
bool view_wraps(const Pano360View& v) { return v.fov <= 0.0f; }

struct Cross {
    double x, y, t;
};

// First projection failure walking the slerp from the visible end toward
// `away`: the last visible pixel before it, bisected to ~1/1024 of the edge,
// and the fraction where the walk lost the view.
bool crossing_from(const Pano360View& view, const double rot[9], const GoProVec3& vis,
                   const GoProVec3& away, Cross& out) {
    const Pixel start = project(view, rot, vis);
    if (!start.ok) return false;
    constexpr int kSteps = 16;
    double t0 = 0;
    Pixel best = start;
    for (int i = 1; i <= kSteps; i++) {
        GoProVec3 dir;
        sph_lerp(vis, away, (double)i / kSteps, dir);
        const Pixel p = project(view, rot, dir);
        if (p.ok) {
            t0 = (double)i / kSteps;
            best = p;
            continue;
        }
        double lo = t0, hi = (double)i / kSteps;
        for (int k = 0; k < 10; k++) {
            GoProVec3 d2;
            sph_lerp(vis, away, (lo + hi) / 2, d2);
            const Pixel q = project(view, rot, d2);
            if (q.ok) {
                lo = (lo + hi) / 2;
                best = q;
            } else {
                hi = (lo + hi) / 2;
            }
        }
        out.x = best.x;
        out.y = best.y;
        out.t = lo;
        return true;
    }
    return false;
}

// A border sits on a pixel centre, and the fine crossing stops just short of
// it; push the vertices past the border so the boundary pixels the blob
// intersects are filled.
void nudge_border(double& x, double& y, int w, int h) {
    if (x < 1) x -= 0.6;
    if (x > w - 1) x += 0.6;
    if (y < 1) y -= 0.6;
    if (y > h - 1) y += 0.6;
}

constexpr int kSideL = 1, kSideT = 2, kSideR = 4, kSideB = 8;

int border_sides(double x, double y, int w, int h) {
    int s = 0;
    if (x < 0.5) s |= kSideL;
    if (x > w - 0.5) s |= kSideR;
    if (y < 0.5) s |= kSideT;
    if (y > h - 0.5) s |= kSideB;
    if (!s) {
        const double d[4] = {x + 0.5, w - 0.5 - x, y + 0.5, h - 0.5 - y};
        const int p = (int)(std::min_element(d, d + 4) - d);
        s = p == 0 ? kSideL : p == 1 ? kSideR : p == 2 ? kSideT : kSideB;
    }
    return s;
}

// Perimeter stations, clockwise from the top-left corner: corners even, side
// midpoints odd.
int border_station(int s) {
    switch (s) {
        case kSideT: return 1;
        case kSideR: return 3;
        case kSideB: return 5;
        case kSideL: return 7;
        case kSideL | kSideT: return 0;
        case kSideT | kSideR: return 2;
        case kSideR | kSideB: return 4;
        case kSideB | kSideL: return 6;
        default: return -1;
    }
}

void border_corner_xy(int st, int w, int h, double& x, double& y) {
    switch (st) {
        case 0: x = -0.5 - kCornerOut; y = -0.5 - kCornerOut; break;
        case 1: x = (w - 1) * 0.5; y = -0.5 - kCornerOut; break;
        case 2: x = w - 0.5 + kCornerOut; y = -0.5 - kCornerOut; break;
        case 3: x = w - 0.5 + kCornerOut; y = (h - 1) * 0.5; break;
        case 4: x = w - 0.5 + kCornerOut; y = h - 0.5 + kCornerOut; break;
        case 5: x = (w - 1) * 0.5; y = h - 0.5 + kCornerOut; break;
        case 6: x = -0.5 - kCornerOut; y = h - 0.5 + kCornerOut; break;
        default: x = -0.5 - kCornerOut; y = (h - 1) * 0.5; break;
    }
}

// A fisheye's image edge is the lens circle, not the rect: closing along the
// rect border would fill the black corners. The rim is convex, so the arc
// between two crossings stays inside the lens.
void lens_arc(const Pano360View& view, double ex, double ey, double ix, double iy,
              const std::vector<GoProVec3>& remainder, std::vector<double>& edges) {
    const double cx = view.width * 0.5 - 0.5, cy = view.height * 0.5 - 0.5;
    const double radius = view.width * 0.5;
    const double a0 = std::atan2(ey - cy, ex - cx);
    const double a1 = std::atan2(iy - cy, ix - cx);
    double ccw = a1 - a0;
    if (ccw < 0) ccw += 2 * kPi;
    double delta = ccw;   // positive = increasing angle
    if (!remainder.empty()) {
        // Two equally long arcs: the one whose midpoint faces what the person
        // actually does, as the rect closure decides it.
        const double cand[2] = {a0 + ccw * 0.5, a0 - (2 * kPi - ccw) * 0.5};
        double best[2] = {-2, -2};
        for (int k = 0; k < 2; k++) {
            float d[3];
            pano360_view_ray(view, (float)(cx + radius * std::cos(cand[k])),
                             (float)(cy + radius * std::sin(cand[k])), d);
            const GoProVec3 m{d[0], d[1], d[2]};
            for (const GoProVec3& r : remainder) best[k] = std::fmax(best[k], dot3(m, r));
        }
        if (best[1] > best[0]) delta = ccw - 2 * kPi;
    } else if (ccw > kPi) {
        delta = ccw - 2 * kPi;
    }
    const int steps = std::max(1, (int)std::ceil(std::fabs(delta) * radius));
    double px = ex, py = ey;
    for (int k = 1; k < steps; k++) {
        const double a = a0 + delta * (double)k / steps;
        const double x = cx + radius * std::cos(a), y = cy + radius * std::sin(a);
        push_edge(edges, px, py, x, y);
        px = x;
        py = y;
    }
    push_edge(edges, px, py, ix, iy);
}

// Closed between two border crossings along the rectangle border -- a fisheye's
// lens rim: the same edge joins straight, adjacent through their shared corner,
// opposite along the arc whose unprojected midpoint best matches the remainder.
void border_path(const Pano360View& view, const double rot[9], int w, int h,
                 double ex, double ey, double ix, double iy,
                 const std::vector<GoProVec3>& remainder, std::vector<double>& edges) {
    if (view.proj == Pano360Projection::Fisheye) {
        lens_arc(view, ex, ey, ix, iy, remainder, edges);
        return;
    }
    const int te = border_station(border_sides(ex, ey, w, h));
    const int ti = border_station(border_sides(ix, iy, w, h));
    if (te < 0 || ti < 0) {
        push_edge(edges, ex, ey, ix, iy);
        return;
    }
    const int span = (ti - te + 8) % 8;
    bool via_cw;
    if (span < 4) {
        via_cw = true;
    } else if (span > 4) {
        via_cw = false;
    } else if (remainder.empty()) {
        via_cw = true;
    } else {
        // Two equally long arcs: pick the one that covers what the person
        // actually does.
        double m1x, m1y, m2x, m2y;
        border_corner_xy((te + 2) % 8, w, h, m1x, m1y);
        border_corner_xy((te + 6) % 8, w, h, m2x, m2y);
        float d1[3], d2[3];
        pano360_view_ray(view, (float)m1x, (float)m1y, d1);
        pano360_view_ray(view, (float)m2x, (float)m2y, d2);
        double s1 = -2, s2 = -2;
        for (const GoProVec3& r : remainder) {
            s1 = std::fmax(s1, dot3(GoProVec3{d1[0], d1[1], d1[2]}, r));
            s2 = std::fmax(s2, dot3(GoProVec3{d2[0], d2[1], d2[2]}, r));
        }
        via_cw = s1 >= s2;
    }
    // The walk runs forward through the perimeter stations from `from`; the
    // edge set is direction-free, so the shorter arc is what matters.
    const int from = via_cw ? te : ti;
    const int steps = via_cw ? span : 8 - span;
    double px = via_cw ? ex : ix, py = via_cw ? ey : iy;
    const double fx = via_cw ? ix : ex, fy = via_cw ? iy : ey;
    for (int d = 1; d < steps; d++) {
        const int st = (from + d) % 8;
        if (st % 2) continue;
        double cx, cy;
        border_corner_xy(st, w, h, cx, cy);
        push_edge(edges, px, py, cx, cy);
        px = cx;
        py = cy;
    }
    push_edge(edges, px, py, fx, fy);
}

// Between two projected-inside points a slerp stays inside: gnomonic maps
// great circles to straight lines and the rect is convex, so the border cut
// below only guards rounding right at the boundary.
void subdivide(const Pano360View& view, const double rot[9], bool wrap, const GoProVec3& a,
               const GoProVec3& b, double ax, double ay, double bx, double by,
               double thresh, int depth, std::vector<double>& edges) {
    const double jump = std::hypot(bx - ax, by - ay);
    if (jump <= thresh) {
        push_edge(edges, ax, ay, wrap ? ax + std::remainder(bx - ax, (double)view.width) : bx,
                  by);
        return;
    }
    GoProVec3 mid;
    sph_lerp(a, b, .5, mid);
    const Pixel m = project(view, rot, mid);
    if (m.ok) {
        const double mx = wrap ? ax + std::remainder(m.x - ax, (double)view.width) : m.x;
        subdivide(view, rot, wrap, a, mid, ax, ay, mx, m.y, thresh, depth - 1, edges);
        subdivide(view, rot, wrap, mid, b, mx, m.y, bx, by, thresh, depth - 1, edges);
        return;
    }
    if (depth <= 0) {
        push_edge(edges, ax, ay, wrap ? ax + std::remainder(bx - ax, (double)view.width) : bx,
                  by);
        return;
    }
    Cross out, in;
    if (crossing_from(view, rot, a, b, out) && crossing_from(view, rot, b, a, in) &&
        out.t < 1.0 - in.t) {
        nudge_border(out.x, out.y, view.width, view.height);
        nudge_border(in.x, in.y, view.width, view.height);
        // Between the crossings the ring is outside its own fill: the closure
        // hugs the border it left through.
        std::vector<GoProVec3> remainder;
        GoProVec3 dir;
        for (int s = 1; s < 7; s++) {
            sph_lerp(a, b, out.t + (1.0 - in.t - out.t) * s / 7.0, dir);
            remainder.push_back(rotated(rot, dir));
        }
        push_edge(edges, ax, ay, out.x, out.y);
        border_path(view, rot, view.width, view.height, out.x, out.y, in.x, in.y,
                    remainder, edges);
        push_edge(edges, in.x, in.y, bx, by);
        return;
    }
    push_edge(edges, ax, ay, wrap ? ax + std::remainder(bx - ax, (double)view.width) : bx,
              by);
}

}  // namespace

std::vector<float> gopro_target_rays(const Pano360Layout& l, Pano360View view, int w, int h) {
    view.width = w;
    view.height = h;
    Pano360Remap map;
    pano360_remap(l, view, map);
    std::vector<float> rays((size_t)w * h * 3);
    // Match existing Spirula pixels through their source coordinates, including its EAC seam mapping.
    for (size_t i = 0; i < map.x.size(); i++) {
        double x = map.x[i] + .5, y = map.y[i] + .5;
        int row = y >= l.track_h;
        y -= row * l.track_h;
        double raw = x;
        if (x >= l.face / 2.) raw += l.strip;
        if (x >= 2.5 * l.face) raw += l.strip;
        double stored = (l.track_w - l.track_h) / 2., span = stored - 64, u, v;
        int cell;
        if (raw >= stored && raw < stored + l.track_h) {
            if (!row) {
                cell = 1;
                u = (raw - stored) / l.track_h;
                v = y / l.track_h;
            } else {
                cell = 4;
                u = y / l.track_h;
                v = 1 - (raw - stored) / l.track_h;
            }
        } else {
            int right = raw >= stored + l.track_h;
            double local = raw - (right ? l.track_w - stored : 0);
            double sx = (local - (local >= stored * .5 ? 64 : 0)) / span, sy = y / l.track_h;
            if (!row) {
                cell = right ? 2 : 0;
                u = sx;
                v = sy;
            } else {
                cell = right ? 5 : 3;
                u = 1 - sy;
                v = sx;
            }
        }
        u = std::tan((u - .5) * 1.5707963267948966);
        v = std::tan((v - .5) * 1.5707963267948966);
        double a = 0, b = 0, c = 0;
        switch (cell) {
            case 0: a = -1; b = -v; c = u; break;
            case 1: a = u; b = -v; c = 1; break;
            case 2: a = 1; b = -v; c = -u; break;
            case 3: a = u; b = -1; c = -v; break;
            case 4: a = -u; b = -v; c = -1; break;
            default: a = u; b = 1; c = v; break;
        }
        double norm = std::sqrt(a * a + b * b + c * c);
        rays[i * 3] = a / norm;
        rays[i * 3 + 1] = -b / norm;
        rays[i * 3 + 2] = c / norm;
    }
    return rays;
}

bool gopro_mask_polygon(uint8_t* mask, int w, int h, const Pano360View& view,
                        const std::vector<GoProVec3>& world_rays,
                        const double (&world_rot)[9], uint8_t value,
                        std::string& error) {
    if (!mask || w <= 0 || h <= 0) {
        error = "GoPro view mask has no pixels";
        return false;
    }
    // The seam handling wraps at w and projects through `view`; two widths
    // would disagree at the seam.
    if (w != view.width || h != view.height) {
        error = "GoPro view mask does not match its view";
        return false;
    }
    if (world_rays.size() < 3) {
        error = "GoPro tracking polygon needs 3 or more rays";
        return false;
    }
    for (const double r : world_rot)
        if (!std::isfinite(r)) {
            error = "Nonfinite GoPro World-Lock rotation";
            return false;
        }
    std::vector<GoProVec3> chain;
    densify_loop(world_rays, chain);
    if (chain.empty()) {
        error = "GoPro tracking polygon has no rays";
        return false;
    }
    const bool wrap = view_wraps(view);
    std::vector<Pixel> px(chain.size());
    for (size_t i = 0; i < chain.size(); i++) px[i] = project(view, world_rot, chain[i]);
    // Unwrap the ring's x onto one width-window only where the projection
    // itself wraps: a pinhole's border is a cut, and folding it would tear.
    if (wrap) {
        for (size_t k = 1; k < chain.size(); k++) {
            const double dx = std::remainder(px[k].x - px[k - 1].x, (double)w);
            px[k].x = px[k - 1].x + dx;
        }
    }

    const double thresh = kJumpFrac * std::max(w, h);
    std::vector<double> edges;
    const size_t n = chain.size();
    const int depth = 8;
    bool any_out = false;
    for (const Pixel& p : px) any_out = any_out || !p.ok;
    if (!any_out) {
        for (size_t k = 1; k < n; k++)
            subdivide(view, world_rot, wrap, chain[k - 1], chain[k], px[k - 1].x,
                      px[k - 1].y, px[k].x, px[k].y, thresh, depth, edges);
        subdivide(view, world_rot, wrap, chain[n - 1], chain[0], px[n - 1].x,
                  px[n - 1].y, px[0].x, px[0].y, thresh, depth, edges);
        fill_edges(mask, w, h, value, edges);
        return true;
    }

    // Visible runs of the ring.
    size_t out0 = 0;
    while (out0 < n && px[out0].ok) out0++;
    std::vector<char> keep(n, 0);
    size_t run = SIZE_MAX;
    for (size_t j = out0 + 1; j <= out0 + n; j++) {
        const size_t k = j % n;
        if (j < out0 + n && px[k].ok) {
            if (run == SIZE_MAX) run = j;
            continue;
        }
        if (run == SIZE_MAX) continue;
        for (size_t q = run; q < j; q++) keep[q % n] = 1;
        run = SIZE_MAX;
    }
    size_t first = n;
    for (size_t i = 0; i < n; i++)
        if (keep[i]) {
            first = i;
            break;
        }
    if (first == n) return true;

    // One closed walk over the ring: each visible stretch runs to its border
    // crossing and hops along the border to the next stretch's entry, so the
    // fill is the person inside the rect and nothing else.
    double pending_x = 0, pending_y = 0;
    size_t pending_idx = SIZE_MAX;
    std::vector<GoProVec3> remainder;
    for (size_t step = 0; step <= n; step++) {
        const size_t cur = (first + step) % n;
        const size_t prev = (cur + n - 1) % n;
        if (keep[prev] && keep[cur]) {
            subdivide(view, world_rot, wrap, chain[prev], chain[cur], px[prev].x,
                      px[prev].y, px[cur].x, px[cur].y, thresh, depth, edges);
            continue;
        }
        Cross c;
        if (keep[prev] && !keep[cur]) {
            if (crossing_from(view, world_rot, chain[prev], chain[cur], c)) {
                nudge_border(c.x, c.y, w, h);
                pending_idx = cur;
            } else {
                pending_idx = prev;
                c.x = px[prev].x;
                c.y = px[prev].y;
            }
            push_edge(edges, px[prev].x, px[prev].y, c.x, c.y);
            pending_x = c.x;
            pending_y = c.y;
            continue;
        }
        if (!keep[prev] && keep[cur]) {
            if (crossing_from(view, world_rot, chain[cur], chain[prev], c)) {
                nudge_border(c.x, c.y, w, h);
            } else {
                c.x = px[cur].x;
                c.y = px[cur].y;
            }
            push_edge(edges, c.x, c.y, px[cur].x, px[cur].y);
            if (pending_idx != SIZE_MAX) {
                remainder.clear();
                // The ring's own invisible directions between the two
                // crossings pick the border arc when one is ambiguous.
                size_t count = 0;
                for (size_t i = pending_idx; i != cur; i = (i + 1) % n) count++;
                const size_t stride = count > 64 ? (count + 63) / 64 : 1;
                size_t s = 0;
                for (size_t i = pending_idx; i != cur; i = (i + 1) % n, s++)
                    if (s % stride == 0) remainder.push_back(rotated(world_rot, chain[i]));
                border_path(view, world_rot, w, h, pending_x, pending_y, c.x, c.y,
                            remainder, edges);
            }
            pending_idx = SIZE_MAX;
        }
    }
    fill_edges(mask, w, h, value, edges);
    return true;
}

bool gopro_mask_frame(const Pano360View& view, int w, int h, const GoProBinFrame& frame,
                      uint8_t* mask, std::string& error) {
    // The bin's rays sit in the player's y-up camera frame; the view's pixels
    // are y-down, so every ray (and the stored shadow oval with them) mirrors
    // in y.
    double rot[9] = {1, 0, 0, 0, -1, 0, 0, 0, 1};
    for (const GoProBinPerson& person : frame.people) {
        if (person.points.size() >= 3 &&
            !gopro_mask_polygon(mask, w, h, view, person.points, rot, 0, error))
            return false;
        if (person.oval.size() >= 3 &&
            !gopro_mask_polygon(mask, w, h, view, person.oval, rot, 0, error))
            return false;
    }
    return true;
}

}  // namespace app
