// Box / cylinder contacts (contact.cpp BoxCylinderContact) against the
// game's own 0x5b4380 (gamecode.h) on random pairs, bit for bit. 0x5b4380
// takes the box's matrix in eax and its half sizes as the stack argument,
// the cylinder's {radius, half height} in ecx and its matrix in ebx (axis row
// 1, position row 3), the contact record in edi (+0x00 point A, +0x10 point
// B, +0x20 normal, +0x30 depth, +0x34 contact).
//
//   cyltest <Driv3r.exe> [rounds] [verbose]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "../contact.h"
#include "gamecode.h"

namespace
{
std::mt19937 g_rng(4321);
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

// Axis-aligned cases too (the degenerate branches): every few rounds the
// rotation is the identity or a quarter turn.
void MaybeAlign(float* m, int r)
{
    if (r % 5 != 0) return;
    const float p[3] = { m[12], m[13], m[14] };
    memset(m, 0, 16 * sizeof(float));
    if (r % 10 == 0) {
        m[0] = m[5] = m[10] = 1;
    } else {
        m[1] = 1; m[4] = -1; m[10] = 1;
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
    if (argc < 2) { fprintf(stderr, "usage: cyltest <Driv3r.exe> [rounds] [verbose]\n"); return 1; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!GameCode::Load(argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    const int rounds = argc > 2 ? atoi(argv[2]) : 1000;
    const bool verbose = argc > 3;
    const char* names[5] = { "point A", "point B", "normal", "depth", "contact" };
    int contacts = 0, faults = 0, bad = 0, which[5] = {};
    for (int r = 0; r < rounds; ++r) {
        alignas(16) float box[16], cyl[16], half[4], cs[4] = {}, rec[0x40] = {};
        const float zero[3] = { 0, 0, 0 };
        float p[3];
        for (float& v : p) v = Uni(-1.5f, 1.5f);
        RandomMatrix(box, zero);
        RandomMatrix(cyl, p);
        MaybeAlign(r % 3 ? cyl : box, r);
        for (int i = 0; i < 3; ++i) half[i] = Uni(0.1f, 1.5f);
        half[3] = 0;
        cs[0] = Uni(0.05f, 1.0f);
        cs[1] = Uni(0.05f, 1.5f);
        GameRegs g;
        g.eax = A(box); g.ecx = A(cs); g.ebx = A(cyl); g.edi = A(rec);
        GameRegs out;
        if (!GameCode::Call(0x5b4380, g, { A(half) }, &out)) {
            if (faults++ < 3) printf("round %d: fault at game %08x\n", r, GameCode::Game(GameCode::g_faultAddr));
            continue;
        }
        if (out.eax) ++contacts;
        BodyContact c;
        const bool mine = BoxCylinderContact(half, box, cs[0], cs[1], cyl, c);
        const float* m[4] = { c.pointA, c.pointB, c.normal, &c.depth };
        const int n[4] = { 3, 3, 3, 1 };
        int field = mine == (out.eax != 0) ? -1 : 4;
        if (field < 0 && mine)
            for (int f = 0; f < 4 && field < 0; ++f)
                for (int k = 0; k < n[f]; ++k)
                    if (GameCode::Bits(m[f][k]) != GameCode::Bits(rec[f * 4 + k])) field = f;
        if (field >= 0) {
            ++which[field];
            if (bad++ < 6) {
                printf("round %d: %s (game %u, port %d)\n", r, names[field], out.eax, mine);
                Print("game", rec, rec + 4, rec + 8, rec[12]);
                Print("port", c.pointA, c.pointB, c.normal, c.depth);
            }
        } else if (verbose && out.eax && contacts <= 8) {
            printf("round %d:\n", r);
            Print("game", rec, rec + 4, rec + 8, rec[12]);
        }
    }
    printf("%d rounds: %d contacts, %d faults, %d differ (A %d, B %d, normal %d, depth %d, contact %d)\n", rounds,
           contacts, faults, bad, which[0], which[1], which[2], which[3], which[4]);
    return faults || bad ? 1 : 0;
}
