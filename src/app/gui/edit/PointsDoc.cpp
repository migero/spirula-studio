// PointsDoc.cpp -- see PointsDoc.h.

#include "app/gui/edit/PointsDoc.h"

#include "data/SkyDome.h"
#include "i18n/catalog/Edit.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
namespace msg = spirula::i18n::msg::edit;

namespace gui {

namespace {

constexpr uint8_t kTint[3] = {255, 108, 13};

// Keep the rows of every per-camera array a parser filled. A dataset that
// carries none of an optional array keeps carrying none.
template <typename T>
void keep_rows(std::vector<T>& v, int64_t n, int stride, const uint8_t* keep) {
    if ((int64_t)v.size() != n * stride) return;
    std::vector<T> out;
    out.reserve(v.size());
    for (int64_t i = 0; i < n; i++) {
        if (!keep[i]) continue;
        out.insert(out.end(), v.begin() + (ptrdiff_t)(i * stride),
                   v.begin() + (ptrdiff_t)((i + 1) * stride));
    }
    v.swap(out);
}

// The rows that were appended rather than read, which is the dome, in the
// frame the writer wants: the file's own. The parser centred everything in
// _ds, so this puts the tail back the way the PLY branch does.
static ColmapPoints3D appended_in_file_frame(const ParsedDataset& ds,
                                             int64_t base, int64_t total) {
    ColmapPoints3D out;
    const int64_t n = std::max<int64_t>(total - base, 0);
    out.xyz.reserve((size_t)n * 3);
    out.rgb.reserve((size_t)n * 3);
    for (int64_t i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++)
            out.xyz.push_back(ds.points.xyz[(size_t)(base + i) * 3 + (size_t)k] +
                              ds.center[(size_t)k]);
        for (int k = 0; k < 3; k++)
            out.rgb.push_back(ds.points.rgb.empty()
                                  ? (uint8_t)200
                                  : ds.points.rgb[(size_t)(base + i) * 3 + (size_t)k]);
    }
    return out;
}

}  // namespace


PointsDoc::PointsDoc(ParsedDataset ds, PostSplitCameras post,
                     const std::string& source, const std::string& dataset_dir,
                     Show show)
    : _ds(std::move(ds)), _post(std::move(post)), _dataset_dir(dataset_dir),
      _show(std::move(show)) {
    if (!_dataset_dir.empty()) _fmt = spirula::sparse_format_of(_dataset_dir);

    // The preview draws in the normalized frame, so the selection has to
    // project there too: train_to_normalized is stored the other way round.
    double A[16];
    dsparse::train_to_normalized_inverse(_ds, A);
    auto map = [&A](const double* p, float* out) {
        for (int r = 0; r < 3; r++)
            out[r] = (float)(A[r*4+0]*p[0] + A[r*4+1]*p[1] + A[r*4+2]*p[2] +
                             A[r*4+3]);
    };

    const int64_t n = _ds.points.num();
    std::vector<float> pos((size_t)n * 3);
    for (int64_t i = 0; i < n; i++)
        map(&_ds.points.xyz[(size_t)i * 3], &pos[(size_t)i * 3]);
    set_source(source);
    add_layer(msg::elem_point, n, std::move(pos));

    // A camera is its centre: the translation column of its camera-to-world.
    const int64_t nc = _ds.num_cameras;
    if (nc > 0) {
        std::vector<float> cam((size_t)nc * 3);
        for (int64_t i = 0; i < nc; i++) {
            const double c[3] = {_ds.c2w[(size_t)i * 12 + 3],
                                 _ds.c2w[(size_t)i * 12 + 7],
                                 _ds.c2w[(size_t)i * 12 + 11]};
            map(c, &cam[(size_t)i * 3]);
        }
        add_layer(msg::elem_camera, nc, std::move(cam));
    }

    _display = _ds;
    _post_display = _post;
    rebuild_display(false);
}


// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void PointsDoc::rebuild_display(bool cameras_changed) {
    static const std::vector<uint8_t> kNoCameras;
    const std::vector<uint8_t>& pk = alive_of(kPoints);
    const std::vector<uint8_t>& ck =
        layer_count() > kCameras ? alive_of(kCameras) : kNoCameras;

    // Re-baking the split table costs what a dataset's camera count costs, so
    // it happens when that set changes rather than on every point deleted.
    if (cameras_changed && !ck.empty()) {
        _display = _ds;
        const int64_t nc = _ds.num_cameras;
        keep_rows(_display.camera_models, nc, 1, ck.data());
        keep_rows(_display.camera_distortions, nc, 1, ck.data());
        keep_rows(_display.image_filenames, nc, 1, ck.data());
        keep_rows(_display.mask_filenames, nc, 1, ck.data());
        keep_rows(_display.depth_filenames, nc, 1, ck.data());
        keep_rows(_display.normal_filenames, nc, 1, ck.data());
        keep_rows(_display.widths, nc, 1, ck.data());
        keep_rows(_display.heights, nc, 1, ck.data());
        keep_rows(_display.c2w, nc, 12, ck.data());
        keep_rows(_display.intrins, nc, 4, ck.data());
        keep_rows(_display.dist_coeffs, nc, 8, ck.data());
        keep_rows(_display.redistort, nc, 1, ck.data());
        keep_rows(_display.exif_quarter_turns, nc, 1, ck.data());
        _display.num_cameras = (int64_t)_display.widths.size();
        // The split table is derived, so it is rebuilt rather than filtered;
        // its rows are per FACE, which is not one per camera.
        _display.train_indices.resize((size_t)_display.num_cameras);
        for (size_t i = 0; i < _display.train_indices.size(); i++)
            _display.train_indices[i] = (int32_t)i;
        _display.val_indices.clear();
        _post_display = bake_post_split(_display, false, false);
    }

    // The points, filtered and tinted.
    const int64_t n = (int64_t)pk.size();
    ColmapPoints3D& out = _display.points;
    out.xyz.clear();
    out.rgb.clear();
    out.xyz.reserve((size_t)n * 3);
    out.rgb.reserve((size_t)n * 3);
    const uint8_t* psel_w = sel_of(kPoints).data();
    for (int64_t i = 0; i < n; i++) {
        if (!pk[(size_t)i]) continue;
        for (int k = 0; k < 3; k++) out.xyz.push_back(_ds.points.xyz[(size_t)i*3+k]);
        for (int k = 0; k < 3; k++) {
            const uint8_t base = _ds.points.rgb.empty()
                                     ? (uint8_t)200
                                     : _ds.points.rgb[(size_t)i * 3 + k];
            out.rgb.push_back(psel_w && psel_w[i] ? kTint[k] : base);
        }
    }

    // One flag per camera of the DISPLAY dataset, which is the live subset in
    // its own order -- what the frusta are drawn from.
    _cam_highlight.assign((size_t)_display.num_cameras, 0);
    if (ck.empty()) return;
    const Selection& csel = sel_of(kCameras);
    size_t live = 0;
    for (size_t i = 0; i < ck.size() && live < _cam_highlight.size(); i++) {
        if (!ck[i]) continue;
        _cam_highlight[live++] = csel.weight((int64_t)i) ? 1 : 0;
    }
}

void PointsDoc::publish_impl(bool geometry) {
    if (!_show) return;
    const int64_t live_cams =
        layer_count() > kCameras ? alive_count_of(kCameras) : 0;
    const bool cameras_changed = geometry && live_cams != _live_cameras;
    _live_cameras = live_cams;
    rebuild_display(cameras_changed);
    _show(_display, _post_display,
          _cam_highlight.empty() ? nullptr : _cam_highlight.data());
}

// Points AND cameras, both filtered to what is live and both taken from the
// parsed dataset, which is the one frame they are already in together.
bool PointsDoc::live_centers(dsparse::CenterTable& out) const {
    const std::vector<uint8_t>& pk = alive_of(kPoints);
    std::vector<double> pts;
    for (size_t i = 0; i < pk.size(); i++) {
        if (!pk[i]) continue;
        pts.insert(pts.end(), _ds.points.xyz.begin() + (ptrdiff_t)(i * 3),
                   _ds.points.xyz.begin() + (ptrdiff_t)(i * 3 + 3));
    }
    std::vector<double> c2w;
    if (layer_count() > kCameras) {
        const std::vector<uint8_t>& ck = alive_of(kCameras);
        for (size_t i = 0; i < ck.size(); i++) {
            if (!ck[i]) continue;
            c2w.insert(c2w.end(), _ds.c2w.begin() + (ptrdiff_t)(i * 12),
                       _ds.c2w.begin() + (ptrdiff_t)(i * 12 + 12));
        }
    }
    if (pts.empty() && c2w.empty()) return false;
    double A[16];
    dsparse::train_to_normalized_inverse(_ds, A);
    out = dsparse::scene_centers(c2w.empty() ? nullptr : c2w.data(),
                                 (int64_t)c2w.size() / 12,
                                 pts.empty() ? nullptr : pts.data(),
                                 (int64_t)pts.size() / 3, 3, A);
    return true;
}

