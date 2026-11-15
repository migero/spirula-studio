#include "app/GoProBin.h"
#include "app/GoProMask.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool ok, const std::string& text) {
    if (!ok) throw std::runtime_error(text);
}

namespace app {

// The test's own reference triples, kept apart from the modules' GoProVec3.
struct Vec3 {
    double x, y, z;
};

Vec3 unit3(Vec3 a) {
    const double n = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return Vec3{a.x / n, a.y / n, a.z / n};
}
double dot3(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross3(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 add3(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale3(const Vec3& a, double s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
GoProVec3 g(const Vec3& v) { return GoProVec3{v.x, v.y, v.z}; }
Vec3 from(const GoProVec3& v) { return Vec3{v.x, v.y, v.z}; }
// pano360_ray_pixel takes a float triple; the ray types here are doubles.
std::array<float, 3> ray3(const GoProVec3& v) {
    return {float(v.x), float(v.y), float(v.z)};
}
std::array<float, 3> ray3(const Vec3& v) {
    return {float(v.x), float(v.y), float(v.z)};
}

}  // namespace app

namespace {

void write_u32(std::vector<uint8_t>& b, uint32_t v) {
    for (int k = 0; k < 4; k++) b.push_back(uint8_t(v >> (8 * k)));
}

void write_f64(std::vector<uint8_t>& b, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    for (int k = 0; k < 8; k++) b.push_back(uint8_t(bits >> (8 * k)));
}

void write_vec(std::vector<uint8_t>& b, const app::Vec3& v) {
    write_f64(b, v.x);
    write_f64(b, v.y);
    write_f64(b, v.z);
}

void write_person(std::vector<uint8_t>& b, const app::GoProBinPerson& p) {
    write_u32(b, (uint32_t)p.id);
    write_u32(b, p.outline ? 1 : 0);
    write_u32(b, (uint32_t)p.points.size());
    write_vec(b, app::from(p.center));
    for (const app::GoProVec3& q : p.points) write_vec(b, app::Vec3{q.x, q.y, q.z});
    write_u32(b, (uint32_t)p.oval.size());
    for (const app::GoProVec3& q : p.oval) write_vec(b, app::Vec3{q.x, q.y, q.z});
}

// A small closed loop of unit rays around `dir`, used as a stored oval.
app::GoProBinPerson ring_oval(const app::GoProBinPerson& person, const app::Vec3& dir,
                              double radius, int n) {
    app::GoProBinPerson with = person;
    const app::Vec3 basis1 = app::unit3(std::fabs(dir.z) < .9 ? app::Vec3{0, 0, 1} : app::Vec3{1, 0, 0});
    const app::Vec3 basis2 = app::unit3(app::cross3(dir, basis1));
    for (int k = 0; k < n; k++) {
        const double t = 2. * 3.141592653589793 * k / n;
        with.oval.push_back(app::g(app::unit3(
            app::add3(app::scale3(basis1, radius * std::cos(t)),
                      app::add3(app::scale3(basis2, radius * std::sin(t)), app::scale3(dir, 1.))))));
    }
    return with;
}

app::GoProBinPerson ring_person(int id, const app::Vec3& dir, double radius, int n,
                                double tilt = 0.) {
    app::GoProBinPerson person;
    person.id = id;
    person.outline = true;
    person.center = app::g(app::unit3(dir));
    const app::Vec3 basis1 = app::unit3(std::fabs(dir.z) < .9 ? app::Vec3{0, 0, 1} : app::Vec3{1, 0, 0});
    const app::Vec3 basis2 = app::unit3(app::cross3(dir, basis1));
    for (int k = 0; k < n; k++) {
        const double t = 2. * 3.141592653589793 * k / n;
        const double r = radius * (1. + 0.45 * std::cos(t + tilt));  // not a ring: a contour
        person.points.push_back(app::g(app::unit3(
            app::add3(app::scale3(basis1, r * std::cos(t)),
                      app::add3(app::scale3(basis2, r * std::sin(t)), app::scale3(dir, 1.))))));
    }
    return person;
}

size_t zeros(const std::vector<uint8_t>& mask) {
    return (size_t)std::count(mask.begin(), mask.end(), 0);
}

// Pixels a fisheye cannot see: outside the lens circle, where no outline ray
// lands. A fill that reaches them has closed through the square's corner.
size_t outside_circle(const std::vector<uint8_t>& mask, int w, int h) {
    size_t n = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const double dx = (i + 0.5 - w * 0.5) / (w * 0.5);
            const double dy = (j + 0.5 - h * 0.5) / (h * 0.5);
            if (std::sqrt(dx * dx + dy * dy) > 1.0 + 2.0 / w &&
                mask[(size_t)j * w + i] == 0)
                ++n;
        }
    return n;
}

// A projected pixel inside the mask; rounding pushed a seam pixel onto a
// neighbouring column, so clamp it back into the image.
size_t mask_index(float px, float py, int w, int h) {
    const long lx = std::clamp(std::lround(px), 0L, (long)w - 1);
    const long ly = std::clamp(std::lround(py), 0L, (long)h - 1);
    return (size_t)ly * w + (size_t)lx;
}

void bin_tests(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    for (const char c : std::string("GPDSTR06")) bytes.push_back((uint8_t)c);
    write_u32(bytes, 3);  // legacy header flags, still 28 bytes before record 0
    write_vec(bytes, app::unit3(app::Vec3{0.2, 0.4, 1.0}));

    const auto person = ring_person(7, app::Vec3{0, 0, 1}, 0.06, 5);
    const auto person2 = ring_person(8, app::Vec3{0.5, 0.2, 0.84}, 0.08, 4);
    const auto person_oval = ring_oval(person, app::Vec3{0, -0.2, 0.98}, 0.05, 8);
    const auto person2_oval = ring_oval(person2, app::Vec3{0.45, -0.1, 0.89}, 0.06, 8);
    // Frame 0: one person with a stored oval, two views.
    write_f64(bytes, 0.0);
    write_u32(bytes, 1);
    write_person(bytes, person_oval);
    write_u32(bytes, 2);
    for (int j = 0; j < 8; j++) write_vec(bytes, app::Vec3{0, 1, 0});
    write_u32(bytes, 1);  // legacy record state
    write_vec(bytes, app::Vec3{0, 1, 0});
    // Frame 1: another person with an oval, no views.
    write_f64(bytes, 0.25);
    write_u32(bytes, 1);
    write_person(bytes, person2_oval);
    write_u32(bytes, 0);
    write_u32(bytes, 1);
    write_vec(bytes, app::Vec3{0, 1, 0});
    // Frame 2: no people (a bare 40-byte record on its own).
    write_f64(bytes, 0.5);
    write_u32(bytes, 0);
    write_u32(bytes, 0);
    write_u32(bytes, 0);
    write_vec(bytes, app::Vec3{0, 1, 0});
    // Frame 3: no people, no views.
    write_f64(bytes, 0.75);
    write_u32(bytes, 0);
    write_u32(bytes, 0);
    write_u32(bytes, 0);
    write_vec(bytes, app::Vec3{0, 1, 0});

    {
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    }

    std::string error;
    app::GoProBin bin;
    require(bin.open(path, error), error);
    require(bin.frames() == 4, "GoPro bin frame count");
    require(bin.first_time() == 0. && bin.last_time() == 0.75, "GoPro bin times");
    app::GoProBinFrame f;
    require(bin.lookup(0.0, 24, f, error), error);
    require(f.time == 0. && f.people.size() == 1, "GoPro bin first frame");
    require(f.people[0].id == 7 && f.people[0].outline, "GoPro bin person fields");
    require(f.people[0].points.size() == 5, "GoPro bin outline points");
    require(f.people[0].oval.size() == 8, "GoPro bin stored oval rays");
    require(f.people[0].center.z > 0.999, "GoPro bin centre ray");
    require(!bin.lookup(10.0, 24, f, error), "Lookup outside the bin must fail");
    require(!bin.lookup(0.4, 24, f, error), "Lookup outside tolerance must fail");
    require(!bin.lookup(0.53, 24, f, error), "Lookup just past tolerance must fail");
    require(bin.lookup(0.01, 24, f, error) && f.time == 0.0, "Nearest-record lookup rule");
    require(bin.lookup(0.51, 24, f, error) && f.time == 0.5, "Nearest-record lookup rule");
    require(f.people.empty(), "Empty frame decodes to no people");
    require(bin.lookup(0.25, 24, f, error), error);
    require(f.people[0].oval.size() == 8, "Second person's stored oval");

    // An oval count past the player's 96-point cap makes the file corrupt.
    {
        std::vector<uint8_t> bad = bytes;
        // 36 header + 8 time + 4 count + 12 id/outline/n + 24 centre + 120 points
        std::memcpy(bad.data() + 204, "\xc8\0\0\0", 4);  // oval count 200 > 96
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)bad.data(), (std::streamsize)bad.size());
    }
    std::string reject_error;
    app::GoProBin reject;
    require(!reject.open(path, reject_error) && !reject_error.empty(),
            "Oval count past the cap must be rejected");

    std::filesystem::remove(path);
    std::cout << "GoPro bin reader checks passed\n";
}

void mask_tests() {
    app::Pano360Layout layout;
    require(app::pano360_detect(2, 4096, 1344, {}, layout), "MAX layout");
    app::Pano360Options opt;
    opt.mode = app::Pano360Mode::Faces;
    opt.size = 64;
    const auto views = app::pano360_views(layout, opt);
    require(views.size() == 10, "Faces view count");

    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const app::Vec3 sun = app::unit3(app::Vec3{1, 1, 0});
    const app::Vec3 forward{0, 0, 1};
    const auto person = ring_person(1, forward, 0.1, 7);

    int axial = -1;
    for (size_t v = 0; v < views.size(); v++) {
        const float axis[3] = {views[v].rot[2], views[v].rot[5], views[v].rot[8]};
        if (std::fabs(axis[0]) < 0.01f && std::fabs(axis[1]) < 0.01f && axis[2] > 0.99f) {
            axial = (int)v;
            break;
        }
    }
    require(axial >= 0, "No axial +z view in the plan");

    const int w = views[axial].width, h = views[axial].height;
    float px, py;
    require(app::pano360_ray_pixel(views[axial], app::ray3(person.center).data(),
                                   px, py),
            "Centre ray must project");
    std::vector<uint8_t> mask((size_t)w * h, 255);
    std::string error;
    std::vector<app::GoProVec3> rays;
    for (const app::GoProVec3& q : person.points) rays.push_back(q);
    require(app::gopro_mask_polygon(mask.data(), w, h, views[axial], rays, identity, 0, error),
            error);
    const size_t removed = zeros(mask);
    require(removed > 10 && removed < (size_t)w * h / 4, "Person fill covers a plausible area");
    const size_t ci = (size_t)std::lround(py) * w + std::lround(px);
    require(mask[ci] == 0, "Person centre is removed");
    require(mask[0] == 255, "The view's far corner stays");

    // Moved out of sight: nothing is removed.
    {
        const auto behind = ring_person(1, app::Vec3{0, 0, -1}, 0.08, 7);
        std::vector<uint8_t> m2((size_t)w * h, 255);
        std::vector<app::GoProVec3> behind_rays;
        for (const app::GoProVec3& q : behind.points) behind_rays.push_back(q);
        require(app::gopro_mask_polygon(m2.data(), w, h, views[axial], behind_rays, identity, 0,
                                        error),
                error);
        require(zeros(m2) == 0, "A person behind the view removes nothing");
    }

    // A person at the axial view's edge shows in both neighbours there.
    const app::Vec3 seam_dir = app::unit3(app::Vec3{1, 0, 1});
    const auto seam = ring_person(1, seam_dir, 0.12, 9);
    std::vector<app::GoProVec3> seam_rays(seam.points);
    int covering = 0;
    for (size_t v = 0; v < views.size(); v++) {
        float sx, sy;
        if (!app::pano360_ray_pixel(views[v], app::ray3(seam.center).data(), sx, sy))
            continue;
        std::vector<uint8_t> m3((size_t)views[v].width * views[v].height, 255);
        require(app::gopro_mask_polygon(m3.data(), views[v].width, views[v].height, views[v],
                                        seam_rays, identity, 0, error),
                error);
        if (zeros(m3) > 0) {
            covering++;
            const size_t si = mask_index(sx, sy, views[v].width, views[v].height);
            require(m3[si] == 0, "Seam person's centre pixel removed");
        }
    }
    require(covering >= 2, "The seam person fills more than one view");

    // Fisheye: the circle is the image edge, so an outline crossing it closes
    // along the lens rim, never through the square's black corners.
    {
        app::Pano360Options fs;
        fs.mode = app::Pano360Mode::Fisheye;
        fs.size = 64;
        const auto fish = app::pano360_views(layout, fs);
        require(fish.size() == 2 && fish[0].proj == app::Pano360Projection::Fisheye,
                "Fisheye view count");
        const int fw = fish[0].width, fh = fish[0].height;
        std::vector<uint8_t> m6((size_t)fw * fh, 255);
        require(app::gopro_mask_polygon(m6.data(), fw, fh, fish[0], rays, identity, 0, error),
                error);
        require(zeros(m6) > 0, "A fisheye view removes the person on its axis");
        require(outside_circle(m6, fw, fh) == 0, "and never a pixel outside its lens");

        // A person over the rim: the 90 degree seam the two lenses share.
        const auto onrim = ring_person(1, app::unit3(app::Vec3{1, 0.35, 0}), 0.12, 12);
        std::vector<app::GoProVec3> rim_rays(onrim.points);
        int in_view = 0;
        for (size_t v = 0; v < fish.size(); v++) {
            std::vector<uint8_t> m7((size_t)fw * fh, 255);
            require(app::gopro_mask_polygon(m7.data(), fw, fh, fish[v], rim_rays, identity,
                                            0, error),
                    error);
            if (zeros(m7) > 0) ++in_view;
            require(outside_circle(m7, fw, fh) == 0,
                    "A rim person's fill stays inside the lens");
        }
        require(in_view == 2, "A person on the lens seam is in both fisheyes");

        // A ring straddling the rim on the diagonal, where the corner of the
        // square sits square-on to it: the closure that reaches the corner is
        // the one that would paint black pixels as the person.
        std::vector<app::GoProVec3> diag;
        for (int k = 0; k < 96; k++) {
            const double t = 2. * 3.141592653589793 * k / 96;
            float d[3];
            app::pano360_view_ray(fish[0], (float)(fw * .5 + .30 * fw + .20 * fw * std::cos(t)),
                                  (float)(fh * .5 + .30 * fh + .20 * fh * std::sin(t)), d);
            diag.push_back({d[0], d[1], d[2]});
        }
        for (size_t v = 0; v < fish.size(); v++) {
            std::vector<uint8_t> m8((size_t)fw * fh, 255);
            require(app::gopro_mask_polygon(m8.data(), fw, fh, fish[v], diag, identity, 0,
                                            error),
                    error);
            require(zeros(m8) > 0, "A diagonal rim person is in the view he crosses");
            require(outside_circle(m8, fw, fh) == 0,
                    "and his fill closes on the rim, not through the corner");
        }
    }

    // A rotation applied to the World-Lock rays: the mask follows the mapped
    // ray, pinning the mapping's sense.
    const double c = std::cos(3.141592653589793 / 6.), s = std::sin(3.141592653589793 / 6.);
    const double rot[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    std::vector<uint8_t> m4((size_t)w * h, 255);
    require(app::gopro_mask_polygon(m4.data(), w, h, views[axial], rays, rot, 0, error), error);
    const app::Vec3 mapped = app::Vec3{s, 0, c};
    float rx, ry;
    require(app::pano360_ray_pixel(views[axial], app::ray3(mapped).data(), rx, ry),
            "Mapped ray must project");
    require(m4[(size_t)std::lround(ry) * w + std::lround(rx)] == 0,
            "Rotated mask lands at the mapped ray");
    const size_t unmoved = (size_t)std::lround(py) * w + std::lround(px);
    if (unmoved != (size_t)std::lround(ry) * w + std::lround(rx))
        require(m4[unmoved] == 255, "The un-rotated spot stays");

    // An equirect view at the antimeridian fills both edges of the image.
    app::Pano360Options eq;
    eq.mode = app::Pano360Mode::Equirect;
    eq.size = 96;
    const auto equirect = app::pano360_views(layout, eq);
    require(equirect.size() == 1, "Equirect view count");
    const int ew = equirect[0].width, eh = equirect[0].height;
    const auto wrap = ring_person(1, app::unit3(app::Vec3{-0.02, 0, -1}), 0.15, 9);
    std::vector<app::GoProVec3> wrap_rays(wrap.points);
    std::vector<uint8_t> m5((size_t)ew * eh, 255);
    require(app::gopro_mask_polygon(m5.data(), ew, eh, equirect[0], wrap_rays, identity, 0, error),
            error);
    require(zeros(m5) > 0, "Wrapped person does not vanish");
    bool left = false, right = false;
    for (int y = 0; y < eh; y++) {
        left |= m5[(size_t)y * ew] == 0;
        right |= m5[(size_t)y * ew + ew - 1] == 0;
    }
    require(left && right, "Wrapped person fills both image edges");

    // One call over a whole frame: the outline, then the stored shadow oval.
    // The oval sits away from the person, spanning its own scanlines.
    const auto ground = ring_person(1, app::unit3(app::Vec3{0, -0.3, 0.95}), 0.1, 7);
    const auto grounded = ring_oval(ground, app::unit3(app::Vec3{0, -0.45, 0.89}), 0.15, 8);
    app::GoProBinFrame frame;
    frame.time = 0;
    frame.people.push_back(grounded);
    std::vector<uint8_t> plain((size_t)w * h, 255);
    require(app::gopro_mask_frame(views[axial], w, h, frame, plain.data(), error),
            error);
    require(zeros(plain) > 10, "Frame mask removes the person");

    app::GoProBinFrame outline_only;
    outline_only.time = 0;
    outline_only.people.push_back(ground);
    std::vector<uint8_t> no_oval((size_t)w * h, 255);
    require(app::gopro_mask_frame(views[axial], w, h, outline_only, no_oval.data(), error),
            error);
    require(zeros(no_oval) < zeros(plain), "The stored oval adds removal");

    // A ring that leaves the view through the bottom border only: the fill
    // closes through the border and reaches the last row.
    {
        std::vector<app::GoProVec3> ring;
        for (int k = 0; k < 72; k++) {
            const double t = 2. * 3.141592653589793 * k / 72;
            float d[3];
            app::pano360_view_ray(views[axial], (float)(w * .5 + .3 * h * std::cos(t)),
                                  (float)(h * .75 + .3 * h * std::sin(t)), d);
            ring.push_back({d[0], d[1], d[2]});
        }
        std::vector<uint8_t> m((size_t)w * h, 255);
        require(app::gopro_mask_polygon(m.data(), w, h, views[axial], ring, identity, 0,
                                        error),
                error);
        bool bottom = false;
        for (int x = 0; x < w; x++) bottom = bottom || m[(size_t)(h - 1) * w + x] == 0;
        require(bottom, "A border-crossing fill reaches the last row");
        require(zeros(m) < (size_t)w * h / 2, "The bottom-crossing fill stays inside the ring");
    }

    // A ring wrapping every corner: each border hop takes the short arc, so
    // the fill is the ring's interior and the corners stay.
    {
        std::vector<app::GoProVec3> ring;
        for (int k = 0; k < 96; k++) {
            const double t = 2. * 3.141592653589793 * k / 96;
            float d[3];
            app::pano360_view_ray(views[axial], (float)(w * .5 + .625 * h * std::cos(t)),
                                  (float)(h * .5 + .625 * h * std::sin(t)), d);
            ring.push_back({d[0], d[1], d[2]});
        }
        std::vector<uint8_t> m((size_t)w * h, 255);
        require(app::gopro_mask_polygon(m.data(), w, h, views[axial], ring, identity, 0,
                                        error),
                error);
        require(zeros(m) > (size_t)w * h / 2, "A corner-wrapping ring fills its interior");
        require(m[0] == 255 && m[(size_t)h * w - 1] == 255,
                "The corners outside a corner-wrapping ring stay");
    }

    // The raster checks above leave the last mask; print nothing more.

    std::cout << "GoPro mask raster checks passed\n";
}

}  // namespace

int main() {
    try {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "spirula-gopro-bin-test.bin";
        bin_tests(path);
        mask_tests();
        std::cout << "gopro_bin_test passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
