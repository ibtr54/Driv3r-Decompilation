// Cylinder / sphere contacts (contact.cpp CylinderSphereContact) against the
// game's own 0x5b7cf0 (gamecode.h) on random pairs, bit for bit. 0x5b7cf0
// takes the contact record in ebx and four stack arguments (cdecl): the
// cylinder's {radius, half height}, the sphere's {radius}, the cylinder's
// matrix (axis row 1, centre row 3) and the sphere's; it returns the record's
// +0x34 (+0x00 point A, +0x10 point B, +0x20 normal, +0x30 depth).
//
//   cylsph <Driv3r.exe> [rounds] [verbose 0/1]
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

// The sphere's centre for each kind of round, in the cylinder's frame (x, y
// along the axis, z), then to the world: 0 anywhere near, 1 on the axis
// line, 2 beside the side, 3 over a cap face, 4 near the rim, 5 on the rim
// circle, 6 / 7 anywhere near with the cylinder upright (identity rotation).
void Place(int kind, float R, float h, float sr, const float* m, float out[3])
{
    float l[3];
    const float a = Uni(0, 6.2831853f);
    const float sign = Uni(0, 1) < 0.5f ? -1.0f : 1.0f;
    switch (kind) {
    case 1:
        l[0] = 0, l[2] = 0, l[1] = Uni(-(h + sr) * 1.2f, (h + sr) * 1.2f);
        break;
    case 2: {
        const float d = Uni(0, (R + sr) * 1.1f);
        l[0] = d * std::cos(a), l[2] = d * std::sin(a), l[1] = Uni(-h, h);
        break;
    }
    case 3: {
        const float d = Uni(0, R);
        l[0] = d * std::cos(a), l[2] = d * std::sin(a), l[1] = sign * (h + Uni(0, sr * 1.1f));
        break;
    }
    case 4: {
        const float d = R + Uni(0, sr), e = Uni(0, sr);
        l[0] = d * std::cos(a), l[2] = d * std::sin(a), l[1] = sign * (h + e);
        break;
    }
    case 5:
        l[0] = R * std::cos(a), l[2] = R * std::sin(a), l[1] = sign * h;
        break;
    default:
        for (int k = 0; k < 3; ++k) l[k] = Uni(-1, 1) * (k == 1 ? h + sr : R + sr) * 1.2f;
        break;
    }
    for (int k = 0; k < 3; ++k) out[k] = l[0] * m[k] + l[1] * m[4 + k] + l[2] * m[8 + k] + m[12 + k];
}

void Print(const char* who, const float* a, const float* b, const float* n, float d)
{
    printf("  %s A (%.9g %.9g %.9g) B (%.9g %.9g %.9g) n (%.9g %.9g %.9g) d %.9g\n", who, a[0], a[1], a[2], b[0], b[1],
           b[2], n[0], n[1], n[2], d);
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: cylsph <Driv3r.exe> [rounds] [verbose 0/1]\n"); return 1; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!GameCode::Load(argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    const int rounds = argc > 2 ? atoi(argv[2]) : 1000;
    const bool verbose = argc > 3 && atoi(argv[3]) != 0;
    const char* names[5] = { "point A", "point B", "normal", "depth", "contact" };
    int onRim = 0;
    int contacts = 0, faults = 0, bad = 0, which[5] = {}, kinds[8] = {}, kindContacts[8] = {};
    for (int r = 0; r < rounds; ++r) {
        const int kind = r % 8;
        alignas(16) float ma[16], mb[16], sa[4] = {}, sb[4] = {}, rec[0x40] = {};
        float pa[3];
        for (float& v : pa) v = Uni(-50.0f, 50.0f);
        RandomMatrix(ma, pa);
        if (kind >= 6) {
            memset(ma, 0, 12 * sizeof(float));
            ma[0] = ma[5] = ma[10] = 1;
        }
        sa[0] = Uni(0.05f, 1.0f);
        sa[1] = Uni(0.05f, 3.0f);
        sb[0] = Uni(0.05f, 1.5f);
        float pb[3];
        Place(kind, sa[0], sa[1], sb[0], ma, pb);
        RandomMatrix(mb, pb);
        GameRegs g;
        g.ebx = A(rec);
        GameRegs out;
        if (!GameCode::Call(0x5b7cf0, g, { A(sa), A(sb), A(ma), A(mb) }, &out)) {
            if (faults++ < 3) printf("round %d: fault at game %08x\n", r, GameCode::Game(GameCode::g_faultAddr));
            continue;
        }
        ++kinds[kind];
        if (out.eax) ++contacts, ++kindContacts[kind];
        // the centre on the rim: depth exactly the sphere's radius
        if (out.eax && GameCode::Bits(rec[12]) == GameCode::Bits(sb[0])) ++onRim;
        BodyContact c;
        const bool mine = CylinderSphereContact(sa, ma, sb[0], mb, c);
        const float* m[4] = { c.pointA, c.pointB, c.normal, &c.depth };
        const int n[4] = { 3, 3, 3, 1 };
        int field = mine == (out.eax != 0) ? -1 : 4;
        if (field < 0 && mine)
            for (int f = 0; f < 4 && field < 0; ++f)
                for (int k = 0; k < n[f]; ++k)
                    if (GameCode::Bits(m[f][k]) != GameCode::Bits(rec[f * 4 + k])) field = f;
        if (field >= 0) {
            ++which[field];
            if (bad++ < 8) {
                printf("round %d (kind %d): %s (game %u, port %d)\n", r, kind, names[field], out.eax, mine);
                Print("game", rec, rec + 4, rec + 8, rec[12]);
                Print("port", c.pointA, c.pointB, c.normal, c.depth);
            }
        } else if (verbose && out.eax && contacts <= 8) {
            printf("round %d (kind %d):\n", r, kind);
            Print("game", rec, rec + 4, rec + 8, rec[12]);
        }
    }
    printf("%d rounds: %d contacts, %d faults, %d differ (A %d, B %d, normal %d, depth %d, contact %d)\n", rounds,
           contacts, faults, bad, which[0], which[1], which[2], which[3], which[4]);
    printf("depth = the sphere's radius (the centre on the rim): %d\n", onRim);
    printf("contacts by kind (near, axis, side, cap, rim, on the rim, upright x2):");
    for (int k = 0; k < 8; ++k) printf(" %d/%d", kindContacts[k], kinds[k]);
    printf("\n");
    return faults || bad ? 1 : 0;
}
