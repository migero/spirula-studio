#pragma once
#include "app/Pano360.h"
#include "app/gui/Subprocess.h"
#include "nn/io/Image.h"
#include <atomic>
#include <thread>
#include <functional>

namespace app {
struct TrackingCrop {
    nn::Image image;
    double tangent = 1;
    double rotation[9]{}; // view to camera; x right, y down, z forward
    std::vector<uint8_t> mask; // 255 keeps, 0 removes
    bool segmented = false; // mask done; the pixels are freed after, and the
                            // frame's other views reuse this mask
};
struct TrackingFrame {
    int64_t index = -1;
    double time = 0;
    bool fallback = false;
    std::vector<TrackingCrop> crops;
};
class TrackingCropBridge {
public:
    ~TrackingCropBridge();
    bool start(const std::string& renderer, const std::string& cache_folder,
               const std::string& video, const std::vector<int64_t>& frames,
               const std::atomic<bool>& cancel, std::function<void(const std::string&)> log,
               std::string& error);
    bool read(int64_t index, TrackingFrame& frame, std::string& error);
    bool finish(std::string& error);
    static std::string renderer_path(const std::string& configured);
private:
    gui::ProcessReader pipe_;
    std::thread watcher_;
    std::atomic<bool> stopped_{false};
    std::string options_;
    bool exact(void* dst, size_t bytes);
    bool line(std::string& out);
};
std::vector<float> tracking_target_rays(const Pano360Layout& layout, Pano360View view, int w, int h);
void project_tracking_masks(const std::vector<float>& rays, const TrackingFrame& frame,
                            std::vector<uint8_t>& mask);
}
