#pragma once

// Which of a 360 capture's views a person can be in, remembered across frames.
//
// The rays themselves are a CACHE: the masks on disk are the deliverable, so
// losing them costs speed on the next run, never correctness. What must survive
// a restart is the decision -- for each frame, which views were genuinely searched
// and which were filled from rays -- because without it a resumed run cannot tell
// a gated frame from a scanned one and restarts its safety-scan cadence cold, in
// the middle of a camera.
//
// One bit per view per frame: a 552-frame capture of ten views is under 6 KB,
// against 182 MB of masks it describes.

#include "app/PersonRays.h"

#include <cstdint>
#include <vector>

namespace app {

// Bytes per entry, and the format's magic. Bit i of a frame's word is view i.
inline constexpr int kScanIndexEntryBytes = 2;   // ten views, room to sixteen
inline constexpr char kScanIndexMagic[8] = {'S', 'S', 'R', 'A', 'Y', '0', '0', '1'};
inline constexpr char kScanIndexFile[] = ".spirula-rays";

// Which views each frame was really searched in, by frame number. Nothing here is
// needed to read a mask back -- the PNGs stand alone -- so a missing or truncated
// file costs a cold start and nothing more.
class ScanIndex {
public:
    // Empty until load() or resize(); an empty index reports every frame as
    // unscanned, which is what makes a fresh run scan everything.
    void resize(size_t frames);

    // Whether view `view` of `frame` was searched by the model rather than
    // filled from rays. Out of range reads false: safer to re-scan than to skip.
    bool scanned(size_t frame, int view) const;
    void mark_scanned(size_t frame, int view);
    // The frame after the last full scan, or -1 when none is recorded, which is
    // what the safety scan uses to resume its cadence.
    int64_t last_full_scan() const { return last_full_; }
    void set_last_full_scan(int64_t frame) { last_full_ = frame; }

    // True when `frame` is far enough past the last full scan to need one. On a
    // fresh or resumed index the answer is yes, so nothing is skipped unsearched.
    bool due_for_full_scan(int64_t frame, int every) const;

    // Views searched in `frame`, ascending.
    std::vector<int> scanned_views(size_t frame) const;
    // How many of `frames`' entries carry no bit at all -- the work a cold start
    // would have to redo, which is what makes a resume measurable.
    size_t uncached_frames(size_t frames) const;

    bool empty() const { return words_.empty(); }
    size_t frames() const { return words_.size(); }

    // A fixed header and the words. Fails on a wrong magic, a short read or a
    // size that does not match, and leaves *this untouched in every case: a
    // corrupt cache must never be able to make a run skip a search.
    bool save(const std::string& path, size_t frames, std::string& error) const;
    bool load(const std::string& path, std::string& error);

private:
    std::vector<uint16_t> words_;
    int64_t last_full_ = -1;
    uint32_t magic_ = 0;
};

}  // namespace app