// colltest <Miami_Day_PC.d3c> <phys.txt>: builds the static collision world
// (collision.h) and casts the wheel rays of the traced car dynamics calls
// ("Xr" records: the ray batch 0x8b3458 and the answers 0x8b3668) against
// it, comparing hit, distance, surface and normal with the game's.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../collision.h"

static float Fl(unsigned u) { float f; memcpy(&f, &u, 4); return f; }

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: colltest city.d3c phys.txt\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CityPlacement city;
    if (!city.Load(d)) { fprintf(stderr, "city: %s\n", city.Error().c_str()); return 2; }
    CollisionWorld world;
    world.Build(city);
    size_t models = 0;
    for (const CityCollisionRegion& r : city.collision) models += r.models.size();
    printf("%zu instances, %zu collision regions (%zu model records), %zu objects, %zu shapes\n", city.instances.size(),
           city.collision.size(), models, world.Objects(), world.Shapes());

    FILE* f = fopen(argv[2], "r");
    if (!f) { perror(argv[2]); return 2; }
    static char line[1 << 17];
    int rays = 0, hitsMatch = 0, close = 0, surfaceMatch = 0, missBoth = 0, shown = 0;
    double worst = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Xr ", 3) != 0) continue;
        char* p = line + 3;
        const unsigned n = strtoul(p, &p, 10);
        strtoul(p, &p, 16);
        std::vector<unsigned> w;
        while (*p && *p != '\n') { w.push_back(static_cast<unsigned>(strtoul(p, &p, 16))); while (*p == ' ') ++p; }
        if (w.size() < 0x428 / 4) continue;
        for (int i = 0; i < 12; ++i) {
            const unsigned* r = &w[i * 0x2c / 4];
            const unsigned* h = &w[(0x210 + i * 0x30) / 4];
            if (r[0] == 0 || Fl(r[9]) <= 0) continue;   // no body: an unused slot
            const float start[3] = { Fl(r[1]), Fl(r[2]), Fl(r[3]) }, dir[3] = { Fl(r[5]), Fl(r[6]), Fl(r[7]) };
            const float len = Fl(r[9]);
            if (!(std::fabs(dir[0]) <= 1) || !(len < 100)) continue;
            ++rays;
            CollisionWorld::Hit hit;
            const bool mine = world.Cast(&city.ground, start, dir, len, hit);
            const bool game = h[0] == 2;
            if (!mine && !game) { ++missBoth; continue; }
            if (mine == game) {
                ++hitsMatch;
                const double dd = std::fabs(hit.distance - Fl(h[1]));
                worst = std::max(worst, dd);
                if (dd < 1e-3) ++close;
                if (hit.surface == int(h[3])) ++surfaceMatch;
            }
            if ((mine != game || std::fabs(hit.distance - Fl(h[1])) > 1e-3 || hit.surface != int(h[3])) && shown++ < 12)
                printf("call %u ray %d at (%.2f %.2f %.2f): mine %s d %.5f s %d n (%.3f %.3f %.3f) / game %s d %.5f s %d "
                       "n (%.3f %.3f %.3f)\n", n, i, start[0], start[1], start[2], mine ? "hit" : "miss", hit.distance,
                       hit.surface, hit.normal[0], hit.normal[1], hit.normal[2], game ? "hit" : "miss", Fl(h[1]),
                       int(h[3]), Fl(h[4]), Fl(h[5]), Fl(h[6]));
            if ((mine != game || std::fabs(hit.distance - Fl(h[1])) > 1e-3) && hit.instance >= 0 && shown <= 12) {
                const CityInstance& ci = city.instances[size_t(hit.instance)];
                printf("    instance %d: model 0x%04x extra 0x%04x range %d cell %d,%d at (%.2f %.2f %.2f)\n", hit.instance,
                       ci.model, ci.extra, ci.range, ci.column, ci.row, ci.position[0], ci.position[1], ci.position[2]);
                static int dumped = -1;
                if (dumped != hit.instance) {
                    dumped = hit.instance;
                    for (const std::vector<float>& v : world.InstanceShapes(hit.instance))
                        printf("      shape type %d surface %d size (%.3f %.3f %.3f) x (%.3f %.3f %.3f) y (%.3f %.3f %.3f) "
                               "z (%.3f %.3f %.3f) at (%.3f %.3f %.3f)\n", int(v[0]), int(v[1]), v[2], v[3], v[4], v[5],
                               v[6], v[7], v[9], v[10], v[11], v[13], v[14], v[15], v[17], v[18], v[19]);
                }
            }
        }
    }
    fclose(f);
    printf("%d rays: both miss %d, both hit %d (distance within 1e-3: %d, worst %.6f; surface equal %d)\n", rays, missBoth,
           hitsMatch, close, worst, surfaceMatch);
    return 0;
}
