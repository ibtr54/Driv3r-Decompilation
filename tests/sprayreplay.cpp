// The car tick's spray on loose ground and its tyre smoke (CarSim::Effects)
// against run 32 (BOOT_TO_MENU.md section 85): for each traced car tick, the
// object after the tick ("FCO"; the gate obj+0xb80 from before, "FCo") and
// the body's velocity ("FCv", traced since run 32) into Effects; its
// decision against the game's emitters after the tick: the tyre smoke
// (record word 0, "FCe": on, alpha, position) and the spray record (word 2,
// "FCs0".."FCs2": each emitter's 0x180 bytes, "FCr" the record): on +0x9c,
// position +0x20, the colours +0x118..+0x148 (0x570c60 by the record's
// +0xc), yaw +0xc0 / pitch +0xc8, speeds +0xd0 / +0xd4 and emitter 2's rate
// +0x170 (0x570b90).
//   sprayreplay <phys.txt>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../carsim.h"
#include "../smoke.h"

namespace {

struct Emit
{
    int on = 0;
    float alpha = 0, pos[3] = {};
};
struct TickRec
{
    std::vector<uint8_t> before, after;
    float vel[4] = {};
    bool haveVel = false;
    std::map<int, Emit> smoke;                       // word 0 per wheel
    std::map<int, std::vector<uint32_t>> spray[3];  // word 2's emitters per wheel
    std::map<int, std::vector<uint32_t>> record;    // word 2's record per wheel
};

std::vector<uint32_t> Words(const char* line)
{
    const char* p = line;
    for (int k = 0; k < 3; ++k) {
        p = strchr(p, ' ');
        if (!p) return {};
        ++p;
    }
    std::vector<uint32_t> out;
    for (;;) {
        char* end = nullptr;
        unsigned long w = strtoul(p, &end, 16);
        if (end == p) break;
        out.push_back(uint32_t(w));
        p = end;
    }
    return out;
}

float F(uint32_t w)
{
    float f;
    memcpy(&f, &w, 4);
    return f;
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
        fprintf(stderr, "usage: sprayreplay <phys.txt>\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::map<unsigned, TickRec> ticks;
    std::vector<char> buf(1 << 16);
    int lastWheel = -1;
    while (fgets(buf.data(), int(buf.size()), f)) {
        const char* l = buf.data();
        unsigned n = 0;
        if (!strncmp(l, "FCo ", 4) || !strncmp(l, "FCO ", 4)) {
            n = unsigned(strtoul(l + 4, nullptr, 10));
            std::vector<uint32_t> w = Words(l);
            std::vector<uint8_t>& o = l[2] == 'o' ? ticks[n].before : ticks[n].after;
            o.assign(CarSim::kObjectSize, 0);
            if (w.size() * 4 >= size_t(CarSim::kObjectSize)) memcpy(o.data(), w.data(), CarSim::kObjectSize);
            else o.clear();
        } else if (!strncmp(l, "FCv ", 4)) {
            n = unsigned(strtoul(l + 4, nullptr, 10));
            std::vector<uint32_t> w = Words(l);
            if (w.size() >= 4) {
                for (int k = 0; k < 4; ++k) ticks[n].vel[k] = F(w[size_t(k)]);
                ticks[n].haveVel = true;
            }
        } else if (!strncmp(l, "FCe ", 4)) {
            int w, word, on;
            float a, x, y, z;
            if (sscanf(l + 4, "%u %d %d %d %f %f %f %f", &n, &w, &word, &on, &a, &x, &y, &z) == 8) {
                lastWheel = w;
                if (word == 0) {
                    Emit& e = ticks[n].smoke[w];
                    e.on = on;
                    e.alpha = a;
                    e.pos[0] = x, e.pos[1] = y, e.pos[2] = z;
                }
            }
        } else if (!strncmp(l, "FCr ", 4)) {
            n = unsigned(strtoul(l + 4, nullptr, 10));
            ticks[n].record[lastWheel] = Words(l);
        } else if (!strncmp(l, "FCs", 3) && l[3] >= '0' && l[3] <= '2') {
            n = unsigned(strtoul(l + 5, nullptr, 10));
            ticks[n].spray[l[3] - '0'][lastWheel] = Words(l);
        }
    }
    fclose(f);

    // 0x570c60's colours (+0x118..+0x148) per emitter, dirt / sand
    static const float kColour[2][3][8] = {
        { { 0.44140625f, 0.39453125f, 0.32421875f, 0.546875f, 0.56640625f, 0.51171875f, 0.4296875f, 0.0f },
          { 0.44140625f, 0.39453125f, 0.32421875f, 0.546875f, 0.56640625f, 0.51171875f, 0.4296875f, 0.0f },
          { 0.1953125f, 0.17578125f, 0.078125f, 0.8984375f, 0.1953125f, 0.17578125f, 0.078125f, 0.17578125f } },
        { { 0.75390625f, 0.70703125f, 0.63671875f, 0.546875f, 0.87890625f, 0.82421875f, 0.7421875f, 0.0f },
          { 0.75390625f, 0.70703125f, 0.63671875f, 0.546875f, 0.87890625f, 0.82421875f, 0.7421875f, 0.0f },
          { 0.63671875f, 0.58984375f, 0.51953125f, 0.8984375f, 0.76171875f, 0.70703125f, 0.625f, 0.17578125f } },
    };
    int ran = 0, gateMiss = 0, blocks = 0, smokeOk = 0, smokeMiss = 0, sprayOk = 0, sprayMiss = 0, sprayEarly = 0;
    int colourMiss = 0, angleChecks = 0, valueMiss = 0;
    float maxAlpha = 0, maxPos = 0, maxAngle = 0, maxValue = 0;
    int shown = 0;
    for (auto& [n, t] : ticks) {
        if (t.before.size() != size_t(CarSim::kObjectSize) || t.after.size() != size_t(CarSim::kObjectSize) || !t.haveVel)
            continue;
        static CarSim car;
        memcpy(car.obj, t.after.data(), CarSim::kObjectSize);
        car.OF(0xb80) = Fl(t.before, 0xb80);
        for (int w = 0; w < 6; ++w) car.slots[w] = (car.OU(0x74 + w * 0xd0) & 0x40) ? 1 : 0;
        CarSmokeState s;
        const bool did = car.Effects(t.vel, s);
        const bool gameRan = Fl(t.after, 0xb80) < Fl(t.before, 0xb80);
        ++ran;
        if (did != gameRan) ++gateMiss;
        if (!did) continue;
        ++blocks;
        for (int k = 0; k < 2; ++k) {
            const int w = k ? 3 : 1;
            // the tyre smoke, now with the traced velocity
            auto it = t.smoke.find(w);
            if (it == t.smoke.end() ? s.tyreOn[k] : it->second.on != int(s.tyreOn[k])) {
                ++smokeMiss;
                if (shown++ < 20) printf("tick %u wheel %d: tyre smoke game %d port %d (slip %.5f)\n", n, w,
                                         it == t.smoke.end() ? -1 : it->second.on, int(s.tyreOn[k]), car.slip);
            } else {
                ++smokeOk;
                if (it != t.smoke.end() && it->second.on) {
                    maxAlpha = std::fmax(maxAlpha, std::fabs(it->second.alpha - s.tyreAlpha[k]));
                    for (int j = 0; j < 3; ++j) maxPos = std::fmax(maxPos, std::fabs(it->second.pos[j] - s.tyrePos[k][j]));
                }
            }
            // the spray record
            const bool taken = t.spray[0].count(w) && t.spray[1].count(w) && t.spray[2].count(w);
            for (int j = 0; j < 3; ++j) {
                if (!taken) {
                    if (s.sprayOn[k][j]) {
                        ++sprayEarly;
                        if (shown++ < 20) printf("tick %u wheel %d emitter %d: port on, game not taken\n", n, w, j);
                    } else {
                        ++sprayOk;
                    }
                    continue;
                }
                const std::vector<uint32_t>& e = t.spray[j][w];
                const int on = int(e[0x9c / 4] & 0xff);
                if (on != int(s.sprayOn[k][j])) {
                    ++sprayMiss;
                    if (shown++ < 20)
                        printf("tick %u wheel %d emitter %d: game %d port %d (surface %u, ratio %.3f, speed %.3f, flags %x)\n",
                               n, w, j, on, int(s.sprayOn[k][j]), car.OU(0x78 + w * 0xd0), car.OF(0x80 + w * 0xd0),
                               std::sqrt(t.vel[0] * t.vel[0] + t.vel[1] * t.vel[1] + t.vel[2] * t.vel[2]),
                               car.OU(0x74 + w * 0xd0));
                } else {
                    ++sprayOk;
                }
                for (int q = 0; q < 3; ++q) maxPos = std::fmax(maxPos, std::fabs(F(e[(0x20 + 4 * q) / 4]) - s.tyrePos[k][q]));
                // the start colour +0x118..+0x124, the end colour +0x13c..+0x148
                const int sand = t.record.count(w) && t.record[w].size() > 3 ? int(t.record[w][3]) : 0;
                if (sand != int(s.spraySand[k])) ++colourMiss;
                for (int q = 0; q < 8; ++q)
                    if (F(e[((q < 4 ? 0x118 : 0x12c) + 4 * q) / 4]) != kColour[sand & 1][j][q]) ++colourMiss;
                // 0x570b90 (1: yaw, pitch; 2: yaw; speeds; 2's rate)
                if (j == 0) continue;
                const float* m = &car.OF(0xac0);
                const float yaw = GameAtan2(m[8], -m[10]), pitch = std::asin(-m[9]);
                ++angleChecks;
                maxAngle = std::fmax(maxAngle, std::fabs(F(e[0xc0 / 4]) - yaw));
                if (j == 1) maxAngle = std::fmax(maxAngle, std::fabs(F(e[0xc8 / 4]) - pitch));
                const float v = j == 2 && s.spraySpeed < 2.0f ? 2.0f : s.spraySpeed;
                const float want[3] = { v * 0.0500000007f, v * 0.0350000001f, 15.0f - v * 1.5f };
                const float got[3] = { F(e[0xd0 / 4]), F(e[0xd4 / 4]), F(e[0x170 / 4]) };
                for (int q = 0; q < (j == 2 ? 3 : 2); ++q) {
                    const float d = std::fabs(got[q] - want[q]);
                    maxValue = std::fmax(maxValue, d);
                    if (d != 0) ++valueMiss;
                }
            }
        }
    }
    printf("%d ticks, gate differs %d; %d blocks\n", ran, gateMiss, blocks);
    printf("tyre smoke: %d agree, %d differ; max |alpha| %.6g\n", smokeOk, smokeMiss, maxAlpha);
    printf("spray: %d emitter decisions agree, %d differ, %d port-early; colour words wrong %d\n", sprayOk, sprayMiss,
           sprayEarly, colourMiss);
    printf("spray aim: %d checks, max |angle| %.6g; speeds / rate: %d not bit-equal (max %.6g); max |position| %.6g\n",
           angleChecks, maxAngle, valueMiss, maxValue, maxPos);
    return (gateMiss || smokeMiss || sprayMiss || sprayEarly || colourMiss) ? 1 : 0;
}
