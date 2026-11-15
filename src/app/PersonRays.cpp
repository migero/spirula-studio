#include "app/PersonRays.h"

#include <cmath>

namespace app {

namespace {

// A ray is inside v when it projects to a pixel inside it; pano360_ray_pixel
// already answers exactly that, false for behind or outside.
bool ray_in_view(const Pano360View& v, const float dir[3]) {
    float x, y;
    return pano360_ray_pixel(v, dir, x, y);
}

}  // namespace

void PersonRays::centre(float out[3]) const {
    double x = 0, y = 0, z = 0;
    for (size_t i = 0; i + 2 < dirs.size(); i += 3) {
        x += dirs[i];
        y += dirs[i + 1];
        z += dirs[i + 2];
    }
    const double n = std::sqrt(x * x + y * y + z * z);
    if (n <= 0.0) {
        out[0] = 0;
        out[1] = 0;
        out[2] = 1;
        return;
    }
    out[0] = (float)(x / n);
    out[1] = (float)(y / n);
    out[2] = (float)(z / n);
}

double PersonRays::angularRadius() const {
    if (dirs.size() < 6) return 0.0;
    float c[3];
    centre(c);
    double worst = 0.0;
    for (size_t i = 0; i + 2 < dirs.size(); i += 3) {
        const double d = (double)dirs[i] * c[0] + (double)dirs[i + 1] * c[1] +
                         (double)dirs[i + 2] * c[2];
        worst = std::max(worst, std::acos(std::min(1.0, std::max(-1.0, d))));
    }
    return worst;
}

std::vector<float> person_rays_from_mask(const Pano360View& v, const uint8_t* mask,
                                         int mask_w, int mask_h, int stride,
                                         bool keep) {
    std::vector<float> out;
    if (stride < 1) stride = 1;
    const uint8_t want = keep ? 255 : 0;
    for (int j = 0; j < mask_h; j += stride) {
        for (int i = 0; i < mask_w; i += stride) {
            if (mask[(size_t)j * mask_w + i] != want) continue;
            // The mask is the image's own size, so the pixel IS the view's
            // pixel and no rescale is wanted here.
            float d[3];
            pano360_view_ray(v, (float)i, (float)j, d);
            out.push_back(d[0]);
            out.push_back(d[1]);
            out.push_back(d[2]);
        }
    }
    return out;
}

bool person_rays_merge(PersonRays& a, const PersonRays& b, double cos_thresh) {
    if (b.empty()) return false;
    if (a.empty()) {
        a = b;
        return true;
    }
    // Angular overlap between the two cones, not ray identity: the same person
    // seen from two views has different rays per view and overlapping cones.
    float ca[3], cb[3];
    a.centre(ca);
    b.centre(cb);
    const double cd = (double)ca[0] * cb[0] + (double)ca[1] * cb[1] +
                      (double)ca[2] * cb[2];
    const double da = a.angularRadius(), db = b.angularRadius();
    const double gap = std::acos(std::min(1.0, std::max(-1.0, cd))) - da - db;
    if (std::cos(std::max(0.0, gap)) < cos_thresh) return false;
    a.dirs.insert(a.dirs.end(), b.dirs.begin(), b.dirs.end());
    a.last_frame = std::max(a.last_frame, b.last_frame);
    return true;
}

void person_rays_to_mask(const PersonRays& p, const Pano360View& v, int w, int h,
                         std::vector<uint8_t>& mask, int stride, int dilate_px) {
    mask.assign((size_t)w * h, 0);
    if (w <= 0 || h <= 0) return;
    // A `stride`th-pixel sample leaves dots, so each ray claims the stride-1 px
    // gap around it (at stride 4: bridges 101..103, still reaches 139) and each
    // row is filled between its first and last lit pixel. Per person.
    const int reach = std::max(dilate_px, stride - 1);
    for (size_t i = 0; i + 2 < p.dirs.size(); i += 3) {
        float x, y;
        if (!pano360_ray_pixel(v, &p.dirs[i], x, y)) continue;
        const int ix = (int)std::lround(x), iy = (int)std::lround(y);
        for (int dy = -reach; dy <= reach; dy++) {
            const int py = iy + dy;
            if (py < 0 || py >= h) continue;
            uint8_t* row = mask.data() + (size_t)py * w;
            for (int dx = -reach; dx <= reach; dx++) {
                const int px = ix + dx;
                if (px < 0 || px >= w) continue;
                row[px] = 255;
            }
        }
    }
    for (int y = 0; y < h; y++) {
        uint8_t* row = mask.data() + (size_t)y * w;
        int first = -1, last = -1;
        for (int x = 0; x < w; x++)
            if (row[x]) {
                if (first < 0) first = x;
                last = x;
            }
        if (first < 0 || last <= first) continue;
        for (int x = first; x <= last; x++) row[x] = 255;
    }
}

std::vector<int> person_visible_views(const PersonRays& p,
                                      const std::vector<Pano360View>& views) {
    std::vector<int> out;
    for (size_t k = 0; k < views.size(); k++) {
        for (size_t i = 0; i + 2 < p.dirs.size(); i += 3) {
            if (ray_in_view(views[k], &p.dirs[i])) {
                out.push_back((int)k);
                break;
            }
        }
    }
    return out;
}

bool person_may_be_in(const PersonRays& p, const Pano360View& v, double slop_rad) {
    if (p.empty()) return false;
    float c[3];
    p.centre(c);
    // The view's own axis is the third column of rot, which is where it looks.
    const float axis[3] = {v.rot[2], v.rot[5], v.rot[8]};
    const double d = (double)c[0] * axis[0] + (double)c[1] * axis[1] +
                     (double)c[2] * axis[2];
    const double ang = std::acos(std::min(1.0, std::max(-1.0, d)));
    return ang <= p.angularRadius() + slop_rad;
}

}  // namespace app