// RAW file coordinates -> the normalized frame: the parser's centring shift,
// then the inverse of train_to_normalized.
spirula::Sim3 PointsDoc::view_frame() const {
    double A[16];
    dsparse::train_to_normalized_inverse(_ds, A);
    spirula::Sim3 shift;
    for (int i = 0; i < 3; i++) shift.t[i] = -_ds.center[(size_t)i];
    return spirula::Sim3::from_3x4(A) * shift;
}

std::array<double, 3> PointsDoc::file_up_in_view() const {
    // Not up_hint(): the view frame's own +Z still follows the cameras' mean
    // up. This pushes file +Z into the view frame instead, which is what makes
    // the dome LEVEL once it is written back.
    double A[16];
    dsparse::train_to_normalized_inverse(_ds, A);
    const double u[3] = {A[2], A[6], A[10]};   // A * (0, 0, 1)
    const double len = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(len > 1e-9)) {
        float h[3];
        return up_hint(h) ? std::array<double, 3>{h[0], h[1], h[2]}
                          : std::array<double, 3>{0, 0, 1};
    }
    return {u[0] / len, u[1] / len, u[2] / len};
}

bool PointsDoc::up_hint(float up[3]) const {
    double acc[3] = {0, 0, 0};
    if (_ds.gauge_oriented) {
        // The gauge measured this model's up from gravity, so its own +Z is it.
        acc[2] = 1.0;
    } else {
        if (layer_count() <= kCameras) return false;
        const std::vector<uint8_t>& ck = alive_of(kCameras);
        for (size_t i = 0; i < ck.size(); i++) {
            if (!ck[i]) continue;
            // OpenGL camera-to-world: the second column is the camera's up.
            for (int r = 0; r < 3; r++) acc[r] += _ds.c2w[i * 12 + r * 4 + 1];
        }
    }
    const spirula::Sim3 n = view_frame();
    double v[3];
    n.rotate(acc, v);
    const double len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (!(len > 1e-9)) return false;
    for (int r = 0; r < 3; r++) up[r] = (float)(v[r] / len);
    return true;
}

const spirula::SparseStats* PointsDoc::sparse_stats() const {
    if (!_stats_read) {
        _stats_read = true;
        try {
            if (!_dataset_dir.empty()) _stats = spirula::read_sparse_stats(_dataset_dir);
        } catch (const std::exception&) {
            _stats = spirula::SparseStats{};
        }
        // A model edited and saved by an earlier session still lines up; one
        // whose point count disagrees with what was parsed does not.
        if ((int64_t)_stats.track_beg.size() != _ds.points.num() + 1)
            _stats = spirula::SparseStats{};
    }
    return _stats.empty() ? nullptr : &_stats;
}

bool PointsDoc::colours(std::vector<float>& rgb) const {
    if (layer() != kPoints || _ds.points.rgb.empty()) return false;
    rgb.resize(_ds.points.rgb.size());
    for (size_t i = 0; i < rgb.size(); i++) rgb[i] = _ds.points.rgb[i] / 255.0f;
    return true;
}

// Live cameras only, read straight off the camera layer.
std::vector<float> PointsDoc::camera_centres() const {
    std::vector<float> out;
    if (layer_count() <= kCameras) return out;
    const std::vector<uint8_t>& ck = alive_of(kCameras);
    const float* p = positions_of(kCameras);
    for (size_t i = 0; i < ck.size(); i++)
        if (ck[i]) out.insert(out.end(), p + i * 3, p + i * 3 + 3);
    return out;
}

void PointsDoc::revert_display() {
    if (_show) _show(_ds, _post, nullptr);
}

