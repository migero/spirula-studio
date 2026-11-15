#pragma once

// A person, as rays. One SAM mask on one of a 360 capture's ten views says
// where something is in THAT view's pixels; the same person is different pixels
// in every other view. A ray is neither -- a direction, the same in all ten, so
// one set of them answers "which views can see this" at once.
//
// MERGE and SPLIT fall out of that, and a per-face mask can do neither: a
// person straddling a boundary is ONE ray set found in both views, never
// tracked twice, and projecting it back gives each the pixels he really
// occupies with no seam-shaped hole. Sky needs no tracker -- a cone a person
// cannot be in is not searched. docs/datasets.md, "Ray space".

#include "app/Pano360.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace app {

// A person's directions, and the frame they were last seen in. Rays are unit and
// in the sphere frame Pano360View::rot maps into -- the same frame for all ten
// views, which is the whole point.
struct PersonRays {
    std::vector<float> dirs;   // 3 floats each
    int64_t last_frame = -1;

    size_t size() const { return dirs.size() / 3; }
    bool empty() const { return dirs.empty(); }
    void clear() { dirs.clear(); last_frame = -1; }

    // Where it is: the mean unit vector, which is the direction a tracker's
    // next guess should start from.
    void centre(float out[3]) const;
    // Half the angle between the furthest ray and the centre, radians. The
    // apparent size of a mask grows with its FOV, so a size test in pixels is
    // not comparable between views; this one is.
    double angularRadius() const;
};

// The rays a mask covers in `v`, at every `stride`th pixel. `keep` says which
// value is the person; the convention is 255 = KEEP, so masked out is 0.
std::vector<float> person_rays_from_mask(const Pano360View& v,
                                         const uint8_t* mask, int mask_w,
                                         int mask_h, int stride, bool keep = false);

// Appends `other`'s rays, and reports whether it is the same person as this one
// -- the merge. `cos_thresh` is the cosine of the angle their centres may
// differ by; 1 merges nothing.
bool person_rays_merge(PersonRays& a, const PersonRays& b, double cos_thresh);

// Rasterises `p`'s rays into a mask `w` x `h` at `v`, 255 where a ray lands.
// A person is whole in every view that can see it and absent from the rest.
// `stride` is the sampling person_rays_from_mask used, so ray gaps close.
void person_rays_to_mask(const PersonRays& p, const Pano360View& v, int w, int h,
                         std::vector<uint8_t>& mask, int stride = 1,
                         int dilate_px = 0);

// Which of `views` a person can possibly be in: any ray landing inside. The
// search set for the next frame, and empty when a track has drifted out of
// every view.
std::vector<int> person_visible_views(const PersonRays& p,
                                      const std::vector<Pano360View>& views);

// Whether a ray can be inside `v`, cheaper than projecting: the centre angle
// alone, no per-ray pixel. Used to size the search before committing to it.
bool person_may_be_in(const PersonRays& p, const Pano360View& v, double slop_rad);

}  // namespace app