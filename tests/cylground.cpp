// A cylinder's ground test (contact.cpp CylinderGroundPoints) against the
// game's own 0x5b3010 (gamecode.h) on random cylinders over a made-up
// ground, bit for bit. 0x5b3010 takes the cylinder's {radius, half height}
// in ecx and its world matrix in edx, then the stack arguments: the probe
// (cdecl, point, out: +0x00 normal, +0x10 height, +0x14 surface), the best
// record's holder, the free record's holder (its +0x00 = the depth to
// beat). It probes eight points; a deeper one takes the free record (depth
// +0x00, probe +0x10.., point +0x34..) and swaps the holders. Returns 5
// when the best depth is above 0, else 6.
//
//   cylground <Driv3r.exe> [rounds]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "../contact.h"
#include "gamecode.h"

namespace
{
std::mt19937 g_rng(8765);
float Uni(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }
uint32_t A(const void* p) { return reinterpret_cast<uint32_t>(p); }

void RandomMatrix(float* m, const float p[3])
{
    float q[4], n = 0;
    for (float& v : q) v = Uni(-1, 1), n += v * v;
    n = std::sqrt(n);
    for (float& v : q) v /= n;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float r[16] = { 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y), 0,
                          2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x), 0,
                          2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y), 0,
                          p[0], p[1], p[2], 1 };
    memcpy(m, r, sizeof(r));
}

// Rotations about one axis by t: about z (a lying wheel when t is near 90
// degrees, steep near 0), or about y after a quarter turn about z (lying,
// heading t).
void TurnMatrix(float* m, const float p[3], int kind, float t)
{
    const float c = std::cos(t), s = std::sin(t);
    float r[16] = {};
    if (kind == 0) {
        const float v[16] = { c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        memcpy(r, v, sizeof(r));
    } else {
        // rows: axis row 1 = (c, 0, -s) level
        const float v[16] = { 0, 1, 0, 0, c, 0, -s, 0, s, 0, c, 0, 0, 0, 0, 1 };
        memcpy(r, v, sizeof(r));
    }
    r[12] = p[0]; r[13] = p[1]; r[14] = p[2];
    memcpy(m, r, sizeof(r));
}

// The made-up ground: a tilted plane with bumps, normal up.
float GroundY(float x, float z) { return 0.25f * x - 0.15f * z + 0.1f * std::sin(3.0f * x + 2.0f * z); }

float g_probes[64][3];
int g_count = 0;
void __cdecl Probe(const float* p, float* out)
{
    if (g_count < 64) memcpy(g_probes[g_count], p, 12);
    ++g_count;
    out[0] = 0; out[1] = 1; out[2] = 0; out[3] = 0;
    out[4] = GroundY(p[0], p[2]);
    out[5] = 3;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: cylground <Driv3r.exe> [rounds]\n"); return 1; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!GameCode::Load(argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    const int rounds = argc > 2 ? atoi(argv[2]) : 10000;
    int faults = 0, badProbe = 0, badBest = 0, found = 0, kinds[3] = {};
    for (int r = 0; r < rounds; ++r) {
        alignas(16) float m[16], cs[4] = {};
        alignas(16) float recA[0x20] = {}, recB[0x20] = {};
        float p[3] = { Uni(-2, 2), Uni(-0.8f, 0.8f), Uni(-2, 2) };
        const int sort = r % 8;
        if (sort == 0) {  // standing exactly up
            const float v[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p[0], p[1], p[2], 1 };
            memcpy(m, v, sizeof(m));
            if (r % 16 == 0) TurnMatrix(m, p, 0, 0.0f);
        } else if (sort == 1) {  // upside down
            const float v[16] = { 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0, p[0], p[1], p[2], 1 };
            memcpy(m, v, sizeof(m));
        } else if (sort == 2) {
            TurnMatrix(m, p, 0, Uni(-0.6f, 0.6f));  // steep and near 0.9
        } else if (sort == 3) {
            TurnMatrix(m, p, 1, Uni(-3.2f, 3.2f));  // lying, any heading
        } else if (sort == 4) {
            TurnMatrix(m, p, 0, Uni(1.3f, 1.85f));  // lying, a little tilted
        } else {
            RandomMatrix(m, p);
        }
        cs[0] = Uni(0.05f, 0.6f);
        cs[1] = Uni(0.05f, 1.2f);
        float* best = recA;
        float* free = recB;
        free[0] = 0.0f;
        g_count = 0;
        GameRegs g;
        g.ecx = A(cs); g.edx = A(m);
        GameRegs out;
        if (!GameCode::Call(0x5b3010, g, { A(reinterpret_cast<void*>(&Probe)), A(&best), A(&free) }, &out)) {
            if (faults++ < 3) printf("round %d: fault at game %08x\n", r, GameCode::Game(GameCode::g_faultAddr));
            continue;
        }
        float mp[8][3], low[8];
        CylinderGroundPoints(cs[0], cs[1], m, mp, low);
        float ay = std::fabs(m[5]);
        ++kinds[std::fabs(m[5] - 1.0f) < 1e-5f ? 0 : ay > 0.9f ? 1 : 2];
        bool ok = g_count == 8;
        for (int i = 0; ok && i < 8; ++i)
            for (int k = 0; k < 3; ++k)
                if (GameCode::Bits(mp[i][k]) != GameCode::Bits(g_probes[i][k])) ok = false;
        if (!ok) {
            if (badProbe++ < 6) {
                printf("round %d: probes differ (game %d calls), a.y %.9g\n", r, g_count, m[5]);
                for (int i = 0; i < 8 && i < g_count; ++i)
                    printf("  %d game (%.9g %.9g %.9g) port (%.9g %.9g %.9g)\n", i, g_probes[i][0], g_probes[i][1],
                           g_probes[i][2], mp[i][0], mp[i][1], mp[i][2]);
            }
            continue;
        }
        // the deepest as Step takes it: strictly deeper than the last, above 0
        int bi = -1;
        float bd = 0.0f;
        for (int i = 0; i < 8; ++i) {
            const float d = GroundY(mp[i][0], mp[i][2]) - low[i];
            if (d > bd) bd = d, bi = i;
        }
        const bool gameFound = out.eax == 5;
        bool same = gameFound == (bi >= 0);
        if (same && bi >= 0) {
            same = GameCode::Bits(best[0]) == GameCode::Bits(bd) && best[0x34 / 4] == mp[bi][0] &&
                   GameCode::Bits(best[0x38 / 4]) == GameCode::Bits(low[bi]) && best[0x3c / 4] == mp[bi][2];
            ++found;
        }
        if (!same && badBest++ < 6)
            printf("round %d: best differs: game %u d %.9g p (%.9g %.9g %.9g), port %d d %.9g\n", r, out.eax, best[0],
                   best[13], best[14], best[15], bi, bd);
    }
    printf("%d rounds (up %d, steep %d, lying %d): %d found, %d faults, %d probe differences, %d best differences\n",
           rounds, kinds[0], kinds[1], kinds[2], found, faults, badProbe, badBest);
    return faults || badProbe || badBest ? 1 : 0;
}
