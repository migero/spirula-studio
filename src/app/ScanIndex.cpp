#include "app/ScanIndex.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace app {

namespace {

// The same FNV-1a the mask editor's index.json uses for its fingerprints.
uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

constexpr uint32_t kMagic = 0x53535241u;   // "SSRA", little-endian
constexpr uint32_t kVersion = 1;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_bytes;
    uint64_t frames;
    uint64_t checksum;      // over the words, not the header
    int64_t  last_full;
};
#pragma pack(pop)

}  // namespace

void ScanIndex::resize(size_t frames) {
    words_.assign(frames, 0);
    last_full_ = -1;
    magic_ = kMagic;
}

bool ScanIndex::scanned(size_t frame, int view) const {
    if (frame >= words_.size() || view < 0 || view >= 16) return false;
    return (words_[frame] >> view) & 1u;
}

void ScanIndex::mark_scanned(size_t frame, int view) {
    if (frame >= words_.size() || view < 0 || view >= 16) return;
    words_[frame] |= (uint16_t)(1u << view);
}

bool ScanIndex::due_for_full_scan(int64_t frame, int every) const {
    if (every < 1) every = 1;
    if (last_full_ < 0) return true;
    return frame - last_full_ >= every;
}

std::vector<int> ScanIndex::scanned_views(size_t frame) const {
    std::vector<int> out;
    if (frame >= words_.size()) return out;
    for (int v = 0; v < 16; v++)
        if ((words_[frame] >> v) & 1u) out.push_back(v);
    return out;
}

size_t ScanIndex::uncached_frames(size_t frames) const {
    size_t n = 0;
    for (size_t i = 0; i < frames && i < words_.size(); i++)
        if (words_[i] == 0) ++n;
    return n;
}

bool ScanIndex::save(const std::string& path, size_t frames, std::string& error) const {
    if (words_.size() != frames) {
        error = "scan index size does not match the frame count";
        return false;
    }
    // A temp file and a rename, so an interrupted write cannot leave a
    // half-written index that would make a later run skip a search.
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "cannot write " + tmp;
        return false;
    }
    Header h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.entry_bytes = kScanIndexEntryBytes;
    h.frames = frames;
    h.last_full = last_full_;
    const size_t bytes = words_.size() * sizeof(uint16_t);
    const auto* raw = reinterpret_cast<const uint8_t*>(words_.data());
    h.checksum = bytes ? fnv1a64(raw, bytes) : 0;
    f.write(reinterpret_cast<const char*>(&h), sizeof h);
    if (bytes) f.write(reinterpret_cast<const char*>(raw), (std::streamsize)bytes);
    f.flush();
    if (!f) {
        error = "cannot write " + tmp;
        return false;
    }
    f.close();
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot move the scan index into place";
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool ScanIndex::load(const std::string& path, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot read " + path;
        return false;
    }
    Header h{};
    f.read(reinterpret_cast<char*>(&h), sizeof h);
    if (!f || h.magic != kMagic || h.version != kVersion) {
        error = "not a scan index";
        return false;
    }
    if (h.entry_bytes != kScanIndexEntryBytes) {
        error = "scan index entry size does not match this build";
        return false;
    }
    std::vector<uint16_t> words((size_t)h.frames);
    const size_t bytes = words.size() * sizeof(uint16_t);
    if (bytes) {
        f.read(reinterpret_cast<char*>(words.data()), (std::streamsize)bytes);
        if (!f) {
            error = "scan index is truncated";
            return false;
        }
    }
    // Verified before anything is adopted: a corrupt cache must be able to cost
    // time, never a skipped search.
    const auto* raw = reinterpret_cast<const uint8_t*>(words.data());
    if ((bytes ? fnv1a64(raw, bytes) : 0) != h.checksum) {
        error = "scan index failed its checksum";
        return false;
    }
    words_.swap(words);
    last_full_ = h.last_full;
    magic_ = kMagic;
    return true;
}

}  // namespace app