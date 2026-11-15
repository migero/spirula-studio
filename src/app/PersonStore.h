// Deciding which of a 360 capture's views are worth searching for a person, and
// what to do with what the search finds.
//
// A full scan of ten views costs ten model calls a frame, and on a capture whose
// operator fills one of them the other nine are almost always empty -- measured on
// a 552-frame handheld walk, eight of ten views had no mask at all. So the views
// are gated: a tracked person is searched only in the views his cone can reach,
// and every so often every view is searched anyway, which is what catches someone
// who walks in between scans.
//
// That last part must not be optimised away: gating is a speed measure, the full
// scan is the correctness one, and the store carries forward whatever it found.

#include "app/PersonRays.h"
#include "app/ScanIndex.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace app {

// A person's cone, and how it moves. Positions are the sphere's, so a track from
// one view is comparable with one from another -- which is what lets a gate be a
// geometric question rather than a bookkeeping one.
struct PersonTrack {
    PersonRays rays;
    // Centre on the last two frames, so the next frame's guess leads a walking
    // person instead of trailing them. Seconds, not frames: extraction rate is
    // the user's choice and the two must not be conflated.
    double last_x = 0, last_y = 0, last_z = 0;
    double prev_x = 0, prev_y = 0, prev_z = 0;
    double dt = 0.0;

    void note_position(double dt_seconds);
    // Where he should be `dt` from now: last plus velocity, capped in
    // `max_speed` radians a second so a bad frame cannot throw the track across
    // the sphere. `max_speed` <= 0 leaves it uncapped.
    void predict(double dt_seconds, double max_speed, double out[3]) const;
    double max_speed = 1.5;
};

// A track list across frames of one capture. The rays are the cache; this is the
// thing that reads like state while a run is going.
class PersonStore {
public:
    // `merge_cos` is the cosine of how close two people's cones may be and still
    // count as one person. 0.98 is about 11 degrees, roughly a person-sized
    // gap; it is a parameter because it depends on how close people stand.
    void configure(double merge_cos, double max_speed_rad_s = 1.5);

    // Adds `p`, merging it into an existing track when they are the same person.
    // Returns the index of the track it landed in, or -1 for an empty person.
    int add(const PersonRays& p, double dt_seconds);

    // Tracks not seen for `max_age` seconds are dropped: a person who left
    // should stop costing searches.
    void expire(double now_seconds, double max_age);

    size_t size() const { return tracks_.size(); }
    const PersonTrack& at(size_t i) const { return tracks_[i]; }
    const std::vector<PersonTrack>& tracks() const { return tracks_; }
    std::vector<PersonTrack>& tracks() { return tracks_; }
    void clear() { tracks_.clear(); }

    // Every view at least one track's predicted cone can reach. The search set.
    std::vector<int> views_to_search(const std::vector<Pano360View>& views,
                                     double dt_seconds, double slop_rad) const;

    // Peak rays across all tracks, which is what bounds a capture's cache.
    size_t ray_budget() const;

private:
    std::vector<PersonTrack> tracks_;
    double merge_cos_ = 0.98;
    double max_speed_ = 1.5;
};

// The search set for one frame: the gated views, or every view on a frame that is
// due a full scan. `views` is the whole plan. A forced full scan is what makes a
// person who appears mid-capture get found, so it is not optional.
std::vector<int> gate_search_views(const PersonStore& store,
                                   const std::vector<Pano360View>& views,
                                   int64_t frame, size_t view_count, int64_t force_full,
                                   ScanIndex& index, int every);

// Fills `out` with the union of every track's mask in `v`. Rays outside `v` are
// skipped, so a person lands whole in each view that can see him and not at all
// in the rest -- the split, without a second detection to reconcile.
void project_tracks(const PersonStore& store, const Pano360View& v, int w, int h,
                    std::vector<uint8_t>& out, int stride, int dilate_px);

// Rays for one detection on one view, at the view's native size.
PersonRays rays_from_detections(const Pano360View& v, const std::vector<uint8_t>& mask,
                                 int w, int h, int stride, bool keep);

}  // namespace app