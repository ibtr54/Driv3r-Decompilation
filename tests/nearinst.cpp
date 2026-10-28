// nearinst <Miami_Day_PC.d3c> x z r: the city instances within r of (x, z):
// index, model (package uid, index), position, GEBI range, and whether the
// model has a PMLD collision record in its package's region (the program's
// collision world builds objects only for those, collision.cpp).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "../city.h"

int main(int argc, char** argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: nearinst city.d3c x z r\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CityPlacement city;
    if (!city.Load(d)) {
        fprintf(stderr, "city: %s\n", city.Error().c_str());
        return 2;
    }
    const float x = float(atof(argv[2])), z = float(atof(argv[3])), r = float(atof(argv[4]));
    for (size_t i = 0; i < city.instances.size(); ++i) {
        const CityInstance& c = city.instances[i];
        const float dx = c.position[0] - x, dz = c.position[2] - z;
        if (dx * dx + dz * dz > r * r) continue;
        const uint32_t id = c.model;
        int pmld = -1;
        for (const CityCollisionRegion& reg : city.collision) {
            if (reg.region != int(c.PackageUid())) continue;
            for (const auto& m : reg.models)
                if (m.id == id) pmld = int(m.count);
        }
        printf("%6zu model %2u:%4u extra %04x at (%.2f %.2f %.2f) range %d cell %d/%d%s\n", i, c.PackageUid(),
               c.ModelIndex(), c.extra, c.position[0], c.position[1], c.position[2], c.range, c.column, c.row,
               !c.HasModel() ? " (no model)" : pmld < 0 ? " NO COLLISION RECORD" : "");
        if (pmld >= 0) printf("        collision entries %d\n", pmld);
        // a record for the same id in any other region
        for (const CityCollisionRegion& reg : city.collision)
            for (const auto& m : reg.models)
                if (m.id == id && reg.region != int(c.PackageUid()))
                    printf("        also in region %d: first %u count %u radius %.2f\n", reg.region, m.first, m.count,
                           m.radius);
    }
    return 0;
}
