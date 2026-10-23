// Replays the narrow phase 0x5a3730 on the pairs of runs 16 / 17
// (D3PROXY_NARROW, driv3r-re\tools\physics\README.md): each "NP" record has
// both sides' first 0x90 bytes (type +0, shape set +0xc, matrix +0x10,
// position +0x40 or +0x80), their shapes (0x60 each: half size +0x00,
// matrix +0x10, type byte +0x50, surface +0x51, bit 31 of +0x50 = off) and
// the game's result (point A +0x00, point B +0x10, normal +0x20, depth
// +0x30, contact +0x34, surfaces +0x40 / +0x44; the points relative to the
// sides' positions). The port's box / box test (contact.h BoxBoxContact)
// runs on the same shapes in the game's own space, the pairs as 0x5a3730
// picks them (bounding shapes first, then every shape but the bounds, the
// deepest kept).
//
//   narrowreplay <phys.txt>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
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
    int tested = 0, equal = 0, close = 0, skipped = 0, shown = 0;
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
        for (const Shape& s : sa) boxes = boxes && s.type == 0;
        for (const Shape& s : sb) boxes = boxes && s.type == 0;
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
                if (!BoxBoxContact(sa[i].size, wa, sb[j].size, wb, k)) continue;
                if (!found || best.depth < k.depth) { best = k; found = true; bi = i; bj = j; }
            }
        }
        ++tested;
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
        worst = std::fmax(worst, diff);
        if (diff == 0) ++equal;
        else if (diff < 1e-4f) ++close;
        if (diff >= 1e-4f && shown++ < 20)
            printf("NP %u: shapes %zu / %zu, depth %.5f (trace %.5f), normal (%.4f %.4f %.4f) trace (%.4f %.4f %.4f),\n"
                   "   A (%.4f %.4f %.4f) trace (%.4f %.4f %.4f), B (%.4f %.4f %.4f) trace (%.4f %.4f %.4f)\n",
                   n, bi, bj, best.depth, depth, best.normal[0], best.normal[1], best.normal[2], traced[2][0], traced[2][1],
                   traced[2][2], best.pointA[0], best.pointA[1], best.pointA[2], traced[0][0], traced[0][1], traced[0][2],
                   best.pointB[0], best.pointB[1], best.pointB[2], traced[1][0], traced[1][1], traced[1][2]);
    }
    printf("%d box pairs with a contact: %d equal, %d within 1e-4, worst difference %g (%d pairs with other shapes skipped)\n",
           tested, equal, close, worst, skipped);
    return 0;
}
