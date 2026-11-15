// The gate: which of ten views a frame searches, and what a resume knows. Each
// property below is one the gate could quietly break: cold start searching all
// views, a tracked person narrowing it, the cadence still scanning everything (a
// person appearing mid-capture is found only there), the index round-tripping
// and refusing a corrupt file, and a person landing whole in every view that can
// see him and in none of the rest.

#include "app/PersonRays.h"
#include "app/PersonStore.h"
#include "app/ScanIndex.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

using app::Pano360Layout;
using app::Pano360Options;
using app::Pano360View;
using app::PersonRays;
using app::PersonStore;
using app::PersonTrack;
using app::ScanIndex;

// A solid person, masked OUT: 0 where he is, 255 elsewhere.
std::vector<uint8_t> person_box(int w, int h, int cx, int cy, int half) {
    std::vector<uint8_t> m((size_t)w * h, 255);
    for (int y = cy - half; y <= cy + half; y++)
        for (int x = cx - half; x <= cx + half; x++)
            if (x >= 0 && y >= 0 && x < w && y < h) m[(size_t)y * w + x] = 0;
    return m;
}

PersonRays person_on(const Pano360View& v, int cx, int cy, int half, int stride) {
    const auto m = person_box(v.width, v.height, cx, cy, half);
    return app::rays_from_detections(v, m, v.width, v.height, stride, false);
}

}  // namespace

