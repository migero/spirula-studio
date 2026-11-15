// sky_dome_real_test -- the sky dome over a REAL reconstruction named on the
// command line, so the numbers it prints are the ones the user will see. Takes
// a dataset folder; with no argument it says so and passes.
//
// Prints the centre it measured from, the radius at p99 and at the literal
// maximum, and how far the added points sit from being exactly on the sphere.

// Real-data check: the sky dome over the mon reconstruction.
#include "app/gui/edit/PointsDoc.h"
#include "data/DatasetParser.h"
#include "data/SceneCenter.h"
#include <cmath>
#include <cstdio>
#include <string>
using gui::PointsDoc;
int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: sky_dome_real_test <dataset-dir>\n");
        return 0;
    }
    const std::string dir = argv[1];
    DatasetParserConfig pc;
    ParsedDataset ds = parse_dataset(dir, pc, "colmap");
    std::printf("mon: %d cameras, %lld points, centre %.3f %.3f %.3f\n",
        (int)ds.num_cameras, (long long)ds.points.num(),
        ds.center[0], ds.center[1], ds.center[2]);
    const int64_t base = ds.points.num();
    PostSplitCameras post = bake_post_split(ds, false, false);
    PointsDoc doc(std::move(ds), post, dir + "/sparse/0/points3D.bin", dir, nullptr);

    for (double pct : {99.0, 100.0}) {
        std::string err;
        auto f = doc.measure_sky_dome(5, pct, &err);
        std::printf("  p%.0f: radius %.4f  points %lld  %s\n", pct, f.radius,
                    (long long)f.points, err.c_str());
    }
    std::string err;
    auto fit = doc.add_sky_dome(5, 99.0, &err);
    std::printf("added: radius %.4f, %lld points (err '%s')\n", fit.radius,
                (long long)fit.points, err.c_str());
    std::printf("layer %lld (was %lld), dataset %lld\n",
        (long long)doc.count(), (long long)base,
        (long long)doc.dataset()->points.num());

    // Check the geometry in the layer frame.
    double A[16]; dsparse::train_to_normalized_inverse(*doc.dataset(), A);
    auto to_view=[&](const double*p,double*o){for(int r=0;r<3;r++)
        o[r]=A[r*4+0]*p[0]+A[r*4+1]*p[1]+A[r*4+2]*p[2]+A[r*4+3];};
    std::vector<double> cams;
    for(int64_t i=0;i<doc.alive_count_of(1);i++){
        const float*c=doc.positions_of(1)+i*3; double q[3]={c[0],c[1],c[2]},o[3];
        // camera layer is already in the view frame
        for(int r=0;r<3;r++)o[r]=q[r];
        cams.insert(cams.end(),o,o+3);
    }
    auto C=dsparse::geometric_median(cams.data(),(int64_t)(cams.size()/3),3,0);
    const float* pos=doc.positions_of(0);
    double worst=0,minz=1e30; int64_t below=0;
    for(int64_t i=base;i<doc.count();i++){
        double dx=pos[i*3]-C[0],dy=pos[i*3+1]-C[1],dz=pos[i*3+2]-C[2];
        worst=std::max(worst,std::fabs(std::sqrt(dx*dx+dy*dy+dz*dz)-fit.radius));
        minz=std::min(minz,dz); if(dz<-1e-6)below++;
    }
    std::printf("centre %.4f %.4f %.4f\n",C[0],C[1],C[2]);
    std::printf("sphere error %.3e   below base %lld   min dz %.4f\n",
        worst,(long long)below,minz);
    // Camera heights, for reference: a dome base at the cameras' height.
    std::printf("camera z range: %.4f .. %.4f\n",
        doc.positions_of(1)[2],
        [&]{float m=-1e30f;const float*p=doc.positions_of(1);
             for(int64_t i=0;i<doc.alive_count_of(1);i++)m=std::max(m,p[i*3+2]);return m;}());
    std::printf("view_frame scale %.6g\n", doc.view_frame().s);

    // Read the dome's FILE coordinates straight out of the dataset: that is
    // exactly what gets written to points3D.bin, so this checks the save, not
    // a re-derivation of the matrix that produced it.
    const std::vector<double>& px = doc.dataset()->points.xyz;
    std::vector<double> z3;
    double zlo = 1e300, zhi = -1e300;
    for (int64_t i = base; i < doc.count(); i++) {
        const double f[3] = {px[(size_t)i*3], px[(size_t)i*3+1], px[(size_t)i*3+2]};
        z3.push_back(f[2]);
        zlo = std::min(zlo, f[2]);
        zhi = std::max(zhi, f[2]);
    }
    std::vector<double> base_ring;
    for (int64_t i = base; i < doc.count(); i++) {
        const double f[3] = {px[(size_t)i*3], px[(size_t)i*3+1], px[(size_t)i*3+2]};
        if (f[2] <= zlo + 0.01 * (zhi - zlo)) {
            base_ring.push_back(f[0]);
            base_ring.push_back(f[1]);
            base_ring.push_back(f[2]);
        }
    }
    const size_t nb = base_ring.size() / 3;
    const double radius = fit.radius / doc.view_frame().s;   // view -> file units
    double m[3] = {0, 0, 0};
    for (size_t i = 0; i < nb; i++)
        for (int r = 0; r < 3; r++) m[r] += base_ring[i*3+r] / (double)nb;
    // A level dome's file-frame z-extent equals its radius exactly, and its
    // base is a circle of that same radius. Tilt shows up in both at once.
    const double height = zhi - zlo;
    double rmean = 0;
    for (size_t i = 0; i < nb; i++)
        rmean += std::hypot(base_ring[i*3] - m[0], base_ring[i*3+1] - m[1]) / (double)nb;
    // Least squares z = a*x + b*y + c over the ring: the tilt is atan|grad|.
    double Sxx = 0, Sxy = 0, Syy = 0, Sxz = 0, Syz = 0, n = 0;
    for (size_t i = 0; i < nb; i++) {
        const double dx = base_ring[i*3] - m[0], dy = base_ring[i*3+1] - m[1];
        const double dz = base_ring[i*3+2] - m[2];
        Sxx += dx*dx; Sxy += dx*dy; Syy += dy*dy; Sxz += dx*dz; Syz += dy*dz; n += 1;
    }
    const double det = Sxx*Syy - Sxy*Sxy;
    const double ga = det > 0 ? (Syy*Sxz - Sxy*Syz) / det : 0.0;
    const double gb = det > 0 ? (Sxx*Syz - Sxy*Sxz) / det : 0.0;
    const double tilt = std::atan(std::sqrt(ga*ga + gb*gb)) * 180.0 / 3.14159265358979323846;
    std::printf("dome radius %.4f | z-extent %.4f (%.4f x radius) | base ring r %.4f (%.4f x), %zu pts\n",
                radius, height, height / radius, rmean, rmean / radius, nb);
    std::printf("angle between base plane and world +Z: %.4f deg  %s\n", tilt,
                tilt < 5.0 ? "OK (level with the ground)" : "TILTED");
    return 0;
}
