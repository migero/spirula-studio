#pragma once

// The GoPro-native per-view bin masker: a .people.bin frame's outline and
// stored shadow-oval rays are directions in the video's own frame, this
// rasters them into the view's pixel mask (0 = remove). The raster follows
// the view pixel mapping Pano360 owns, densifying each outline edge into
// great-circle steps so it lands faithfully across wide views and at the
// lens seam.

#include "app/GoProBin.h"
#include "app/Pano360.h"

#include <string>
#include <vector>

namespace app {

// tracking_target_rays from TrackingCropBridge.cpp, copied so both this
// raster's tests and its seam checks have the same view-pixel rays without a
// SAM build. Same EAC mapping, byte for byte.
std::vector<float> gopro_target_rays(const Pano360Layout& l, Pano360View view,
                                     int w, int h);

// Rasterizes one World-Lock closed loop into `mask` (w x h; the caller owns
// the base value, usually 255 to keep) by setting `value` inside it.
bool gopro_mask_polygon(uint8_t* mask, int w, int h, const Pano360View& view,
                        const std::vector<GoProVec3>& world_rays,
                        const double (&world_rot)[9], uint8_t value,
                        std::string& error);

// One view's whole frame: every person's outline, plus their stored shadow
// oval (version06 bins). The frame's rays are the player's camera-frame ones
// (y up); the raster mirrors y.
bool gopro_mask_frame(const Pano360View& view, int w, int h,
                      const GoProBinFrame& frame, uint8_t* mask,
                      std::string& error);

}  // namespace app
