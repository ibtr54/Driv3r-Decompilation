// Console check of the city placement decoder (city.cpp) against the
// model packages (mdpc.cpp):
//   - every instance's model resolves (package uid present in
//     SuperRegions.pcs, index below its model count),
//   - every rotation has unit length,
//   - every GEIR instance lies inside its cell (column / row, 480 units
//     centred on 0; x grows with the column, z falls with the row): by its
//     position (a few stand up to 27 units outside in Miami; more than 32
//     fails).
// Models whose box is empty (+FLT_MAX / -FLT_MAX corners, only slot 5
// geometry) are left out of the box statistics.
// Build: tests\build_citytest.bat. Usage: citytest <city.d3c> <SuperRegions.pcs>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "../city.h"
#include "../mdpc.h"

static std::vector<uint8_t> Read(const char* path)
{
    std::vector<uint8_t> d;
    FILE* f = fopen(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d.resize(size_t(n));
    if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: citytest <city.d3c> <SuperRegions.pcs>\n"); return 2; }
    std::vector<uint8_t> d3c = Read(argv[1]), pcs = Read(argv[2]);
    CityPlacement city;
    if (!city.Load(d3c)) { printf("FAIL load: %s\n", city.Error().c_str()); return 1; }

    std::map<uint32_t, MdpcPackage> packages;
    for (auto& c : FindMdpcChunks(pcs)) {
        MdpcPackage p;
        if (!p.Load(pcs, c.first, c.second)) { printf("FAIL package at 0x%zx: %s\n", c.first, p.Error().c_str()); return 1; }
        packages[p.uid] = std::move(p);
    }

    size_t noModel = 0, unresolved = 0, notUnit = 0, outsideCell = 0, cellInstances = 0, yaw = 0, emptyBox = 0;
    float farthest = 0;
    size_t ranges[6] = {};
    std::map<uint32_t, size_t> perPackage;
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (const CityInstance& in : city.instances) {
        ++ranges[in.range];
        const float* q = in.rotation;
        float norm = q[3] > 100.0f ? q[0] * q[0] + q[1] * q[1] : q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (q[3] > 100.0f) ++yaw;
        if (std::fabs(norm - 1.0f) > 1e-3f) ++notUnit;
        if (in.column >= 0) {
            ++cellInstances;
            float half = 240.0f * float(city.gridSide);
            float x0 = -half + 480.0f * float(in.column), z1 = half - 480.0f * float(in.row);
            const float* p = in.position;
            float out = std::fmax(std::fmax(x0 - p[0], p[0] - x0 - 480.0f), std::fmax(p[2] - z1, z1 - 480.0f - p[2]));
            if (out > 0.01f) ++outsideCell;
            if (out > farthest) farthest = out;
        }
        if (!in.HasModel()) { ++noModel; continue; }
        auto it = packages.find(in.PackageUid());
        if (it == packages.end() || in.ModelIndex() >= it->second.models.size()) {
            ++unresolved;
            continue;
        }
        ++perPackage[in.PackageUid()];
        // The model's box centre in the world.
        const MdpcModel& m = it->second.models[in.ModelIndex()];
        if (m.box[0][0] > 1e30f || m.box[0][0] < -1e30f) { ++emptyBox; continue; }
        float c[3] = {}, w[3];
        for (int k = 0; k < 8; ++k)
            for (int j = 0; j < 3; ++j) c[j] += m.box[k][j] / 8.0f;
        in.Transform(c, w);
        for (int j = 0; j < 3; ++j) {
            if (w[j] < lo[j]) lo[j] = w[j];
            if (w[j] > hi[j]) hi[j] = w[j];
        }
    }
    printf("grid %d x %d; ", city.gridSide, city.gridSide);
    printf("%zu instances (%zu in GEIR cells), %zu yaw form, %zu quaternion\n", city.instances.size(), cellInstances, yaw,
           city.instances.size() - yaw);
    printf("ranges: %zu %zu %zu %zu %zu %zu\n", ranges[0], ranges[1], ranges[2], ranges[3], ranges[4], ranges[5]);
    printf("packages:");
    for (auto& p : perPackage) printf(" %u:%zu", p.first, p.second);
    printf("\nno model (index 0x3ff): %zu, empty model box: %zu\n", noModel, emptyBox);
    printf("box centres: x %.0f..%.0f, y %.0f..%.0f, z %.0f..%.0f\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
    printf("outside their cell: %zu (at most %.2f units)\n", outsideCell, farthest);
    printf("unresolved models: %zu, rotations not unit: %zu\n", unresolved, notUnit);
    bool ok = unresolved == 0 && notUnit == 0 && farthest < 32.0f && !city.instances.empty();
    printf(ok ? "OK\n" : "FAIL\n");
    return ok ? 0 : 1;
}