void PointsDoc::rows_appended(int layer, int64_t first, int64_t count,
                             const uint8_t* rgb) {
    if (layer != kPoints) return;
    // The layer holds the same rows in the viewport's frame; _ds holds them in
    // the parsed one, so take them back rather than recomputing.
    const float* view = positions_of(layer);
    std::vector<double> back((size_t)count * 3);
    // NOT view_frame().inverse(): that also carries the shift from raw file to
    // parsed, so inverting it applies the shift twice. train_to_normalized is
    // stored the other way round, so it already maps view back to parsed.
    const std::array<float, 16>& back_to_parsed = _ds.train_to_normalized;
    for (int64_t i = 0; i < count; i++) {
        const double p[3] = {view[(first + i) * 3], view[(first + i) * 3 + 1],
                             view[(first + i) * 3 + 2]};
        for (int r = 0; r < 3; r++) {
            const float* row = back_to_parsed.data() + r * 4;
            back[(size_t)i * 3 + (size_t)r] = (double)row[0] * p[0] +
                                              (double)row[1] * p[1] +
                                              (double)row[2] * p[2] + row[3];
        }
    }
    _ds.points.xyz.insert(_ds.points.xyz.end(), back.begin(), back.end());
    if (rgb) _ds.points.rgb.insert(_ds.points.rgb.end(), rgb, rgb + (size_t)count * 3);
    else     _ds.points.rgb.insert(_ds.points.rgb.end(), (size_t)count * 3, (uint8_t)200);
    // The tracks no longer line up, so every attribute read out of the file's
    // own point rows is gone -- as it is for any save that changes the count.
    _stats_read = true;
    _stats = spirula::SparseStats{};
}

void PointsDoc::rows_dropped(int layer, int64_t count) {
    if (layer != kPoints) return;
    const size_t n = (size_t)count * 3;
    if (_ds.points.xyz.size() < n || _ds.points.rgb.size() < n) return;
    _ds.points.xyz.resize(_ds.points.xyz.size() - n);
    _ds.points.rgb.resize(_ds.points.rgb.size() - n);
    _stats_read = true;
    _stats = spirula::SparseStats{};
}

