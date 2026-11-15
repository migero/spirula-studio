#include "data/SkyDome.h"

#include "data/SceneCenter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

// The 12 vertices of an icosahedron and its 20 faces, as in the Metashape
// script this replaces. Subdivision normalises every midpoint back onto the
// sphere, so the mesh is a geodesic dome, not a faceted one.
const double kIcosaVerts[12][3] = {
    {-1, 0.6180339887498949, 0}, {1, 0.6180339887498949, 0},
    {-1, -0.6180339887498949, 0}, {1, -0.6180339887498949, 0},
    {0, -1, 0.6180339887498949}, {0, 1, 0.6180339887498949},
    {0, -1, -0.6180339887498949}, {0, 1, -0.6180339887498949},
    {0.6180339887498949, 0, -1}, {0.6180339887498949, 0, 1},
    {-0.6180339887498949, 0, -1}, {-0.6180339887498949, 0, 1}};
const int kIcosaFaces[20][3] = {
    {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
    {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
    {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
    {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};

void normalize(double v[3]) {
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1e-10) { v[0] /= n; v[1] /= n; v[2] /= n; }
}

// Unit vectors of the upper half of a subdivided icosphere. The count is a
    // shade over half the sphere, not half of it: the icosahedron has four
    // vertices on the equator and so is not top/bottom symmetric.
void icosphere_upper(int subdivisions, std::vector<std::array<double, 3>>& out) {
    if (subdivisions < 0 || subdivisions > 7)
        throw std::runtime_error("sky dome subdivisions must be 0..7");
    std::vector<std::array<double, 3>> verts;
    verts.reserve((size_t)(10 * (1 << (2 * subdivisions)) + 2));
    for (const auto& v : kIcosaVerts) {
        double t[3] = {v[0], v[1], v[2]};
        normalize(t);
        verts.push_back({t[0], t[1], t[2]});
    }
    std::vector<std::array<int, 3>> faces;
    faces.reserve((size_t)(20 * (1 << (2 * subdivisions))));
    for (const auto& f : kIcosaFaces) faces.push_back({f[0], f[1], f[2]});

    for (int s = 0; s < subdivisions; s++) {
        std::vector<std::array<int, 3>> next;
        next.reserve(faces.size() * 4);
        // Midpoint cache, so a shared edge contributes one vertex.
        std::vector<std::pair<std::pair<int, int>, int>> mid;
        auto midpoint = [&](int a, int b) {
            const std::pair<int, int> key{std::min(a, b), std::max(a, b)};
            for (const auto& m : mid)
                if (m.first == key) return m.second;
            double t[3] = {(verts[(size_t)a][0] + verts[(size_t)b][0]) * 0.5,
                           (verts[(size_t)a][1] + verts[(size_t)b][1]) * 0.5,
                           (verts[(size_t)a][2] + verts[(size_t)b][2]) * 0.5};
            normalize(t);
            verts.push_back({t[0], t[1], t[2]});
            mid.push_back({key, (int)verts.size() - 1});
            return (int)verts.size() - 1;
        };
        for (const auto& f : faces) {
            const int ab = midpoint(f[0], f[1]);
            const int bc = midpoint(f[1], f[2]);
            const int ca = midpoint(f[2], f[0]);
            next.push_back({f[0], ab, ca});
            next.push_back({f[1], bc, ab});
            next.push_back({f[2], ca, bc});
            next.push_back({ab, bc, ca});
        }
        faces.swap(next);
    }

    out.clear();
    out.reserve(verts.size() / 2 + 1);
    for (const auto& v : verts)
        if (v[2] >= -1e-12) out.push_back(v);
}

// The p-th percentile of `v` (0..100), by the nearest-rank convention on the
// sorted values. Sorts a copy.
double percentile_of(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    p = std::clamp(p, 0.0, 100.0);
    std::sort(v.begin(), v.end());
    const double idx = p / 100.0 * (double)(v.size() - 1);
    const size_t lo = (size_t)std::floor(idx), hi = (size_t)std::ceil(idx);
    const double frac = idx - (double)lo;
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

}  // namespace

ColmapPoints3D sky_dome_points(const SkyDomeConfig& cfg,
                               const std::vector<double>& cam_centres,
                               const std::vector<uint8_t>* alive,
                               const double* points, int64_t n,
                               const double up[3], SkyDomeFit* fit) {
    SkyDomeFit out;

    // The centre is the cameras' geometric median, so a few distant rig members
    // cannot drag the dome off to one side.
    if (cam_centres.size() < 9)
        throw std::runtime_error("sky dome needs at least 3 camera positions");
    out.center = dsparse::geometric_median(cam_centres.data(),
                                           (int64_t)(cam_centres.size() / 3), 3, 0);

    // The radius: a percentile of the live keypoint distances, so a floater
    // cannot inflate the dome. The literal maximum stays reachable at 100.
    std::vector<double> dist;
    dist.reserve((size_t)std::max<int64_t>(n, 0));
    for (int64_t i = 0; i < n; i++) {
        if (alive && (size_t)i < alive->size() && !(*alive)[(size_t)i]) continue;
        const double dx = points[i * 3 + 0] - out.center[0];
        const double dy = points[i * 3 + 1] - out.center[1];
        const double dz = points[i * 3 + 2] - out.center[2];
        dist.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    if (dist.size() < 2)
        throw std::runtime_error("sky dome needs at least 2 live keypoints");
    const double p_radius = percentile_of(dist, cfg.percentile);
    const double max_radius = *std::max_element(dist.begin(), dist.end());
    out.radius = std::min(p_radius, max_radius);
    out.percentile_radius = p_radius;
    out.max_radius = max_radius;

    std::vector<std::array<double, 3>> unit;
    icosphere_upper(cfg.subdivisions, unit);
    if (unit.empty()) throw std::runtime_error("sky dome produced no points");

    // An orthonormal frame whose third axis is up, so the dome's base is level.
    double w[3] = {up[0], up[1], up[2]};
    double wn = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (!(wn > 1e-12)) { w[0] = 0; w[1] = 0; w[2] = 1; wn = 1.0; }
    for (int r = 0; r < 3; r++) w[r] /= wn;
    double a[3] = {1, 0, 0};
    if (std::fabs(w[0]) > 0.9) { a[0] = 0; a[1] = 1; a[2] = 0; }
    const double d = w[0] * a[0] + w[1] * a[1] + w[2] * a[2];
    for (int r = 0; r < 3; r++) a[r] -= d * w[r];
    double an = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!(an > 1e-12)) { a[0] = 1; a[1] = 0; a[2] = 0; an = 1.0; }
    for (int r = 0; r < 3; r++) a[r] /= an;
    const double b[3] = {w[1] * a[2] - w[2] * a[1],
                         w[2] * a[0] - w[0] * a[2],
                         w[0] * a[1] - w[1] * a[0]};

    ColmapPoints3D out_cloud;
    out_cloud.xyz.reserve(unit.size() * 3);
    out_cloud.rgb.reserve(unit.size() * 3);
    for (const auto& u : unit) {
        for (int r = 0; r < 3; r++)
            out_cloud.xyz.push_back(out.center[(size_t)r] +
                                    out.radius * (a[r] * u[0] + b[r] * u[1] + w[r] * u[2]));
        for (int r = 0; r < 3; r++) out_cloud.rgb.push_back(cfg.rgb[r]);
    }
    out.points = (int64_t)unit.size();
    if (fit) *fit = out;
    return out_cloud;
}