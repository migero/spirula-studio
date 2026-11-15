// app/GoProBin.cpp -- the .people.bin reader; the format is the GoPro
// Player's, verified against its writer in dataset_store.h / dataset_cache.h.

#include "app/GoProBin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "sfm/SfmConfig.h"

namespace app {
namespace {

constexpr uint32_t kMaxPeople = 64;
constexpr uint32_t kMaxViews = 64;
constexpr uint32_t kMaxPoints = 1024;
constexpr uint32_t kMaxOval = 96;

uint32_t le_u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}

double le_f64(const uint8_t* p) {
    uint64_t bits = le_u32(p) | uint64_t(le_u32(p + 4)) << 32;
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

// Calls fail soft and set `bad`, so open/decode report one corruption path.
struct Cursor {
    const uint8_t* p = nullptr;
    size_t end = 0;
    size_t at = 0;
    bool bad = false;

    bool ok(size_t n) const { return end - at >= n; }
    uint32_t u32() {
        if (!ok(4)) { bad = true; return 0; }
        const uint32_t v = le_u32(p + at);
        at += 4;
        return v;
    }
    GoProVec3 v3() {
        GoProVec3 v;
        if (!ok(24)) { bad = true; return v; }
        v.x = le_f64(p + at);
        v.y = le_f64(p + at + 8);
        v.z = le_f64(p + at + 16);
        at += 24;
        return v;
    }
};

std::string corrupt(const std::filesystem::path& bin) {
    return "Corrupt GoPro tracking file: " + bin.string();
}

void walk_doubles(Cursor& c, size_t n) {
    for (size_t i = 0; i < n && !c.bad; i++) {
        const double d = le_f64(c.p + c.at);
        c.at += 8;
        if (!std::isfinite(d)) c.bad = true;
    }
}

}  // namespace

bool GoProBin::open(const std::filesystem::path& bin, std::string& error) {
    std::ifstream f(bin, std::ios::binary);
    if (!f) {
        error = "Cannot open GoPro tracking file: " + bin.string();
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size_stream = f.tellg();
    if (size_stream < 0) {
        error = "Cannot size GoPro tracking file: " + bin.string();
        return false;
    }
    const size_t size = (size_t)size_stream;
    bytes_.assign(size, 0);
    f.seekg(0);
    if (size && !f.read(reinterpret_cast<char*>(bytes_.data()),
                        (std::streamsize)size)) {
        error = "Cannot read GoPro tracking file: " + bin.string();
        return false;
    }

    if (bytes_.size() < 8 ||
        (!std::equal(bytes_.begin(), bytes_.begin() + 8, "GPDSTR06") &&
         !std::equal(bytes_.begin(), bytes_.begin() + 8, "GPDSTR05") &&
         !std::equal(bytes_.begin(), bytes_.begin() + 8, "GPDSTR04") &&
         !std::equal(bytes_.begin(), bytes_.begin() + 8, "GPDSTR03") &&
         !std::equal(bytes_.begin(), bytes_.begin() + 8, "GPDSTR02"))) {
        error = "GoPro tracking magic not recognised; the player writes "
                "GPDSTR02 to GPDSTR06: " + bin.string();
        return false;
    }
    version_ = bytes_[7] - '0';

    starts_.clear();
    ends_.clear();
    times_.clear();
    suns_.clear();

    size_t at = 8;
    if (version_ >= 3) {
        if (size < 36) { error = corrupt(bin); return false; }
        at = 36;  // flags u32 + header sun, unused now that ovals are stored
    }

    double last = -1;
    while (at < size) {
        Cursor c{bytes_.data(), size, at, false};
        if (!c.ok(12)) { error = corrupt(bin); return false; }
        const double time = le_f64(bytes_.data() + at);
        if (!std::isfinite(time) || time < 0 || time <= last) {
            error = corrupt(bin);
            return false;
        }
        last = time;
        const size_t record = at;
        c.at += 8;  // time was read raw above
        const uint32_t count = c.u32();
        if (c.bad || count > kMaxPeople) { error = corrupt(bin); return false; }
        for (uint32_t i = 0; i < count && !c.bad; i++) {
            c.u32();  // id
            c.u32();  // outline
            const uint32_t n = c.u32();
            if (c.bad || n < 3 || n > kMaxPoints || !c.ok((n + 1) * 24)) {
                c.bad = true;
                break;
            }
            walk_doubles(c, (n + 1) * 3);
            if (version_ >= 6) {  // stored shadow oval, closed loop, may be empty
                const uint32_t oc = c.u32();
                if (c.bad || oc > kMaxOval || !c.ok((size_t)oc * 24)) {
                    c.bad = true;
                    break;
                }
                walk_doubles(c, (size_t)oc * 3);
            }
        }
        if (c.bad) { error = corrupt(bin); return false; }
        uint32_t views = c.u32();
        if (c.bad || views > kMaxViews || !c.ok((size_t)views * 96)) {
            error = corrupt(bin);
            return false;
        }
        walk_doubles(c, (size_t)views * 12);
        if (c.bad) { error = corrupt(bin); return false; }
        if (version_ >= 4) {
            // Record state and carried sun. The format still has one sun per
            // frame even though this file's ovals are stored per person.
            if (!c.ok(28)) { error = corrupt(bin); return false; }
            GoProSun sun;
            sun.time = time;
            sun.state = c.u32();
            sun.dir = c.v3();
            suns_.push_back(sun);
        }
        if (c.bad || c.at > size) { error = corrupt(bin); return false; }
        starts_.push_back(record);
        ends_.push_back(c.at);
        times_.push_back(time);
        at = c.at;
    }
    return true;
}

bool GoProBin::decode(size_t at, GoProBinFrame& out, std::string& error) const {
    Cursor c{bytes_.data(), bytes_.size(), at, false};
    out = GoProBinFrame{};
    if (!c.ok(12)) { error = "Corrupt GoPro tracking record"; return false; }
    out.time = le_f64(bytes_.data() + at);
    c.at += 8;
    const uint32_t count = c.u32();
    out.people.reserve(count);
    for (uint32_t i = 0; i < count && !c.bad; i++) {
        GoProBinPerson person;
        person.id = (int)c.u32();
        person.outline = (c.u32() & 1) != 0;
        const uint32_t n = c.u32();
        if (c.bad || n < 3 || n > kMaxPoints || !c.ok((n + 1) * 24)) {
            c.bad = true;
            break;
        }
        person.center = c.v3();
        person.points.reserve(n);
        for (uint32_t j = 0; j < n && !c.bad; j++) person.points.push_back(c.v3());
        if (c.bad) break;
        if (version_ >= 6) {
            const uint32_t oc = c.u32();
            if (c.bad || oc > kMaxOval || !c.ok((size_t)oc * 24)) {
                c.bad = true;
                break;
            }
            person.oval.reserve(oc);
            for (uint32_t j = 0; j < oc && !c.bad; j++) person.oval.push_back(c.v3());
            if (c.bad) break;
        }
        out.people.push_back(std::move(person));
    }
    if (c.bad) { error = "Corrupt GoPro tracking record"; return false; }
    const uint32_t views = c.u32();
    if (c.bad || views > kMaxViews || !c.ok((size_t)views * 96)) {
        error = "Corrupt GoPro tracking record";
        return false;
    }
    walk_doubles(c, (size_t)views * 12);
    if (version_ >= 4) {
        if (!c.ok(28)) {
            error = "Corrupt GoPro tracking record";
            return false;
        }
        c.at += 28;  // record state + carried sun, unused since the ovals are stored
    }
    if (c.bad) { error = "Corrupt GoPro tracking record"; return false; }
    return true;
}

bool GoProBin::lookup(double time, double fps, GoProBinFrame& out, std::string& error) const {
    if (times_.empty()) {
        error = "GoPro tracking file has no frames";
        return false;
    }
    size_t lo = 0, hi = times_.size();
    while (lo < hi) {
        const size_t m = (lo + hi) / 2;
        if (times_[m] < time) lo = m + 1;
        else hi = m;
    }
    size_t i = std::min(lo, times_.size() - 1);
    if (i && std::fabs(times_[i - 1] - time) < std::fabs(times_[i] - time)) i--;
    if (std::fabs(times_[i] - time) > .51 / std::fmax(1., fps)) {
        error = "No GoPro tracking at " + std::to_string(time) +
                " s (nearest record is " + std::to_string(times_[i]) + " s)";
        return false;
    }
    if (!decode(starts_[i], out, error)) return false;
    return true;
}

std::filesystem::path GoProBin::find(const std::string& cache_folder,
                                     const std::filesystem::path& video) {
    const std::string name = video.filename().string() + ".people.camera.bin";
    const std::filesystem::path beside = video.parent_path() / name;
    if (std::filesystem::exists(beside)) return beside;
    if (!cache_folder.empty()) {
        const std::filesystem::path in_cache = std::filesystem::path(cache_folder) / name;
        if (std::filesystem::exists(in_cache)) return in_cache;
    }
    return {};
}

void attachSunTracks(sfm::SfmConfig& cfg) {
    if (cfg.sensor_gauge == "none") return;
    for (const sfm::TelemetryInput& in : cfg.telemetry_inputs) {
        if (in.path.empty()) continue;
        const std::filesystem::path bin = GoProBin::find("", in.path);
        if (bin.empty()) continue;
        GoProBin gb;
        std::string err;
        if (!gb.open(bin, err)) continue;
        for (const GoProSun& s : gb.suns())
            if (s.valid()) cfg.sun_track.push_back({s.time, s.dir.x, s.dir.y, s.dir.z});
        if (!cfg.sun_track.empty()) return;
    }
}

}  // namespace app