// What the dome would measure, without adding it: the dome in the viewport's
// frame (the layer is what the op appends to and the save reads), and what it
// came out at. `layer_pos` may be null to measure only.
static bool measure_dome(const PointsDoc& d, int subdivisions, double percentile,
                         std::string* error, std::vector<float>* layer_pos,
                         std::vector<uint8_t>* rgb_out, double* radius_out,
                         int64_t* points_out) {
    // Measured on the points as they stand: a deleted keypoint must not hold
    // the radius out, and neither must a dome already in the file, whose own
    // points sit exactly ON the sphere a new one would be sized against.
    std::vector<uint8_t> alive = d.alive_of(0);
    const ParsedDataset& ds = *d.dataset();
    const ColmapPoints3D& cloud = ds.points;
    if ((int64_t)cloud.trackless.size() == cloud.num())
        for (int64_t i = 0; i < cloud.num(); i++)
            if (cloud.trackless[(size_t)i]) alive[(size_t)i] = 0;
    const int64_t n = cloud.num();

    SkyDomeConfig cfg;
    cfg.subdivisions = subdivisions;
    cfg.percentile = percentile;

    double A[16];
    dsparse::train_to_normalized_inverse(ds, A);
    auto to_view = [&](const double* p, double* out) {
        for (int r = 0; r < 3; r++)
            out[r] = A[r*4+0]*p[0] + A[r*4+1]*p[1] + A[r*4+2]*p[2] + A[r*4+3];
    };
    std::vector<double> pts_view((size_t)n * 3);
    for (int64_t i = 0; i < n; i++)
        to_view(&ds.points.xyz[(size_t)i * 3], &pts_view[(size_t)i * 3]);

    // The camera centres, in the same frame and from the camera layer, so a
    // camera the user deleted cannot place the sky.
    std::vector<double> cams;
    if (d.layer_count() > 1) {
        const std::vector<uint8_t>& ck = d.alive_of(1);
        const float* cp = d.positions_of(1);
        for (size_t i = 0; i < ck.size(); i++) {
            if (!ck[i]) continue;
            cams.insert(cams.end(), cp + i * 3, cp + i * 3 + 3);
        }
    }
    // Up in the view frame, level with the file frame's own +Z.
    const std::array<double, 3> fuv = d.file_up_in_view();
    double up[3] = {fuv[0], fuv[1], fuv[2]};

    SkyDomeFit fit;
    ColmapPoints3D dome;
    try {
        dome = sky_dome_points(cfg, cams, &alive, pts_view.data(), n, up, &fit);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    if (radius_out) *radius_out = fit.radius;
    if (points_out) *points_out = fit.points;
    if (rgb_out) *rgb_out = dome.rgb;
    // Already in the view frame: the dome was built about a centre taken from
    // pts_view, which is the frame the layer holds. Mapping it again would
    // shrink it by the frame's scale.
    if (layer_pos) layer_pos->assign(dome.xyz.begin(), dome.xyz.end());
    return true;
}

PointsDoc::DomeFit PointsDoc::measure_sky_dome(int subdivisions, double percentile,
                                              std::string* error) const {
    DomeFit out;
    if (!measure_dome(*this, subdivisions, percentile, error, nullptr, nullptr,
                      &out.radius, &out.points))
        return DomeFit{};
    out.present = true;
    return out;
}

PointsDoc::DomeFit PointsDoc::add_sky_dome(int subdivisions, double percentile,
                                           std::string* error) {
    std::vector<float> layer_pos;
    std::vector<uint8_t> rgb;
    DomeFit out;
    if (!measure_dome(*this, subdivisions, percentile, error, &layer_pos, &rgb,
                      &out.radius, &out.points))
        return DomeFit{};
    // One op, so undo is one Ctrl+Z. rows_appended keeps _ds.points in step.
    run(make_sky_dome_op(*this, layer_pos, rgb));
    out.present = true;
    return out;
}


// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------

std::vector<SaveTarget> PointsDoc::save_targets() const {
    std::vector<SaveTarget> t;
    switch (_fmt) {
        case spirula::SparseFormat::Colmap:
            t.push_back({&msg::target_colmap, "", true});
            break;
        case spirula::SparseFormat::Nerfstudio:
        case spirula::SparseFormat::Metashape:
            t.push_back({&msg::target_nerfstudio, "", true, false});
            break;
        default:
            break;
    }
    t.push_back({&msg::target_points_ply, ".ply", false});
    return t;
}

std::string PointsDoc::default_save_path(int target) const {
    const std::vector<SaveTarget> t = save_targets();
    if (target < 0 || target >= (int)t.size()) return {};
    return t[(size_t)target].folder ? _dataset_dir : source_path();
}

void PointsDoc::save(int target, const std::string& path,
                     std::atomic<int>* progress) {
    const std::vector<SaveTarget> t = save_targets();
    if (target < 0 || target >= (int)t.size()) return;
    if (t[(size_t)target].folder) {
        spirula::SparseKeep keep;
        keep.points = alive_of(kPoints);
        // The dome, which no row of the file's is: it is appended rather than
        // filtered, and in the file's own frame.
        const int64_t appended = appended_on(kPoints);
        if (appended > 0)
            keep.append = appended_in_file_frame(
                _ds, (int64_t)_ds.points.num() - appended, _ds.points.num());
        const std::vector<uint8_t>& ck = alive_of(kCameras);
        for (size_t i = 0; i < ck.size(); i++)
            if (!ck[i] && i < _ds.image_filenames.size())
                keep.drop_images.push_back(_ds.image_filenames[i]);
        const spirula::Sim3 moved = file_placement();
        std::error_code ec;
        if (fs::equivalent(path, _dataset_dir, ec)) {
            spirula::sparse_write_filtered(_dataset_dir, keep, &moved, &_baseline);
        } else {
            spirula::sparse_write_copy(_dataset_dir, path, keep, &moved, &_baseline);
            // A copy trains from the same pictures. Best effort: where links
            // cannot be made, the trainer is pointed at the source's folders.
            for (const char* sub : {"images", "masks"}) {
                const fs::path from = fs::path(_dataset_dir) / sub;
                if (fs::is_directory(from, ec) && !fs::exists(fs::path(path) / sub, ec))
                    fs::create_directory_symlink(fs::absolute(from, ec), fs::path(path) / sub, ec);
            }
        }
        if (progress) (*progress)++;
        return;
    }
    // A loose PLY is in the parsed frame, which the centring shift left.
    spirula::Sim3 shift;
    for (int i = 0; i < 3; i++) shift.t[i] = _ds.center[(size_t)i];
    const spirula::Sim3 moved = file_placement() * shift;
    spirula::write_ply_points(
        path, _ds.points.xyz.data(),
        _ds.points.rgb.empty() ? nullptr : _ds.points.rgb.data(),
        _ds.points.num(), alive_of(kPoints).data(), &moved);
    if (progress) (*progress)++;
}

}  // namespace gui
