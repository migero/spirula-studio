#include "app/PersonStore.h"

#include <algorithm>
#include <cmath>

namespace app {

namespace {

// Direction at `p`, as the three doubles the velocity maths wants.
void centre_of(const PersonRays& p, double out[3]) {
    float f[3];
    p.centre(f);
    out[0] = f[0];
    out[1] = f[1];
    out[2] = f[2];
}

double angle_between(const double a[3], const double b[3]) {
    const double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::acos(std::min(1.0, std::max(-1.0, d)));
}

}  // namespace

void PersonTrack::note_position(double dt_seconds) {
    double c[3];
    centre_of(rays, c);
    prev_x = last_x;
    prev_y = last_y;
    prev_z = last_z;
    last_x = c[0];
    last_y = c[1];
    last_z = c[2];
    dt = dt_seconds;
}

void PersonTrack::predict(double dt_seconds, double max_speed, double out[3]) const {
    if (dt_seconds <= 0.0 || dt <= 0.0) {
        out[0] = last_x;
        out[1] = last_y;
        out[2] = last_z;
        return;
    }
    double vx = last_x - prev_x, vy = last_y - prev_y, vz = last_z - prev_z;
    const double step = dt_seconds / dt;
    vx *= step;
    vy *= step;
    vz *= step;
    const double speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (max_speed > 0.0 && speed > max_speed) {
        const double k = max_speed / speed;
        vx *= k;
        vy *= k;
        vz *= k;
    }
    out[0] = last_x + vx;
    out[1] = last_y + vy;
    out[2] = last_z + vz;
    const double n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (n > 1e-9) {
        out[0] /= n;
        out[1] /= n;
        out[2] /= n;
    }
}

void PersonStore::configure(double merge_cos, double max_speed_rad_s) {
    merge_cos_ = merge_cos;
    max_speed_ = max_speed_rad_s;
}

int PersonStore::add(const PersonRays& p, double dt_seconds) {
    if (p.empty()) return -1;
    // Merge into the nearest existing track whose cone this one overlaps: two
    // views reporting one person are one person, and merging is what keeps a
    // person at a face boundary from being counted -- and searched -- twice.
    int best = -1;
    double best_gap = -1e9;
    double c[3];
    centre_of(p, c);
    for (size_t i = 0; i < tracks_.size(); i++) {
        const PersonRays& e = tracks_[i].rays;
        if (e.empty()) continue;
        double ec[3];
        centre_of(e, ec);
        const double gap = angle_between(c, ec) - p.angularRadius() - e.angularRadius();
        const double cos_gap = std::cos(std::max(0.0, gap));
        if (cos_gap >= merge_cos_ && cos_gap > best_gap) {
            best_gap = cos_gap;
            best = (int)i;
        }
    }
    if (best < 0) {
        PersonTrack t;
        t.rays = p;
        t.max_speed = max_speed_;
        t.note_position(dt_seconds);
        tracks_.push_back(std::move(t));
        return (int)tracks_.size() - 1;
    }
    person_rays_merge(tracks_[(size_t)best].rays, p, merge_cos_);
    tracks_[(size_t)best].note_position(dt_seconds);
    return best;
}

void PersonStore::expire(double now_seconds, double max_age) {
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [&](const PersonTrack& t) {
                                     const double age = now_seconds - t.dt;
                                     return age > max_age;
                                 }),
                  tracks_.end());
}

std::vector<int> PersonStore::views_to_search(const std::vector<Pano360View>& views,
                                             double dt_seconds,
                                             double slop_rad) const {
    std::vector<char> hit(views.size(), 0);
    for (const PersonTrack& t : tracks_) {
        if (t.rays.empty()) continue;
        // Predict forward, then widen by the movement since the last frame: a
        // person walking across the sky moves cone as well as centre.
        double c[3];
        t.predict(dt_seconds, t.max_speed, c);
        for (size_t v = 0; v < views.size(); v++) {
            if (hit[v]) continue;
            if (person_may_be_in(t.rays, views[v], slop_rad +
                                                   t.rays.angularRadius() * 0.5))
                hit[v] = 1;
        }
    }
    std::vector<int> out;
    for (size_t v = 0; v < views.size(); v++)
        if (hit[v]) out.push_back((int)v);
    return out;
}

size_t PersonStore::ray_budget() const {
    size_t n = 0;
    for (const PersonTrack& t : tracks_) n += t.rays.size();
    return n;
}

std::vector<int> gate_search_views(const PersonStore& store,
                                   const std::vector<Pano360View>& views,
                                   int64_t frame, size_t view_count, int64_t force_full,
                                   ScanIndex& index, int every) {
    std::vector<int> out;
    if (view_count == 0 || views.empty()) return out;
    const bool full = force_full != 0 || index.due_for_full_scan(frame, every);
    if (full) {
        for (size_t v = 0; v < view_count && v < views.size(); v++) {
            out.push_back((int)v);
            index.mark_scanned((size_t)frame, (int)v);
        }
        index.set_last_full_scan(frame);
        return out;
    }
    // An unsearched frame is NOT a reason to scan everything -- gating is for
    // exactly those frames. Only an empty STORE is: no tracks, no cone to gate.
    if (store.size() == 0) {
        for (size_t v = 0; v < view_count && v < views.size(); v++) {
            out.push_back((int)v);
            index.mark_scanned((size_t)frame, (int)v);
        }
        index.set_last_full_scan(frame);
        return out;
    }
    out = store.views_to_search(views, 1.0, 0.15);
    for (int v : out)
        if (v >= 0 && (size_t)v < view_count) index.mark_scanned((size_t)frame, v);
    return out;
}

void project_tracks(const PersonStore& store, const Pano360View& v, int w, int h,
                    std::vector<uint8_t>& out, int stride, int dilate_px) {
    out.assign((size_t)w * h, 255);
    if (w <= 0 || h <= 0) return;
    for (const PersonTrack& t : store.tracks()) {
        if (t.rays.empty()) continue;
        std::vector<uint8_t> one;
        // 255 marks the person's pixels here, and 0 marks them in the dataset's
        // mask convention (255 = KEEP), so the polarity flips on the way in.
        person_rays_to_mask(t.rays, v, w, h, one, stride, dilate_px);
        for (size_t i = 0; i < out.size(); i++)
            if (one[i]) out[i] = 0;
    }
}

PersonRays rays_from_detections(const Pano360View& v, const std::vector<uint8_t>& mask,
                                 int w, int h, int stride, bool keep) {
    PersonRays p;
    p.dirs = person_rays_from_mask(v, mask.data(), w, h, stride, keep);
    return p;
}

}  // namespace app