// The car tick's tyre smoke (CarSim::Effects) against run 28 (BOOT_TO_MENU.md
// section 83): for each traced car tick, the object after the tick ("FCO";
// the block reads nothing the block itself writes but the gate obj+0xb80,
// taken from the object before the tick, "FCo") and the body's velocity
// (not traced: the change of the matrix's position obj+0xaf0 from the tick
// before, x 120) into Effects; its decision for the rear wheels against the
// game's emitters after the tick ("FCe": on +0x9c, alpha +0x124, position
// +0x20). The wheels the model has (obj+0x648 indexing) are taken from the
// records' present flag.
//   smokereplay <phys.txt>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../carsim.h"

namespace {

struct Emit
{
    int on = 0;
    float alpha = 0, pos[3] = {};
};
struct TickRec
{
    std::vector<uint8_t> before, after;
    std::map<int, Emit> wheel;  // word 0 of the wheel's record
};

bool ReadDump(const char* line, std::vector<uint8_t>& out)
{
    // "FCo n addr w0 w1 ..."
    const char* p = line;
    for (int k = 0; k < 3; ++k) {
        p = strchr(p, ' ');
        if (!p) return false;
        ++p;
    }
    out.assign(CarSim::kObjectSize, 0);
    for (int i = 0; i < CarSim::kObjectSize / 4; ++i) {
        char* end = nullptr;
        uint32_t w = strtoul(p, &end, 16);
        if (end == p) return false;
        memcpy(&out[i * 4], &w, 4);
        p = end;
    }
    return true;
}

float Fl(const std::vector<uint8_t>& o, int off)
{
    float f;
    memcpy(&f, &o[off], 4);
    return f;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: smokereplay <phys.txt>\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::map<unsigned, TickRec> ticks;
    std::vector<char> buf(1 << 16);
    while (fgets(buf.data(), int(buf.size()), f)) {
        const char* l = buf.data();
        unsigned n = 0;
        if (!strncmp(l, "FCo ", 4) || !strncmp(l, "FCO ", 4)) {
            n = unsigned(strtoul(l + 4, nullptr, 10));
            ReadDump(l, l[2] == 'o' ? ticks[n].before : ticks[n].after);
        } else if (!strncmp(l, "FCe ", 4)) {
            int w, word, on;
            float a, x, y, z;
            if (sscanf(l + 4, "%u %d %d %d %f %f %f %f", &n, &w, &word, &on, &a, &x, &y, &z) == 8 && word == 0) {
                Emit& e = ticks[n].wheel[w];
                e.on = on;
                e.alpha = a;
                e.pos[0] = x, e.pos[1] = y, e.pos[2] = z;
            }
        }
    }
    fclose(f);

    int ran = 0, gateMiss = 0, blocks = 0, onMatch = 0, onMiss = 0, early = 0, late = 0;
    float maxAlpha = 0, maxPos = 0;
    int shown = 0;
    std::vector<uint8_t> prev;
    for (auto& [n, t] : ticks) {
        if (t.before.size() != size_t(CarSim::kObjectSize) || t.after.size() != size_t(CarSim::kObjectSize)) {
            prev.clear();
            continue;
        }
        static CarSim car;
        memcpy(car.obj, t.after.data(), CarSim::kObjectSize);
        car.OF(0xb80) = Fl(t.before, 0xb80);
        for (int w = 0; w < 6; ++w) car.slots[w] = (car.OU(0x74 + w * 0xd0) & 0x40) ? 1 : 0;
        float vel[3] = {};
        if (!prev.empty())
            for (int k = 0; k < 3; ++k) vel[k] = (Fl(t.after, 0xaf0 + 4 * k) - Fl(prev, 0xaf0 + 4 * k)) * 120.0f;
        prev = t.after;
        CarSmokeState s;
        const bool did = car.Effects(vel, s);
        // the game's gate: obj+0xb80 after against before
        const bool gameRan = Fl(t.after, 0xb80) < Fl(t.before, 0xb80);
        ++ran;
        if (did != gameRan) ++gateMiss;
        if (!did) continue;
        ++blocks;
        for (int k = 0; k < 2; ++k) {
            const int w = k ? 3 : 1;
            auto it = t.wheel.find(w);
            if (it == t.wheel.end()) {
                // not yet taken by the game: the port must not smoke either
                if (s.tyreOn[k]) {
                    ++early;
                    if (shown++ < 12)
                        printf("tick %u wheel %d: port on (slip %.4f), game not taken\n", n, w, car.slip);
                } else {
                    ++onMatch;
                }
                continue;
            }
            const Emit& e = it->second;
            if (e.on != int(s.tyreOn[k])) {
                ++onMiss;
                if (e.on) ++late;
                if (shown++ < 12)
                    printf("tick %u wheel %d: game on %d alpha %.6f, port on %d alpha %.6f (slip %.4f)\n", n, w, e.on,
                           e.alpha, int(s.tyreOn[k]), s.tyreAlpha[k], car.slip);
                continue;
            }
            ++onMatch;
            if (argc > 2)
                printf("tick %u wheel %d on %d: alpha %.6f / %.6f, pos (%.4f %.6f %.4f) / (%.4f %.6f %.4f), t %.4f\n", n, w,
                       e.on, e.alpha, s.tyreAlpha[k], e.pos[0], e.pos[1], e.pos[2], s.tyrePos[k][0], s.tyrePos[k][1],
                       s.tyrePos[k][2], car.OF(0xb7c) * 25.0f);
            for (int j = 0; j < 3; ++j) maxPos = std::fmax(maxPos, std::fabs(e.pos[j] - s.tyrePos[k][j]));
            if (e.on) maxAlpha = std::fmax(maxAlpha, std::fabs(e.alpha - s.tyreAlpha[k]));
        }
    }
    printf("%d ticks, gate differs %d; %d blocks: rear-wheel decisions agree %d, differ %d "
           "(port early %d, port missing %d)\n",
           ran, gateMiss, blocks, onMatch, onMiss + early, early, late);
    printf("max |alpha| diff %.6g (while on), max |position| diff %.6g\n", maxAlpha, maxPos);
    return (gateMiss || onMiss || early) ? 1 : 0;
}
