// EditMaskSelect.cpp -- "Select by mask": what a text prompt picks.
//
// A prompt names a thing ("sky; cloud"). A sample of the dataset's cameras,
// evenly spaced along the capture, is masked by Grounding DINO + SAM 2, and
// every point is projected into each masked frame -- the InViewCount pattern
// (Attributes.cpp) -- to vote with the mask pixel it lands on. A point is
// selected when enough of the frames it projects into agree; occlusion needs
// no special case, because a hidden point lands on whatever is in front.

#include "app/gui/edit/EditSession.h"

#include "app/FrameLook.h"
#include "app/gui/Layout.h"
#include "app/gui/ModelCache.h"
#include "app/gui/PreviewTexture.h"
#include "app/gui/Ui.h"
#include "data/CameraMath.h"
#include "i18n/catalog/Edit.h"
#include "sam/Masking.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace msg = spirula::i18n::msg::edit;

namespace gui {

namespace {

// Longest side a frame is decoded to before masking; the mask keeps this
// size, so a 5.3K frame does not cost a 5.3K model pass or a 5.3K mask.
constexpr int kMaskCap = 1600;

// Prompted objects come out white; the mask is read per camera, never
// tracked, so each frame is segmented on its own.
void fill_options(sam::MaskOptions& o, const std::string& text,
                  const MaskModelFiles& files, const std::string& device) {
    o.model = files.model;
    o.detector = files.detector;
    o.device = device;
    o.text = text;
    o.video = false;
    o.keep_prompted = true;
    o.max_size = kMaskCap;
    o.detect_every = 1;
}

nn::Image decode_capped(const std::string& file, sfm::ExifTransform& turn) {
    nn::Image img = app::load_upright(file, "", std::nullopt,
                                      colorspace::Exposure{}, turn);
    const int longer = std::max(img.width, img.height);
    if (!img.empty() && longer > kMaskCap)
        img = nn::resize_image(img, img.width * kMaskCap / longer,
                               img.height * kMaskCap / longer);
    return img;
}

// The frame with the mask shaded over it, small enough to upload per camera.
constexpr int kThumbCap = 480;

void shade(nn::Image& img, const sam::Mask& mask) {
    for (size_t k = 0; k < img.data.size(); k += 3) {
        if (!mask.data[k / 3]) continue;
        img.data[k] = (uint8_t)((img.data[k] + 255) / 2);
        img.data[k + 1] = (uint8_t)((img.data[k + 1] + 80) / 2);
        img.data[k + 2] = (uint8_t)((img.data[k + 2] + 80) / 2);
    }
    const int longer = std::max(img.width, img.height);
    if (longer > kThumbCap)
        img = nn::resize_image(img, img.width * kThumbCap / longer,
                               img.height * kThumbCap / longer);
}

}  // namespace

void EditSession::draw_mask_section(EditDoc& d, float full) {
    auto* pts = dynamic_cast<PointsDoc*>(&d);
    if (!pts) return;
    const float slider_w = std::max(full * 0.45f, px(120.0f));

    if (!ui::CollapsingHeader(msg::sec_mask)) return;
    ui::TextWrapped(msg::mask_hint);

    // Loose PLYs and non-layer-0 views have no cameras to sample.
    const ParsedDataset* ds = pts->dataset();
    const bool usable = ds && ds->num_cameras > 0 && !ds->points.xyz.empty() &&
                        !ds->image_filenames.empty();
    if (!usable) {
        ui::TextDisabled(msg::mask_no_dataset);
        return;
    }

    ImGui::PushID("masksel");
    if (_mask_model_ui) _mask_model_ui(_mask_model_id, _mask_detector_id);
    const MaskModelFiles files = _mask_model_files
        ? _mask_model_files(_mask_model_id, _mask_detector_id)
        : MaskModelFiles{};
    if (files.empty()) ui::TextDisabled(msg::mask_model_missing);
    ImGui::PopID();

    if (ui::InputTextEnglish(msg::mask_prompt, "sky; cloud", &_mask_prompt))
        _mask_status.clear();
    ui::help_on_hover(msg::mask_prompt_help);
    ImGui::SetNextItemWidth(slider_w);
    ui::SliderInt(msg::mask_cameras, &_mask_cameras, 1, 64);
    ui::help_on_hover(msg::mask_cameras_help);
    ImGui::SetNextItemWidth(slider_w);
    ui::SliderFloat(msg::mask_votes, &_mask_min_frac, 0.0f, 1.0f, "%.2f");
    ui::help_on_hover(msg::mask_votes_help);

    const bool busy_here = _mask_busy.load();
    const bool go = _mask_model_ui && !files.empty() && !_mask_prompt.empty();
    ImGui::BeginDisabled(busy_here || !go);
    if (ui::Button(msg::mask_run, ImVec2(full, 0))) start_mask_select();
    ImGui::EndDisabled();
    ui::help_on_hover(msg::mask_run_help);

    if (busy_here) {
        ui::Text(msg::mask_progress,
                 {(long long)_mask_done.load(), (long long)_mask_total.load()});
        if (ui::Button(msg::mask_cancel)) cancel_work();
    } else if (!_mask_status.empty()) {
        const ImGuiStyle& st = ImGui::GetStyle();
        ui::TextColoredWrappedRaw(_mask_status_err ? ImVec4(1, 0.5f, 0.5f, 1)
                                                   : st.Colors[ImGuiCol_Text],
                                  _mask_status);
    }
    drain_mask_thumbs();
    draw_mask_preview();
}

void EditSession::drain_mask_thumbs() {
    std::vector<MaskThumb> taken;
    {
        std::lock_guard<std::mutex> lk(_mask_thumb_mutex);
        if (_mask_thumbs.empty()) return;
        taken = std::move(_mask_thumbs);
        _mask_thumbs.clear();
    }
    for (auto& t : taken) {
        auto it = std::find_if(_mask_view.begin(), _mask_view.end(),
                               [&](const MaskThumb& v) {
                                   return v.camera == t.camera;
                               });
        if (it != _mask_view.end()) *it = std::move(t);
        else _mask_view.push_back(std::move(t));
    }
}

// A floating window of the frames the job has masked so far, shaded where
// the prompt matched -- separate from the viewport, so nothing obscures the
// 3D view. Thumbnails upload once per run; the frame copies them.
void EditSession::draw_mask_preview() {
    struct ThumbTex {
        PreviewTexture tex;
        int camera = -1, run = -1;
        ~ThumbTex() { tex.reset(); }
    };
    static std::vector<ThumbTex> texs;

    if (_mask_view.empty()) {
        texs.clear();
        return;
    }
    if (!_mask_show_preview) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.5f, vp->WorkSize.y * 0.5f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ui::detail::label(msg::mask_preview_title),
                      &_mask_show_preview)) {
        ImGui::End();
        return;
    }
    ui::TextWrapped(msg::mask_preview_legend);

    const float cell = px(260.0f);
    const float pad = ImGui::GetStyle().ItemSpacing.x;
    float row = 0.0f;
    for (size_t i = 0; i < _mask_view.size(); i++) {
        const MaskThumb& t = _mask_view[i];
        if (i) {
            if (row + pad + cell <= ImGui::GetContentRegionAvail().x)
                ImGui::SameLine();
            else
                row = 0.0f;
        }
        ThumbTex* tt = nullptr;
        for (auto& e : texs)
            if (e.camera == t.camera) { tt = &e; break; }
        if (!tt) {
            texs.push_back(ThumbTex{});
            tt = &texs.back();
            tt->camera = t.camera;
            tt->run = _mask_run_id;
            tt->tex.upload(t.rgb.data(), t.w, t.h);
        } else if (tt->run != _mask_run_id || !tt->tex.ready()) {
            tt->run = _mask_run_id;
            tt->tex.upload(t.rgb.data(), t.w, t.h);
        }
        if (!tt->tex.ready()) continue;
        const float s = cell / (float)t.w;
        ImGui::Image(tt->tex.id(), ImVec2(cell, (float)t.h * s));
        ui::TextDisabledRaw(("#" + std::to_string(t.camera + 1)).c_str());
        row += cell + pad;
    }
    ImGui::End();
}

