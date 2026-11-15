#pragma once

// A synthetic upper hemisphere standing in for the sky, added to a sparse
// reconstruction as seed points. A clear sky has no keypoints to triangulate,
// so training starts with nothing above the horizon; this puts a shell of
// points there instead.

#include "data/DatasetParser.h"

#include <array>
#include <cstdint>

struct SkyDomeConfig {
    // Icosphere subdivisions. The whole sphere has 10*4^n+2 vertices, of which
    // a shade over half are upper: 5185 at 5, 20609 at 6. The icosahedron it
    // starts from has four vertices on the equator and is not symmetric.
    int subdivisions = 5;
    // Radius as a percentile of the keypoint distances from the centre. The
    // literal maximum is usually a floater, so this is not 100.
    double percentile = 99.0;
    // The sky the Metashape script falls back to when a point is unseen.
    uint8_t rgb[3] = {135, 180, 220};
};

// What was measured, so a panel can show it before committing: the centre the
// dome was placed at, the radius it got, and whether the percentile or the
// literal maximum bounded it.
struct SkyDomeFit {
    std::array<double, 3> center{};
    double radius = 0.0;
    double percentile_radius = 0.0;   // 0 when the maximum bounded it
    double max_radius = 0.0;          // 0 when the percentile bounded it
    int64_t points = 0;
};

// The upper half of an icosphere of `radius` about `center` in `up`, in the
    // frame of `points`; z >= 0 relative to the centre, so the base is a disc
    // at its height. Throws with too few cameras, or too few live points.
ColmapPoints3D sky_dome_points(const SkyDomeConfig& cfg,
                               const std::vector<double>& cam_centres,
                               const std::vector<uint8_t>* alive,
                               const double* points, int64_t n,
                               const double up[3], SkyDomeFit* fit);