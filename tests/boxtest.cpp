// Box / box contacts (contact.cpp BoxBoxContact) against the game's own
// 0x5b8df0 (gamecode.h) on random pairs, bit for bit (BOOT_TO_MENU.md section
// 96). 0x5b8df0 (REA): box A's matrix in ebx, box B's in edi, their half
// sizes as the two stack arguments (A first), the contact record in esi
// (+0x00 point A, +0x10 point B, +0x20 normal, +0x30 depth, +0x34 contact).
//
//   boxtest <Driv3r.exe> [rounds] [verbose]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "../contact.h"
#include "gamecode.h"

namespace
{
std::mt19937 g_rng(9876);
float Uni(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }
uint32_t A(const void* p) { return reinterpret_cast<uint32_t>(p); }

// A random rotation (from a unit quaternion) at position p, row vectors.
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

// Axis-aligned and parallel cases too (edge axes of length 0, signed zeros):
// every few rounds one box is the identity or a quarter turn.
void MaybeAlign(float* m, int r)
{
    if (r % 4 != 0) return;
    const float p[3] = { m[12], m[13], m[14] };
    memset(m, 0, 16 * sizeof(float));
    if (r % 8 == 0) {
        m[0] = m[5] = m[10] = 1;
    } else {
        m[2] = 1; m[5] = 1; m[8] = -1;
    }
    m[12] = p[0]; m[13] = p[1]; m[14] = p[2]; m[15] = 1;
}

void Print(const char* who, const float* a, const float* b, const float* n, float d)
{
    printf("  %s A (%.9g %.9g %.9g) B (%.9g %.9g %.9g) n (%.9g %.9g %.9g) d %.9g\n", who, a[0], a[1], a[2], b[0], b[1],
           b[2], n[0], n[1], n[2], d);
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: boxtest <Driv3r.exe> [rounds] [verbose]\n"); return 1; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!GameCode::Load(argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    const int rounds = argc > 2 ? atoi(argv[2]) : 100000;
    const bool verbose = argc > 3;
    const char* names[5] = { "point A", "point B", "normal", "depth", "contact" };
    int contacts = 0, edges = 0, faults = 0, bad = 0, which[5] = {};
    for (int r = 0; r < rounds; ++r) {
        alignas(16) float ma[16], mb[16], ha[4], hb[4], rec[0x40] = {};
        float pa[3], pb[3];
        // world-sized positions sometimes (the city is near x 2000, z -1100)
        const float base = (r % 3 == 0) ? 1990.0f : 0.0f;
        for (int k = 0; k < 3; ++k) {
            pa[k] = (k == 1 ? 0.5f : (k == 0 ? base : -base * 0.55f)) + Uni(-0.2f, 0.2f);
            pb[k] = pa[k] + Uni(-2.0f, 2.0f);
        }
        RandomMatrix(ma, pa);
        RandomMatrix(mb, pb);
        MaybeAlign(r % 3 ? mb : ma, r);
        for (int i = 0; i < 3; ++i) {
            ha[i] = Uni(0.05f, 1.5f);
            hb[i] = Uni(0.05f, 1.5f);
        }
        ha[3] = hb[3] = 0;
        GameRegs g;
        g.ebx = A(ma); g.edi = A(mb); g.esi = A(rec);
        GameRegs out;
        if (!GameCode::Call(0x5b8df0, g, { A(ha), A(hb) }, &out)) {
            if (faults++ < 3) printf("round %d: fault at game %08x\n", r, GameCode::Game(GameCode::g_faultAddr));
            continue;
        }
        const bool game = out.eax != 0;
        if (game) {
            ++contacts;
            if (memcmp(rec, rec + 4, 12) != 0) ++edges;
        }
        BodyContact c;
        const bool mine = BoxBoxContact(ha, ma, hb, mb, c);
        const float* m[4] = { c.pointA, c.pointB, c.normal, &c.depth };
        const int n[4] = { 3, 3, 3, 1 };
        int field = mine == game ? -1 : 4;
        if (field < 0 && mine)
            for (int f = 0; f < 4 && field < 0; ++f)
                for (int k = 0; k < n[f]; ++k)
                    if (GameCode::Bits(m[f][k]) != GameCode::Bits(rec[f * 4 + k])) field = f;
        if (field >= 0) {
            ++which[field];
            if (bad++ < 6) {
                printf("round %d: %s (game %d, port %d)\n", r, names[field], game, mine);
                Print("game", rec, rec + 4, rec + 8, rec[12]);
                Print("port", c.pointA, c.pointB, c.normal, c.depth);
            }
        } else if (verbose && game && contacts <= 8) {
            printf("round %d:\n", r);
            Print("game", rec, rec + 4, rec + 8, rec[12]);
        }
    }
    printf("%d rounds: %d contacts (%d edge), %d faults, %d differ (A %d, B %d, normal %d, depth %d, contact %d)\n", rounds,
           contacts, edges, faults, bad, which[0], which[1], which[2], which[3], which[4]);
    return faults || bad ? 1 : 0;
}
