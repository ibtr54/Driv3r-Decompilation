// Cylinder / cylinder contacts (contact.cpp CylinderCylinderContact) against
// the game's own 0x5b9f00 (gamecode.h) on random pairs, bit for bit. 0x5b9f00
// takes A's matrix in ecx and its {radius, half height} as the stack
// argument, B's matrix in eax and its sizes in edx (axes row 1, positions
// row 3), the contact record in esi (+0x00 point A, +0x10 point B, +0x20
// normal, +0x30 depth, +0x34 contact).
//
//   cylcyl <Driv3r.exe> [rounds] [verbose 0/1] [dump.txt]
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

// The degenerate branches: every few rounds B's axis equals A's (parallel),
// is turned a half turn (antiparallel), or both stand upright / lie flat.
void Shape(float* a, float* b, int r)
{
    switch (r % 8) {
    case 0:  // parallel
        memcpy(b, a, 12 * sizeof(float));
        break;
    case 1:  // antiparallel
        for (int k = 0; k < 12; ++k) b[k] = -a[k];
        break;
    case 2: {  // upright and lying
        float* ms[2] = { a, b };
        for (int i = 0; i < 2; ++i) {
            float* m = ms[i];
            const float p[3] = { m[12], m[13], m[14] };
            memset(m, 0, 16 * sizeof(float));
            if (i == 0) {
                m[0] = m[5] = m[10] = 1;
            } else {
                m[1] = 1; m[4] = -1; m[10] = 1;
            }
            m[12] = p[0]; m[13] = p[1]; m[14] = p[2]; m[15] = 1;
        }
        break;
    }
    case 3:  // both on the same axis line
        memcpy(b, a, 12 * sizeof(float));
        for (int k = 0; k < 3; ++k) b[12 + k] = a[12 + k] + a[4 + k] * Uni(-2.5f, 2.5f);
        break;
    default:
        break;
    }
}

void Print(const char* who, const float* a, const float* b, const float* n, float d)
{
    printf("  %s A (%.9g %.9g %.9g) B (%.9g %.9g %.9g) n (%.9g %.9g %.9g) d %.9g\n", who, a[0], a[1], a[2], b[0], b[1],
           b[2], n[0], n[1], n[2], d);
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: cylcyl <Driv3r.exe> [rounds] [verbose 0/1] [dump.txt]\n"); return 1; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!GameCode::Load(argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    const int rounds = argc > 2 ? atoi(argv[2]) : 1000;
    const bool verbose = argc > 3 && atoi(argv[3]) != 0;
    // the game's contacts, one line each: sizes A, B, matrices A, B, record
    FILE* dump = argc > 4 ? fopen(argv[4], "w") : nullptr;
    const char* names[5] = { "point A", "point B", "normal", "depth", "contact" };
    int contacts = 0, edges = 0, faults = 0, bad = 0, which[5] = {}, kinds[8] = {}, kindContacts[8] = {};
    for (int r = 0; r < rounds; ++r) {
        alignas(16) float ma[16], mb[16], sa[4] = {}, sb[4] = {}, rec[0x40] = {};
        float pa[3], pb[3];
        for (float& v : pa) v = Uni(-0.5f, 0.5f);
        for (float& v : pb) v = Uni(-1.5f, 1.5f);
        RandomMatrix(ma, pa);
        RandomMatrix(mb, pb);
        Shape(ma, mb, r);
        sa[0] = Uni(0.05f, 1.0f);
        sa[1] = Uni(0.05f, 1.5f);
        sb[0] = Uni(0.05f, 1.0f);
        sb[1] = Uni(0.05f, 1.5f);
        GameRegs g;
        g.ecx = A(ma); g.edx = A(sb); g.eax = A(mb); g.esi = A(rec);
        GameRegs out;
        if (!GameCode::Call(0x5b9f00, g, { A(sa) }, &out)) {
            if (faults++ < 3) printf("round %d: fault at game %08x\n", r, GameCode::Game(GameCode::g_faultAddr));
            continue;
        }
        ++kinds[r % 8];
        if (out.eax) ++contacts, ++kindContacts[r % 8], edges += memcmp(rec, rec + 4, 12) != 0;
        if (dump && out.eax) {
            fprintf(dump, "%d %.9g %.9g %.9g %.9g", r, sa[0], sa[1], sb[0], sb[1]);
            for (float v : ma) fprintf(dump, " %.9g", v);
            for (float v : mb) fprintf(dump, " %.9g", v);
            for (int k = 0; k < 13; ++k) fprintf(dump, " %.9g", rec[k]);
            fprintf(dump, "\n");
        }
        BodyContact c;
        const bool mine = CylinderCylinderContact(sa, ma, sb, mb, c);
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
                printf("round %d (kind %d): %s (game %u, port %d)\n", r, r % 8, names[field], out.eax, mine);
                Print("game", rec, rec + 4, rec + 8, rec[12]);
                Print("port", c.pointA, c.pointB, c.normal, c.depth);
            }
        } else if (verbose && out.eax && contacts <= 8) {
            printf("round %d:\n", r);
            Print("game", rec, rec + 4, rec + 8, rec[12]);
        }
    }
    printf("%d rounds: %d contacts (%d edge), %d faults, %d differ (A %d, B %d, normal %d, depth %d, contact %d)\n", rounds,
           contacts, edges, faults, bad, which[0], which[1], which[2], which[3], which[4]);
    printf("contacts by kind (parallel, antiparallel, upright/lying, same line, random x4):");
    for (int k = 0; k < 8; ++k) printf(" %d/%d", kindContacts[k], kinds[k]);
    printf("\n");
    if (dump) fclose(dump);
    return faults || bad ? 1 : 0;
}
