// The articulated solver's primitives (articulated.cpp) against the game's
// own routines (gamecode.h) on random inputs, bit for bit.
//
//   spatialtest <Driv3r.exe> [rounds]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "../articulated.h"
#include "gamecode.h"

using namespace Articulated;

namespace
{
std::mt19937 g_rng(12345);

float Rnd()
{
    std::uniform_real_distribution<float> u(-2.0f, 2.0f);
    std::uniform_int_distribution<int> e(-6, 6);
    return std::ldexp(u(g_rng), e(g_rng) / 2);
}
void Fill(float* p, int n)
{
    for (int i = 0; i < n; ++i) p[i] = (i % 4 == 3) ? 0.0f : Rnd();
}
uint32_t A(const void* p) { return reinterpret_cast<uint32_t>(p); }

// compares n floats, skipping each 4th (padding) when pads is set
int Diff(const char* name, const float* mine, const float* game, int n, bool pads = true)
{
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        if (pads && i % 4 == 3) continue;
        if (GameCode::Bits(mine[i]) == GameCode::Bits(game[i])) continue;
        if (bad++ < 4)
            printf("  %s [%d]: %08x (%g), game %08x (%g)\n", name, i, GameCode::Bits(mine[i]), mine[i],
                   GameCode::Bits(game[i]), game[i]);
    }
    return bad;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: spatialtest <Driv3r.exe> [rounds]\n"); return 1; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!GameCode::Load(argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    const int rounds = argc > 2 ? atoi(argv[2]) : 200;
    struct Count { const char* name; int bad = 0; } counts[13] = {
        { "M3V 0x5bdef0" }, { "VecRot 0x5bf350" }, { "Add 0x5bdff0" }, { "Scale 0x5be0b0" },
        { "VecTimesMat 0x5be2b0" }, { "MatTimesVec 0x5be3f0" }, { "Add 0x5be530" }, { "Scale 0x5be7c0" },
        { "Outer 0x5bea30" }, { "Solve 0x5bead0" }, { "Transform 0x5bef80" }, { "Transform 0x5bf640" },
        { "QuatRotate 0x58c1f0" },
    };
    for (int r = 0; r < rounds; ++r) {
        alignas(16) float m[48], m2[48], v[8], v2[8], R[16], mine[48], game[48];
        Fill(m, 48); Fill(m2, 48); Fill(v, 8); Fill(v2, 8); Fill(R, 16);
        const float s = Rnd();
        GameRegs g;
        auto M33 = [](float* p) { return reinterpret_cast<const float(*)[4]>(p); };

        M3V(M33(m), v, mine);
        g = {}; g.eax = A(game); g.edx = A(m); g.ecx = A(v); GameCode::Call(0x5bdef0, g);
        counts[0].bad += Diff(counts[0].name, mine, game, 4);

        VecRot(M33(m), v, mine);
        g = {}; g.ecx = A(m); g.eax = A(v); g.edi = A(game); GameCode::Call(0x5bf350, g);
        counts[1].bad += Diff(counts[1].name, mine, game, 4);

        Add(*reinterpret_cast<SVec*>(v), *reinterpret_cast<SVec*>(v2), *reinterpret_cast<SVec*>(mine));
        g = {}; g.eax = A(game); g.edx = A(v); g.ecx = A(v2); GameCode::Call(0x5bdff0, g);
        counts[2].bad += Diff(counts[2].name, mine, game, 8);

        Scale(*reinterpret_cast<SVec*>(v), s, *reinterpret_cast<SVec*>(mine));
        g = {}; g.eax = A(game); g.ecx = A(v); GameCode::Call(0x5be0b0, g, { GameCode::Bits(s) });
        counts[3].bad += Diff(counts[3].name, mine, game, 8);

        VecTimesMat(*reinterpret_cast<SVec*>(v), *reinterpret_cast<SMat*>(m), *reinterpret_cast<SVec*>(mine));
        g = {}; g.eax = A(v); g.ecx = A(m); g.esi = A(game); GameCode::Call(0x5be2b0, g);
        counts[4].bad += Diff(counts[4].name, mine, game, 8);

        MatTimesVec(*reinterpret_cast<SMat*>(m), *reinterpret_cast<SVec*>(v), *reinterpret_cast<SVec*>(mine));
        g = {}; g.ebx = A(v); g.edi = A(m); g.esi = A(game); GameCode::Call(0x5be3f0, g);
        counts[5].bad += Diff(counts[5].name, mine, game, 8);

        Add(*reinterpret_cast<SMat*>(m), *reinterpret_cast<SMat*>(m2), *reinterpret_cast<SMat*>(mine));
        g = {}; g.ebx = A(game); GameCode::Call(0x5be530, g, { A(m), A(m2) });
        counts[6].bad += Diff(counts[6].name, mine, game, 48);

        Scale(s, *reinterpret_cast<SMat*>(m), *reinterpret_cast<SMat*>(mine));
        g = {}; g.ebx = A(game); GameCode::Call(0x5be7c0, g, { GameCode::Bits(s), A(m) });
        counts[7].bad += Diff(counts[7].name, mine, game, 48);

        memset(mine, 0, sizeof(mine)); memset(game, 0, sizeof(game));
        Outer(*reinterpret_cast<SVec*>(v), *reinterpret_cast<SVec*>(v2), *reinterpret_cast<SMat*>(mine));
        g = {}; g.eax = A(game); g.ecx = A(v); GameCode::Call(0x5bea30, g, { A(v2) });
        counts[8].bad += Diff(counts[8].name, mine, game, 48);

        {
            alignas(16) float mm[48], mg[48], bm[8], bg[8], xm[8] = {}, xg[8] = {};
            memcpy(mm, m, sizeof(mm)); memcpy(mg, m, sizeof(mg));
            memcpy(bm, v, sizeof(bm)); memcpy(bg, v, sizeof(bg));
            Solve(*reinterpret_cast<SVec*>(xm), *reinterpret_cast<SMat*>(mm), *reinterpret_cast<SVec*>(bm));
            g = {}; GameCode::Call(0x5bead0, g, { A(xg), A(mg), A(bg) });
            counts[9].bad += Diff(counts[9].name, xm, xg, 8) + Diff("Solve m", mm, mg, 48) + Diff("Solve rhs", bm, bg, 8);
        }

        Transform(reinterpret_cast<const float(*)[4]>(R), *reinterpret_cast<SVec*>(v), *reinterpret_cast<SVec*>(mine));
        g = {}; g.ecx = A(R); g.eax = A(v); g.edi = A(game); GameCode::Call(0x5bef80, g);
        counts[10].bad += Diff(counts[10].name, mine, game, 8);

        Transform(reinterpret_cast<const float(*)[4]>(R), *reinterpret_cast<SMat*>(m), *reinterpret_cast<SMat*>(mine));
        g = {}; g.eax = A(game); GameCode::Call(0x5bf640, g, { A(R), A(m) });
        counts[11].bad += Diff(counts[11].name, mine, game, 48);

        QuatRotate(R, v, mine);
        g = {}; g.eax = A(v); g.esi = A(R); g.edi = A(game); GameCode::Call(0x58c1f0, g);
        counts[12].bad += Diff(counts[12].name, mine, game, 4, false);
    }
    int total = 0;
    for (const Count& c : counts) {
        printf("%-24s %s (%d words)\n", c.name, c.bad ? "DIFFERS" : "exact", c.bad);
        total += c.bad;
    }
    printf("%d rounds, %s\n", rounds, total ? "differences" : "all exact");
    return total ? 1 : 0;
}
