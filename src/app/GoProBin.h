#pragma once

// The GoPro Player's saved tracking file (.people.bin, magic GPDSTR02..06):
// per frame, people outlined as unit rays plus, in version06, a stored shadow
// oval per person. The camera-locked companion (.people.camera.bin) holds the
// same records with every ray in the video's own frame. Little-endian,
// concatenated records, indexed by one forward scan; the file stays in memory
// and a lookup decodes only the record it needs.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sfm {
struct SfmConfig;
}

namespace app {

struct GoProVec3 {
    double x = 0, y = 0, z = 0;
};

struct GoProBinPerson {
    int id = 0;
    bool outline = false;  // 1 = contour, 0 = box corners
    GoProVec3 center;      // unit ray
    std::vector<GoProVec3> points;  // closed loop of unit rays
    // Stored shadow oval, version06 only; empty when the frame has none.
    std::vector<GoProVec3> oval;
};

struct GoProBinFrame {
    double time = 0;
    std::vector<GoProBinPerson> people;
};

// The per-frame sun the player stores from version 4 on. In the .camera.bin
// companion it is the video's camera-frame direction -- one per frame -- which
// is what core/SunPosition.h's ephemeris can turn into a heading.
struct GoProSun {
    double time = 0;
    uint32_t state = 0;   // 0 none, 1 carried from the last keyframe, 3 a keyframe
    GoProVec3 dir;        // unit ray
    bool valid() const { return state != 0; }
};

class GoProBin {
public:
    bool open(const std::filesystem::path& bin, std::string& error);
    // Nearest record within 0.51/s of `time`s, the tolerance the player uses.
    bool lookup(double time, double src_fps, GoProBinFrame& out, std::string& error) const;
    static std::filesystem::path find(const std::string& cache_folder,
                                      const std::filesystem::path& video);
    bool valid() const { return !times_.empty(); }
    size_t frames() const { return times_.size(); }
    double first_time() const { return times_.empty() ? 0 : times_.front(); }
    double last_time() const { return times_.empty() ? 0 : times_.back(); }
    // One entry per record, in time order; `valid()` is false where the file
    // carries no sun. Empty for a pre-04 file.
    const std::vector<GoProSun>& suns() const { return suns_; }

private:
    bool decode(size_t at, GoProBinFrame& out, std::string& error) const;

    std::vector<uint8_t> bytes_;
    std::vector<size_t> starts_, ends_;
    std::vector<double> times_;
    std::vector<GoProSun> suns_;
    uint8_t version_ = 0;
};

// Fills `cfg.sun_track` from the camera-locked bin beside each video the config
// names, when one exists. The heading a video with no usable GPS track needs.
void attachSunTracks(::sfm::SfmConfig& cfg);

}  // namespace app
