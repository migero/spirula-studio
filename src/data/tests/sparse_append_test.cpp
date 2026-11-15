// sparse_append_test -- the dome points SparseEdit writes (SparseKeep::append).
// They have to come back out of points3D.bin as points that belong to no
// image: an empty track, an id above the file's own, and bytes that land the
// reader exactly on the end of the file. A dropped point and an appended one
// have to coexist without shifting each other.

#include "data/DatasetParser.h"
#include "data/SparseEdit.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

template <typename T>
void put(std::string& o, T v) {
    o.append(reinterpret_cast<const char*>(&v), sizeof v);
}

struct Row {
    uint64_t id = 0;
    double xyz[3] = {0, 0, 0};
    uint8_t rgb[3] = {0, 0, 0};
    double error = 0;
    uint64_t track = 0;
};

// A five-point model, each point seen by two images, as points3D.bin.
std::string make_points3d_bin(int n, int track) {
    std::string s;
    put(s, (uint64_t)n);
    for (int i = 0; i < n; i++) {
        put(s, (uint64_t)(i + 1));
        for (int k = 0; k < 3; k++) put(s, i * 10.0 + k + 0.5);
        s.append("\x11\x22\x33", 3);
        put(s, 0.7);
        put(s, (uint64_t)track);
        for (int t = 0; t < track; t++) {
            put(s, (uint32_t)t);
            put(s, (uint32_t)(t * 100));
        }
    }
    return s;
}

// Read the whole thing back, asserting the reader lands exactly at the end --
// which is what a wrong record size would break.
bool read_points3d_bin(const std::string& bytes, std::vector<Row>& rows) {
    const char* p = bytes.data();
    const char* end = p + bytes.size();
    if ((size_t)(end - p) < sizeof(uint64_t)) return false;
    uint64_t n;
    std::memcpy(&n, p, 8);
    p += 8;
    for (uint64_t i = 0; i < n; i++) {
        if ((size_t)(end - p) < 8 + 24 + 3 + 8 + 8) return false;
        Row r;
        std::memcpy(&r.id, p, 8); p += 8;
        std::memcpy(r.xyz, p, 24); p += 24;
        std::memcpy(r.rgb, p, 3); p += 3;
        std::memcpy(&r.error, p, 8); p += 8;
        std::memcpy(&r.track, p, 8); p += 8;
        if ((size_t)(end - p) < (size_t)r.track * 8) return false;
        p += (size_t)r.track * 8;
        rows.push_back(r);
    }
    return (size_t)(p - bytes.data()) == bytes.size();
}

ColmapPoints3D dome_cloud(const double xyz[6], const uint8_t rgb[6]) {
    ColmapPoints3D c;
    c.xyz.assign(xyz, xyz + 6);
    c.rgb.assign(rgb, rgb + 6);
    return c;
}

