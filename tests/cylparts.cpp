// The pieces of box / cylinder (contact.cpp, included here for its internal
// functions) against the game's: the box in the cylinder's frame 0x5b4120,
// the side-line segments 0x5b5950 and their closest points 0x5b8ab0.
//
//   cylparts <Driv3r.exe> [rounds]
#include "../contact.cpp"

#include <random>

#include "gamecode.h"

namespace
{
std::mt19937 g_rng(4321);
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

int Diff(const char* name, const float* mine, const float* game, int n, int round, int& shown)
{
    int bad = 0;
    for (int i = 0; i < n; ++i)
        if (GameCode::Bits(mine[i]) != GameCode::Bits(game[i])) {
            if (shown++ < 12)
                printf("round %d %s [%d]: %.9g, game %.9g\n", round, name, i, mine[i], game[i]);
            ++bad;
        }
    return bad ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || !GameCode::Load(argv[1])) { fprintf(stderr, "usage: cylparts <Driv3r.exe> [rounds]\n"); return 1; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int rounds = argc > 2 ? atoi(argv[2]) : 2000;
    int badL = 0, badT = 0, badE = 0, shown = 0, rims = 0, closest = 0;
    for (int r = 0; r < rounds; ++r) {
        alignas(16) float box[16], cyl[16], half[4] = {}, cs[4] = {};
        const float zero[3] = { 0, 0, 0 };
        float p[3];
        for (float& v : p) v = Uni(-1.5f, 1.5f);
        RandomMatrix(box, zero);
        RandomMatrix(cyl, p);
        for (int i = 0; i < 3; ++i) half[i] = Uni(0.1f, 1.5f);
        cs[0] = Uni(0.05f, 1.0f);
        cs[1] = Uni(0.05f, 1.5f);

        // the frame
        alignas(16) float Lg[16], L[16];
        GameRegs g;
        g.eax = A(cyl); g.esi = A(Lg);
        GameCode::Call(0x5b4120, g, { A(box) });
        CylFrame(box, cyl, L);
        badL += Diff("L", L, Lg, 16, r, shown);

        // the records as 0x5b4380 builds them
        alignas(16) float boxRec[32] = {}, cylRec[16] = {};
        Box B;
        MakeBox(half, box, B);
        for (int i = 0; i < 3; ++i) {
            for (int k = 0; k < 3; ++k) {
                boxRec[i * 4 + k] = B.axis[i][k];
                boxRec[16 + i * 4 + k] = B.sa[i][k];
            }
            boxRec[12 + i] = B.c[i];
            boxRec[28 + i] = B.half[i];
        }
        Cyl C;
        for (int k = 0; k < 3; ++k) {
            C.axis[k] = cyl[4 + k];
            C.c[k] = cyl[12 + k];
            C.ah[k] = C.axis[k] * cs[1];
            cylRec[k] = C.axis[k];
            cylRec[4 + k] = C.c[k];
            cylRec[8 + k] = C.ah[k];
        }
        C.r = cylRec[12] = cs[0];
        C.h = cylRec[13] = cs[1];

        // the side-line segments, edge by edge on fresh records
        for (int i = 0; i < 3; ++i) {
            alignas(16) float T[0x20] = {};
            T[0x1c] = 1000000.0f;
            g = {};
            g.ebx = A(Lg); g.edi = A(T);
            GameRegs out;
            GameCode::Call(0x5b5950, g, { A(boxRec), A(cylRec), static_cast<uint32_t>(i) }, &out);
            Sat t;
            const bool mine = CylRimEdge(Lg, B, C, i, t);
            if (mine != (out.eax != 0)) { ++badT; continue; }
            if (!t.found) continue;
            ++rims;
            const float m[0x1d - 0] = {};
            (void)m;
            int b = 0;
            b |= Diff("T.p0", t.p0, T, 3, r, shown);
            b |= Diff("T.p1", t.p1, T + 4, 3, r, shown);
            b |= Diff("T.n", t.n, T + 8, 3, r, shown);
            b |= Diff("T.a0", t.a0, T + 12, 3, r, shown);
            b |= Diff("T.a1", t.a1, T + 16, 3, r, shown);
            b |= Diff("T.b0", t.b0, T + 20, 3, r, shown);
            b |= Diff("T.b1", t.b1, T + 24, 3, r, shown);
            b |= Diff("T.best", &t.best, T + 0x1c, 1, r, shown);
            badT += b;

            // the closest points on the game's segments
            alignas(16) float R[0x20] = {};
            g = {};
            g.ecx = A(T); g.esi = A(R);
            GameCode::Call(0x5b8ab0, g, {}, &out);
            Sat tt;
            memcpy(tt.a0, T + 12, 12); memcpy(tt.a1, T + 16, 12);
            memcpy(tt.b0, T + 20, 12); memcpy(tt.b1, T + 24, 12);
            Sat o;
            const bool ok = EdgeClosest(tt, o);
            if (ok != (out.eax != 0)) { ++badE; continue; }
            if (!ok) continue;
            ++closest;
            int e = 0;
            e |= Diff("E.p0", o.p0, R, 3, r, shown);
            e |= Diff("E.p1", o.p1, R + 4, 3, r, shown);
            e |= Diff("E.best", &o.best, R + 0x1c, 1, r, shown);
            badE += e;
        }
    }
    printf("%d rounds: L differs %d; %d rim segments, %d differ; %d closest points, %d differ\n", rounds, badL, rims, badT,
           closest, badE);
    return badL || badT || badE ? 1 : 0;
}