void EditSession::start_mask_select() {
    auto* pts = dynamic_cast<PointsDoc*>(_doc.get());
    if (!pts || busy()) return;
    if (_mask_worker.joinable()) take_mask_result();
    const ParsedDataset* ds = pts->dataset();
    if (!ds || ds->num_cameras < 1 || ds->points.xyz.empty() ||
        (int64_t)ds->image_filenames.size() != ds->num_cameras) return;
    if (pts->layer() != 0) return;
    if (!_mask_model_files || !_mask_device_gate) return;

    const MaskModelFiles files =
        _mask_model_files(_mask_model_id, _mask_detector_id);
    if (files.empty()) {
        _mask_status = msg::mask_model_missing.get();
        _mask_status_err = true;
        return;
    }
    std::string device, error;
    if (!_mask_device_gate(device, error)) {
        if (error.empty()) error = spirula::i18n::format(
            msg::mask_failed, {std::string("device")});
        _mask_status = error;
        _mask_status_err = true;
        return;
    }

    std::string prompt = _mask_prompt;
    // An empty phrase list (blank entries dropped) finds boxes of nothing.
    if (sam::split_phrases(prompt).empty()) return;

    const int64_t nc = ds->num_cameras;
    const int pick = std::clamp(_mask_cameras, 1, (int)std::min<int64_t>(nc, 64));
    const float min_frac = std::clamp(_mask_min_frac, 0.0f, 1.0f);
    const int64_t n = (int64_t)ds->points.xyz.size() / 3;

    _cancel = false;
    _mask_status.clear();
    _mask_status_err = false;
    _mask_error.clear();
    _mask_result.clear();
    {
        std::lock_guard<std::mutex> lk(_mask_thumb_mutex);
        _mask_thumbs.clear();
    }
    _mask_view.clear();
    _mask_run_id++;
    _mask_show_preview = true;
    _mask_done = 0;
    _mask_total = pick;
    _mask_busy = true;
    _mask_worker = std::thread([this, ds, nc, pick, prompt, min_frac, n,
                                files, device]() mutable {
        try {
            sam::MaskOptions o;
            fill_options(o, prompt, files, device);
            sam::Masker masker;
            std::string init_err;
            if (!masker.init(o, init_err)) {
                _mask_error = init_err;
                _mask_busy = false;
                return;
            }
            // The cameras are spread evenly over the whole capture, so a
            // prompt answers for the scene and not for the first frames.
            std::vector<int32_t> cams((size_t)pick);
            for (int i = 0; i < pick; i++)
                cams[(size_t)i] = (int32_t)((2 * (int64_t)i + 1) * nc / (2 * pick));
            std::vector<int32_t> votes((size_t)n, 0), seen((size_t)n, 0);
            for (int i = 0; i < pick; i++) {
                if (_cancel.load(std::memory_order_relaxed)) break;
                const int32_t c = cams[(size_t)i];
                sfm::ExifTransform turn;
                nn::Image img = decode_capped(ds->image_filenames[(size_t)c], turn);
                if (img.empty()) {
                    _mask_done++;
                    continue;
                }
                sam::Mask mask;
                if (!masker.run(img, mask, nullptr, (int64_t)i,
                                nullptr, ds->image_filenames[(size_t)c])) {
                    _mask_error = masker.lastError();
                    break;
                }
                // Shaded with the UPRIGHT mask before the turn back; the
                // sampling below still gets the file-frame mask, and the
                // move preserves img's width/height for it.
                {
                    nn::Image ov = std::move(img);
                    shade(ov, mask);
                    EditSession::MaskThumb t;
                    t.camera = c;
                    t.w = ov.width;
                    t.h = ov.height;
                    t.rgb = std::move(ov.data);
                    std::lock_guard<std::mutex> lk(_mask_thumb_mutex);
                    _mask_thumbs.push_back(std::move(t));
                }
                // The mask comes back upright; the camera's frame is the
                // file's, so the EXIF turn goes back before sampling.
                app::turn_pixels(app::inverse_turn(turn), 1, mask.data,
                                 mask.width, mask.height);
                if (std::find(mask.data.begin(), mask.data.end(), 255) ==
                    mask.data.end()) {
                    _mask_done++;
                    continue;
                }
                const int mw = mask.width, mh = mask.height;
                const uint8_t* md = mask.data.data();
                const int64_t iw = img.width, ih = img.height;
                const int64_t cw = ds->widths[(size_t)c];
                const int64_t ch = ds->heights[(size_t)c];
                camhost::Camera cam;
                cam.model = ds->camera_models[(size_t)c];
                cam.width = (int)cw;
                cam.height = (int)ch;
                cam.fx = ds->intrins[(size_t)c*4];
                cam.fy = ds->intrins[(size_t)c*4+1];
                cam.cx = ds->intrins[(size_t)c*4+2];
                cam.cy = ds->intrins[(size_t)c*4+3];
                if ((int64_t)ds->dist_coeffs.size() >= nc * 8)
                    for (int k = 0; k < 8; k++)
                        cam.dist[k] = ds->dist_coeffs[(size_t)c * 8 + (size_t)k];
#pragma omp parallel for schedule(dynamic, 256)
                for (int64_t p = 0; p < n; p++) {
                    const float* m = &ds->c2w[(size_t)c * 12];
                    const double* pt = &ds->points.xyz[(size_t)p * 3];
                    const double d[3] = {pt[0] - m[3], pt[1] - m[7], pt[2] - m[11]};
                    // OpenGL camera-to-world: the camera looks down its own
                    // -z, and the lens maths wants +z forward, +y down.
                    const double ray[3] = {
                        m[0]*d[0] + m[4]*d[1] + m[8]*d[2],
                        -(m[1]*d[0] + m[5]*d[1] + m[9]*d[2]),
                        -(m[2]*d[0] + m[6]*d[1] + m[10]*d[2])};
                    double pxv[2];
                    if (!camhost::ray_in_frame(cam, ray, pxv))
                        continue;
                    int sx = (int)(pxv[0] * (float)iw / (float)cw);
                    int sy = (int)(pxv[1] * (float)ih / (float)ch);
                    sx = std::clamp(sx, 0, mw - 1);
                    sy = std::clamp(sy, 0, mh - 1);
                    seen[(size_t)p]++;
                    if (md[(size_t)sy * mw + sx]) votes[(size_t)p]++;
                }
                _mask_done++;
            }
            std::vector<uint8_t> w((size_t)n, 0);
            for (int64_t p = 0; p < n; p++) {
                const int64_t s = seen[(size_t)p];
                // At least one frame in agreement: min_frac 0 would otherwise
                // take every point the cameras can see at all.
                if (s > 0 && votes[(size_t)p] > 0 &&
                    (float)votes[(size_t)p] >= min_frac * (float)s)
                    w[(size_t)p] = 255;
            }
            _mask_result = std::move(w);
        } catch (const std::exception& e) {
            _mask_error = e.what();
        }
        _mask_busy = false;
    });
}

void EditSession::take_mask_result() {
    _mask_worker.join();
    _cancel = false;
    if (!_mask_error.empty()) {
        _mask_status = spirula::i18n::format(msg::mask_failed, {_mask_error});
        _mask_status_err = true;
        return;
    }
    if (_mask_result.empty()) {
        _mask_status = msg::mask_cancelled.get();
        _mask_status_err = false;
        return;
    }
    if (_doc && _doc->count() == (int64_t)_mask_result.size()) {
        const ImGuiIO& io = ImGui::GetIO();
        Selection next;
        next.assign(_doc->sel().weights());
        next.combine(_mask_result.data(), combine_now(io.KeyShift, io.KeyCtrl),
                     _doc->alive());
        const int64_t picked = std::count(_mask_result.begin(),
                                          _mask_result.end(), 255);
        run_select(next.weights(),
                   spirula::i18n::format(msg::op_select_mask, {_mask_prompt}));
        _mask_status = spirula::i18n::format(msg::mask_done, {(long long)picked});
        _mask_status_err = false;
    } else {
        _mask_status = msg::mask_cancelled.get();
        _mask_status_err = false;
    }
    _mask_result.clear();
}

}  // namespace gui
