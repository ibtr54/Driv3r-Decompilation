// shapecount <Miami_Day_PC.d3c> [x z r]: the static collision world's shapes
// by type (0 box, 1 cylinder, 2 sphere), and the surfaces of the cylinders;
// with a point, only the shapes within r of (x, z).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#include "../city.h"
#include "../collision.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: shapecount city.d3c [x z r]\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CityPlacement city;
    if (!city.Load(d)) return 1;
    CollisionWorld world;
    world.Build(city);
    const bool near = argc >= 5;
    const float x = near ? float(atof(argv[2])) : 0, z = near ? float(atof(argv[3])) : 0, r = near ? float(atof(argv[4])) : 0;
    std::map<int, int> types;
    std::map<int, int> cylSurf;
    int cylinders = 0;
    for (size_t i = 0; i < world.Shapes(); ++i) {
        const CollisionWorld::Shape& s = world.ShapeAt(i);
        if (near) {
            const float dx = s.world[12] - x, dz = s.world[14] - z;
            if (dx * dx + dz * dz > r * r) continue;
        }
        ++types[s.type];
        if (s.type == 1) {
            ++cylSurf[s.surface];
            if (near && cylinders++ < 20)
                printf("cylinder at (%.2f %.2f %.2f) radius %.3f half height %.3f surface %d\n", s.world[12],
                       s.world[13], s.world[14], s.size[0], s.size[1], s.surface);
        }
    }
    for (const auto& t : types) printf("type %d: %d shapes\n", t.first, t.second);
    for (const auto& c : cylSurf) printf("  cylinders of surface %d: %d\n", c.first, c.second);
    return 0;
}
