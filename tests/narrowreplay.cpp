// Replays the narrow phase 0x5a3730 on the pairs of runs 16 / 17
// (D3PROXY_NARROW, driv3r-re\tools\physics\README.md): each "NP" record has
// both sides' first 0x90 bytes (type +0, shape set +0xc, matrix +0x10,
// position +0x40 or +0x80), their shapes (0x60 each: half size +0x00,
// matrix +0x10, type byte +0x50, surface +0x51, bit 31 of +0x50 = off) and
// the game's result (point A +0x00, point B +0x10, normal +0x20, depth
// +0x30, contact +0x34, surfaces +0x40 / +0x44; the points relative to the
// sides' positions). The port's box / box, box / cylinder and cylinder /
// cylinder tests (contact.h BoxBoxContact, BoxCylinderContact,
// CylinderCylinderContact; 0x5a3ba0 swaps the sides of a cylinder / box pair) run on the same shapes in the game's own space, the
// pairs as 0x5a3730 picks them (bounding shapes first, then every shape but
// the bounds, the deepest kept).
//
//   narrowreplay <phys.txt>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../articulated.h"
#include "../contact.h"

namespace
{
std::map<std::string, std::map<unsigned, std::vector<uint32_t>>> g_dumps;
std::map<unsigned, std::vector<uint32_t>> g_np;

void Read(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    static char line[1 << 17];
    while (fgets(line, sizeof(line), f)) {
        char tag[8];
        unsigned n = 0;
        int used = 0;
        if (sscanf(line, "%7s %u%n", tag, &n, &used) < 2) continue;
        const std::string t = tag;
        if (t != "NP" && t.compare(0, 2, "NP") != 0) continue;
        std::vector<uint32_t> w;
        const char* p = line + used;
        char* end = nullptr;
        for (;;) {
            const unsigned long v = strtoul(p, &end, 16);
            if (end == p) break;
            w.push_back(uint32_t(v));
            p = end;
        }
        if (t == "NP") g_np[n] = w;                       // A, B, out
        else if (!w.empty()) g_dumps[t][n].assign(w.begin() + 1, w.end());  // drop the address
    }
    fclose(f);
}

float F(uint32_t w) { float x; memcpy(&x, &w, 4); return x; }

struct Shape
{
    float size[3];
    float local[16];
    int type, surface;
    bool off;
};

// 0x5a3ba0 for boxes, cylinders and cylinder / sphere (types 0 / 1 / 2;
// false for box / sphere and sphere / sphere, which Covered leaves out).
bool PairContact(const Shape& a, const float* wa, const Shape& b, const float* wb, BodyContact& k)
{
    if (a.type == 0 && b.type == 0) return BoxBoxContact(a.size, wa, b.size, wb, k);
    if (a.type == 0 && b.type == 1) return BoxCylinderContact(a.size, wa, b.size[0], b.size[1], wb, k);
    if (a.type == 1 && b.type == 0) {
        if (!BoxCylinderContact(b.size, wb, a.size[0], a.size[1], wa, k)) return false;
        for (int i = 0; i < 3; ++i) {
            k.normal[i] = -k.normal[i];
            std::swap(k.pointA[i], k.pointB[i]);
        }
        return true;
    }
    if (a.type == 1 && b.type == 1) return CylinderCylinderContact(a.size, wa, b.size, wb, k);
    if (a.type == 1 && b.type == 2) return CylinderSphereContact(a.size, wa, b.size[0], wb, k);
    if (a.type == 2 && b.type == 1) {
        if (!CylinderSphereContact(b.size, wb, a.size[0], wa, k)) return false;
        for (int i = 0; i < 3; ++i) {
            k.normal[i] = -k.normal[i];
            std::swap(k.pointA[i], k.pointB[i]);
        }
        return true;
    }
    return false;
}
bool Covered(int a, int b) { return (a <= 1 && b <= 1) || (a == 1 && b == 2) || (a == 2 && b == 1); }

std::vector<Shape> Shapes(const std::vector<uint32_t>& h)
{
    std::vector<Shape> out;
    for (size_t s = 0; (s + 1) * 24 <= h.size(); ++s) {
        const uint32_t* w = &h[s * 24];
        Shape sh;
        for (int k = 0; k < 3; ++k) sh.size[k] = F(w[k]);
        for (int k = 0; k < 16; ++k) sh.local[k] = F(w[4 + k]);
        sh.type = int(w[20] & 0xff);
        sh.surface = int((w[20] >> 8) & 0xff);
        sh.off = (w[20] & 0x80000000u) != 0;
        out.push_back(sh);
    }
    return out;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { printf("narrowreplay <phys.txt>\n"); return 1; }
    Read(argv[1]);
    int tested = 0, equal = 0, close = 0, skipped = 0, shown = 0, withCyl = 0, cylEqual = 0, cylCyl = 0, cylCylEqual = 0;
    int cylSph = 0, cylSphEqual = 0;
    float worst = 0;
    for (const auto& np : g_np) {
        const unsigned n = np.first;
        const std::vector<uint32_t>& r = g_dumps["NPr"][n];
        const std::vector<uint32_t>& a = g_dumps["NPa"][n];
        const std::vector<uint32_t>& b = g_dumps["NPb"][n];
        if (r.size() < 0x58 / 4 || a.size() < 0x90 / 4 || b.size() < 0x90 / 4) continue;
        if (r[0x34 / 4] == 0) continue;  // no contact (the fields are stale then)
        const std::vector<Shape> sa = Shapes(g_dumps["NPah"][n]), sb = Shapes(g_dumps["NPbh"][n]);
        bool boxes = !sa.empty() && !sb.empty();
        for (size_t i = sa.size() > 1 ? 1 : 0; i < sa.size(); ++i)
            for (size_t j = sb.size() > 1 ? 1 : 0; j < sb.size(); ++j)
                if (!sa[i].off && !sb[j].off) boxes = boxes && Covered(sa[i].type, sb[j].type);
        if (!boxes) { ++skipped; continue; }
        float ma[16], mb[16];
        for (int k = 0; k < 16; ++k) { ma[k] = F(a[4 + k]); mb[k] = F(b[4 + k]); }
        const float* pa = reinterpret_cast<const float*>(&a[(a[0] == 0 ? 0x80 : 0x40) / 4]);
        const float* pb = reinterpret_cast<const float*>(&b[(b[0] == 0 ? 0x80 : 0x40) / 4]);
        // pairs as 0x5a3730: from shape 1 when there are several (0 = bounds)
        BodyContact best;
        bool found = false;
        size_t bi = 0, bj = 0;
        for (size_t i = sa.size() > 1 ? 1 : 0; i < sa.size(); ++i) {
            if (sa[i].off) continue;
            float wa[16];
            Articulated::MatMul(sa[i].local, ma, wa);
            for (size_t j = sb.size() > 1 ? 1 : 0; j < sb.size(); ++j) {
                if (sb[j].off) continue;
                float wb[16];
                Articulated::MatMul(sb[j].local, mb, wb);
                BodyContact k;
                if (!PairContact(sa[i], wa, sb[j], wb, k)) continue;
                if (!found || best.depth < k.depth) { best = k; found = true; bi = i; bj = j; }
            }
        }
        ++tested;
        const bool cyl = found && (sa[bi].type == 1 || sb[bj].type == 1);  // the kept pair
        const bool cc = found && sa[bi].type == 1 && sb[bj].type == 1;
        if (cyl) ++withCyl;
        if (cc) ++cylCyl;
        const bool cs = found && (sa[bi].type == 2 || sb[bj].type == 2);
        if (cs) ++cylSph;
        float traced[3][3];
        for (int k = 0; k < 3; ++k) {
            traced[0][k] = F(r[k]) + pa[k];
            traced[1][k] = F(r[4 + k]) + pb[k];
            traced[2][k] = F(r[8 + k]);
        }
        const float depth = F(r[0x30 / 4]);
        if (!found) {
            if (shown++ < 20) printf("NP %u: traced depth %.5f, port no contact\n", n, depth);
            continue;
        }
        float diff = std::fabs(best.depth - depth);
        for (int k = 0; k < 3; ++k) {
            diff = std::fmax(diff, std::fabs(best.pointA[k] - traced[0][k]));
            diff = std::fmax(diff, std::fabs(best.pointB[k] - traced[1][k]));
            diff = std::fmax(diff, std::fabs(best.normal[k] - traced[2][k]));
        }
        // NARROW_BITS=1: the fields as the game keeps them (points relative
        // to the sides' positions, 0x5a3730), bit for bit, per kept pair type
        if (getenv("NARROW_BITS")) {
            auto bits = [](float x) { uint32_t u; memcpy(&u, &x, 4); return u; };
            int badN = 0, badD = 0, badA = 0, badB = 0;
            for (int k = 0; k < 3; ++k) {
                badN += bits(best.normal[k]) != r[8 + k];
                badA += bits(best.pointA[k] - pa[k]) != r[k];
                badB += bits(best.pointB[k] - pb[k]) != r[4 + k];
            }
            badD = bits(best.depth) != r[0x30 / 4];
            static int s2 = 0;
            const bool edgeLike = std::memcmp(best.pointA, best.pointB, sizeof(best.pointA)) != 0;
            if ((badN || badD || badA || badB) && s2++ < 40)
                printf("NP %u %s%s: normal %d depth %d A %d B %d | A %08x %08x %08x trace %08x %08x %08x | n %08x %08x %08x "
                       "trace %08x %08x %08x | d %08x trace %08x\n",
                       n, cyl ? "cyl" : "box", edgeLike ? " edge" : " face", badN, badD, badA, badB,
                       bits(best.pointA[0] - pa[0]), bits(best.pointA[1] - pa[1]), bits(best.pointA[2] - pa[2]), r[0], r[1],
                       r[2], bits(best.normal[0]), bits(best.normal[1]), bits(best.normal[2]), r[8], r[9], r[10],
                       bits(best.depth), r[12]);
        }
        worst = std::fmax(worst, diff);
        if (diff == 0) ++equal, cylEqual += cyl, cylCylEqual += cc, cylSphEqual += cs;
        else if (diff < 1e-4f) ++close;
        if (diff >= 1e-4f && shown++ < 20)
            printf("NP %u: shapes %zu / %zu, depth %.5f (trace %.5f), normal (%.4f %.4f %.4f) trace (%.4f %.4f %.4f),\n"
                   "   A (%.4f %.4f %.4f) trace (%.4f %.4f %.4f), B (%.4f %.4f %.4f) trace (%.4f %.4f %.4f)\n",
                   n, bi, bj, best.depth, depth, best.normal[0], best.normal[1], best.normal[2], traced[2][0], traced[2][1],
                   traced[2][2], best.pointA[0], best.pointA[1], best.pointA[2], traced[0][0], traced[0][1], traced[0][2],
                   best.pointB[0], best.pointB[1], best.pointB[2], traced[1][0], traced[1][1], traced[1][2]);
    }
    printf("%d box / cylinder pairs with a contact (%d kept a box / cylinder pair, %d of them equal): %d equal, %d within 1e-4, "
           "worst difference %g (%d pairs with box / sphere or sphere / sphere skipped)\n",
           tested, withCyl, cylEqual, equal, close, worst, skipped);
    printf("cylinder / cylinder kept: %d, %d equal\n", cylCyl, cylCylEqual);
    printf("cylinder / sphere kept: %d, %d equal\n", cylSph, cylSphEqual);
    return 0;
}