// A model on disk, so sparse_write_filtered has a baseline to filter. All
// three files, because find_colmap_model probes for them.
void write_model(const fs::path& dir) {
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "points3D.bin", std::ios::binary);
        const std::string body = make_points3d_bin(5, 2);
        f.write(body.data(), (std::streamsize)body.size());
    }
    {
        std::string cb;
        put(cb, (uint64_t)1);
        put(cb, (uint32_t)1);
        put(cb, (int32_t)1);          // PINHOLE
        put(cb, (uint64_t)640);
        put(cb, (uint64_t)480);
        for (int i = 0; i < 4; i++) put(cb, 500.0 + i);
        std::ofstream c(dir / "cameras.bin", std::ios::binary);
        c.write(cb.data(), (std::streamsize)cb.size());
    }
    {
        // One image, two observations, so the track's image ids resolve.
        std::string ib;
        put(ib, (uint64_t)1);
        put(ib, (uint32_t)1);          // image id
        for (int k = 0; k < 4; k++) put(ib, k ? 0.0 : 1.0);  // identity quat
        for (int k = 0; k < 3; k++) put(ib, 0.0);            // zero translation
        put(ib, (uint32_t)1);         // camera id
        ib += "im0.jpg";
        ib += '\0';
        put(ib, (uint64_t)2);         // two 2D features
        for (int k = 0; k < 2; k++) {
            put(ib, 100.0 + k);       // x
            put(ib, 200.0 + k);       // y
            put(ib, (uint64_t)(k == 0 ? 1 : 3));
        }
        std::ofstream f(dir / "images.bin", std::ios::binary);
        f.write(ib.data(), (std::streamsize)ib.size());
    }
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    const fs::path root = "/tmp/spare_append_test";
    fs::remove_all(root);

    // 1. No append: the file must come back byte for byte.
    {
        const fs::path d = root / "plain";
        write_model(d);
        const std::string before = read_file(d / "points3D.bin");
        spirula::SparseKeep keep;
        keep.points.assign(5, 1);
        spirula::sparse_write_filtered(d.string(), keep, nullptr, nullptr);
        check(read_file(d / "points3D.bin") == before,
              "a keep-everything write is byte-identical");
    }

    // 2. Drop one point and append two, then read it back.
    {
        const fs::path d = root / "dome";
        write_model(d);
        const double xyz[6] = {100, 0, 0, 0, 100, 0};
        const uint8_t rgb[6] = {135, 180, 220, 10, 20, 30};
        spirula::SparseKeep keep;
        keep.points = {1, 0, 1, 1, 1};          // drop the second point
        keep.append = dome_cloud(xyz, rgb);

        spirula::sparse_write_filtered(d.string(), keep, nullptr, nullptr);
        std::vector<Row> rows;
        const bool clean = read_points3d_bin(read_file(d / "points3D.bin"), rows);
        check(clean, "the appended file reads back with no trailing bytes");
        check(rows.size() == 6, "four kept points plus two dome points (" +
                                    std::to_string(rows.size()) + ")");

        // The dropped point is gone; the kept ones are untouched.
        bool ids_kept = true;
        const uint64_t want[4] = {1, 3, 4, 5};
        for (int i = 0; i < 4 && i < (int)rows.size(); i++)
            if (rows[(size_t)i].id != want[i]) ids_kept = false;
        check(ids_kept, "the kept points kept their ids and order");

        // The dome: trackless, above every id, and where we put it.
        const Row& a = rows[4];
        const Row& b = rows[5];
        check(a.track == 0 && b.track == 0, "dome points have an empty track");
        check(a.id == 6 && b.id == 7, "dome ids sit above the file's own");
        check(a.xyz[0] == 100.0 && a.xyz[1] == 0.0 && a.xyz[2] == 0.0 &&
                  b.xyz[0] == 0.0 && b.xyz[1] == 100.0,
              "dome positions survived verbatim");
        check(a.rgb[0] == 135 && a.rgb[1] == 180 && a.rgb[2] == 220,
              "dome colour survived verbatim");
        check(a.error == 0.0, "a trackless point carries error 0");
        // The kept points kept their tracks.
        bool tracks_intact = true;
        for (int i = 0; i < 4; i++) if (rows[(size_t)i].track != 2) tracks_intact = false;
        check(tracks_intact, "the real points kept their 2-observation tracks");
    }

    // 3. The same write twice must not accumulate: the baseline is the file as
    //    first found, so the dome is re-added rather than doubled.
    {
        const fs::path d = root / "twice";
        write_model(d);
        const double xyz[6] = {100, 0, 0, 0, 100, 0};
        const uint8_t rgb[6] = {135, 180, 220, 10, 20, 30};
        spirula::SparseBaseline base;
        for (int pass = 0; pass < 2; pass++) {
            spirula::SparseKeep keep;
            keep.points.assign(5, 1);
            keep.append = dome_cloud(xyz, rgb);
            spirula::sparse_write_filtered(d.string(), keep, nullptr, &base);
        }
        std::vector<Row> rows;
        read_points3d_bin(read_file(d / "points3D.bin"), rows);
        check(rows.size() == 7, "a second save does not double the dome (" +
                                    std::to_string(rows.size()) + ")");
    }

    // 4. A placement moves the dome exactly as it moves the real points -- the
    //    dome is added in the parsed frame, so skipping this would leave the
    //    sky behind while the ground moved.
    {
        const fs::path d = root / "moved";
        write_model(d);
        const double xyz[6] = {100, 0, 0, 0, 100, 0};
        const uint8_t rgb[6] = {135, 180, 220, 10, 20, 30};
        spirula::Sim3 moved;
        moved.s = 2.0;
        moved.t[0] = 1000.0;
        spirula::SparseKeep keep;
        keep.points.assign(5, 1);
        keep.append = dome_cloud(xyz, rgb);
        spirula::sparse_write_filtered(d.string(), keep, &moved, nullptr);

        std::vector<Row> rows;
        check(read_points3d_bin(read_file(d / "points3D.bin"), rows),
              "a moved model still reads back cleanly");
        // First real point was (0.5, 1.5, 2.5): scale 2 and +1000 in x.
        check(std::abs(rows[0].xyz[0] - (1000.0 + 2.0 * 0.5)) < 1e-9 &&
                  std::abs(rows[0].xyz[1] - 2.0 * 1.5) < 1e-9,
              "the real points took the placement");
        // Dome point (100, 0, 0) -> (1200, 0, 0).
        check(std::abs(rows[5].xyz[0] - 1200.0) < 1e-9 &&
                  std::abs(rows[5].xyz[1] - 0.0) < 1e-9,
              "the dome took the same placement (" +
                  std::to_string(rows[5].xyz[0]) + ")");
    }

    // 5. The parser reads the appended cloud as points, which is the whole
    //    point: the trainer must see it.
    {
        const fs::path d = root / "parsed";
        write_model(d);
        const double xyz[6] = {100, 0, 0, 0, 100, 0};
        const uint8_t rgb[6] = {135, 180, 220, 10, 20, 30};
        spirula::SparseKeep keep;
        keep.points.assign(5, 1);
        keep.append = dome_cloud(xyz, rgb);
        spirula::sparse_write_filtered(d.string(), keep, nullptr, nullptr);

        // The parser wants the image itself, so give it a real (tiny) one.
        fs::create_directories(d / "images");
        {
            std::ofstream img(d / "images" / "im0.jpg", std::ios::binary);
            img << "\xff\xd8\xff\xe0";       // a JPEG header is enough here
        }
        DatasetParserConfig pc;
        ParsedDataset ds = parse_dataset(d.string(), pc, "colmap");
        check(ds.points.num() == 7, "the parser sees all seven points (" +
                                         std::to_string(ds.points.num()) + ")");
        // read_points3D_binary skips the track, so this cannot fail on the
        // trackless rows -- which is the point of them being trackless.
        check(ds.points.xyz.size() == 21, "xyz is [7,3]");
        check(ds.points.rgb.size() == 21, "rgb is [7,3]");
    }

    // 6. The PLY path carries them too, for a dataset with no reconstruction.
    {
        const fs::path p = root / "cloud.ply";
        ColmapPoints3D base;
        for (int i = 0; i < 3; i++) {
            base.xyz.push_back(i * 1.0);
            base.xyz.push_back(i * 2.0);
            base.xyz.push_back(i * 3.0);
            for (int k = 0; k < 3; k++) base.rgb.push_back((uint8_t)(10 * i + k));
        }
        const double xyz[6] = {100, 0, 0, 0, 100, 0};
        const uint8_t rgb[6] = {135, 180, 220, 10, 20, 30};
        const ColmapPoints3D dome = dome_cloud(xyz, rgb);
        spirula::write_ply_points(p.string(), base.xyz.data(), base.rgb.data(),
                                  base.num(), nullptr, nullptr, &dome);
        ColmapPoints3D back = read_ply_points(p.string());
        check(back.num() == 5, "the PLY holds the three points and the dome (" +
                                   std::to_string(back.num()) + ")");
        check(back.xyz.size() >= 15 &&
                  std::abs(back.xyz[9] - 100.0) < 1e-4 &&
                  std::abs(back.xyz[13] - 100.0) < 1e-4,
              "the dome's points are in the PLY");
    }

    fs::remove_all(root);
    std::printf("%s\n", g_failures ? "FAILURES" : "all checks passed");
    return g_failures ? 1 : 0;
}