int main() {
    Pano360Layout l;
    l.packing = app::Pano360Packing::Eac;
    l.track_w = 4096;
    l.track_h = 1344;
    l.face = 1344;
    l.strip = 32;
    Pano360Options o;
    o.mode = app::Pano360Mode::Faces;
    const std::vector<Pano360View> views = app::pano360_views(l, o);
    const int N = (int)views.size();

    std::printf("Gate over %d views\n", N);

    // ---- cold start searches everything ----
    {
        PersonStore store;
        ScanIndex index;
        index.resize(64);
        const std::vector<int> got =
            app::gate_search_views(store, views, 0, N, 0, index, 25);
        check((int)got.size() == N,
              "an empty store searches every view: no search is skipped cold");
        check((int)index.scanned_views(0).size() == N,
              "and the index records all of them as scanned");
        check(index.last_full_scan() == 0, "the first frame is a full scan");
    }

    // ---- a known person narrows the search ----
    {
        PersonStore store;
        store.configure(0.98);
        // The operator of a handheld camera is below the rear lens: measured, a
        // person at cam7's centre is in cam7 and no other view.
        const PersonRays op = person_on(views[7], views[7].width / 2,
                                        views[7].height / 2, 120, 6);
        check(!op.empty(), "a person on cam7 becomes rays");
        store.add(op, 0.2);
        check(store.size() == 1, "and is one track");

        ScanIndex index;
        index.resize(64);
        index.set_last_full_scan(0);
        for (int f = 1; f < 25; f++) {
            const std::vector<int> got =
                app::gate_search_views(store, views, f, N, 0, index, 25);
            std::printf("        frame %d searches %zu views:", f, got.size());
            for (int v : got) std::printf(" cam%d", v);
            std::printf("\n");
            check(!got.empty(), "a tracked person is still searched for");
            check((int)got.size() < N,
                  "and the search is narrower than a full scan: this is the saving");
        }
    }

    // ---- the cadence still scans everything ----
    {
        PersonStore store;
        store.configure(0.98);
        store.add(person_on(views[7], views[7].width / 2, views[7].height / 2, 120, 6),
                  0.2);
        ScanIndex index;
        index.resize(64);
        index.set_last_full_scan(0);
        const std::vector<int> gated =
            app::gate_search_views(store, views, 1, N, 0, index, 25);
        const std::vector<int> forced =
            app::gate_search_views(store, views, 25, N, 0, index, 25);
        std::printf("        frame 1: %zu views, frame 25: %zu views\n", gated.size(),
                    forced.size());
        check((int)gated.size() < N, "a mid-interval frame is gated");
        check((int)forced.size() == N,
              "the interval frame is a full scan: a new person is still found");
    }

    // ---- a person appearing mid-capture ----
    {
        // This is the case gating could break, so it is checked directly: with
        // only the operator known, a second person is found on a full scan and
        // becomes its own track.
        PersonStore store;
        store.configure(0.98);
        store.add(person_on(views[7], views[7].width / 2, views[7].height / 2, 120, 6),
                  0.2);
        check(store.size() == 1, "one person known");
        ScanIndex index;
        index.resize(64);
        index.set_last_full_scan(0);
        app::gate_search_views(store, views, 1, N, 0, index, 25);
        check(index.due_for_full_scan(25, 25), "frame 25 is due");
        const std::vector<int> full =
            app::gate_search_views(store, views, 25, N, 0, index, 25);
        check((int)full.size() == N, "so every view is searched");
        // And a newcomer in a different view is its own track, not merged into
        // the operator.
        store.add(person_on(views[0], views[0].width / 2, views[0].height / 2, 120, 6),
                  0.2);
        check(store.size() == 2, "a second person is a second track, not a merge");
    }

    // ---- projection ----
    {
        PersonStore store;
        store.configure(0.98);
        store.add(person_on(views[7], views[7].width / 2, views[7].height / 2, 120, 6),
                  0.2);
        std::vector<uint8_t> in7, in0;
        app::project_tracks(store, views[7], views[7].width, views[7].height, in7, 6,
                          0);
        app::project_tracks(store, views[0], views[0].width, views[0].height, in0, 6,
                          0);
        const int n7 = (int)std::count(in7.begin(), in7.end(), (uint8_t)0);
        const int n0 = (int)std::count(in0.begin(), in0.end(), (uint8_t)0);
        std::printf("        cam7 %d px, cam0 %d px\n", n7, n0);
        check(n7 > 0, "the person is written into the view he is in");
        check(n0 == 0, "and into no view that cannot see him: 255 elsewhere, not 0");
        // 255 = KEEP is the convention, so an untouched pixel must be 255 and a
        // person's 0 -- the opposite of a raw detection mask.
        check(std::all_of(in0.begin(), in0.end(), [](uint8_t v) { return v == 255; }),
              "an empty view is all KEEP, the convention the trainer reads");
    }

    // ---- the index round-trips, and a corrupt one is refused ----
    {
        ScanIndex a;
        a.resize(100);
        a.mark_scanned(3, 7);
        a.mark_scanned(3, 2);
        a.mark_scanned(50, 9);
        a.set_last_full_scan(50);
        const std::string path = "/tmp/opencode/scanindex_test.bin";
        std::string err;
        check(a.save(path, 100, err), "the index saves");

        ScanIndex b;
        check(b.load(path, err), "and loads back");
        check(b.scanned(3, 7) && b.scanned(3, 2), "with its bits");
        check(!b.scanned(3, 8), "and only the bits it had");
        check(b.last_full_scan() == 50, "and the full-scan frame");
        check(b.uncached_frames(100) == 98, "and it knows what a cold start would redo");

        // Corrupt one PAYLOAD byte: the header is 36 bytes (packed), so a write before
        // that would only damage the header the length check catches instead.
        FILE* f = std::fopen(path.c_str(), "r+b");
        check(f != nullptr, "the file opens for corruption");
        if (f) {
            std::fseek(f, 36 + 1, SEEK_SET);
            const unsigned char c = 0xff;
            std::fwrite(&c, 1, 1, f);
            std::fclose(f);
        }
        ScanIndex c;
        std::string why;
        check(!c.load(path, why), "a corrupt index is refused");
        check(c.empty(), "and leaves the reader empty: a cold start, not a skip");
        std::printf("        refusal: %s\n", why.c_str());

        // Out-of-range reads are "not scanned", so a short index re-scans rather
        // than skipping.
        ScanIndex d;
        d.resize(10);
        check(!d.scanned(1000, 0), "a frame past the end is never 'already scanned'");
        check(!d.scanned(0, 99), "nor is a view that does not exist");
        std::remove(path.c_str());
    }

    std::printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}