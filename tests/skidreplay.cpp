// The skid-mark ribbon's add-point (SkidRibbon::Add = 0x56f3c0) against run
// 30 (BOOT_TO_MENU.md section 84): for each traced call ("SK" with the
// point "SKp", the wheel's velocity "SKv", the normal "SKb" and the
// ribbon's fields before "SKr"), one SkidRibbon per traced ribbon, built
// only by these calls: on, fresh, width and intensity are taken from the
// record (the car tick sets them), the colour row is 6 (the road, the
// game's ground lookup 0x4e9da0 is not ported) and the mood colour 1. The
// fields after ("SXr") and the vertices head - 6 .. head + 1 ("SXv") are
// compared bit by bit.
//   skidreplay <phys.txt>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../skid.h"

namespace {

std::vector<uint32_t> Words(const char* line, int skip)
{
    const char* p = line;
    for (int k = 0; k < skip; ++k) {
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

struct Call
{
    unsigned n = 0;
    std::string ribbon;
    std::vector<uint32_t> p, vel, nrm, before, after;
    std::map<unsigned, std::vector<uint32_t>> verts;
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: skidreplay <phys.txt>\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<Call> calls;
    std::vector<char> buf(1 << 16);
    while (fgets(buf.data(), int(buf.size()), f)) {
        const char* l = buf.data();
        if (!strncmp(l, "SK ", 3)) {
            Call c;
            char rib[16] = {};
            sscanf(l + 3, "%u %*u %15s", &c.n, rib);
            c.ribbon = rib;
            calls.push_back(c);
        } else if (calls.empty() || l[0] != 'S') {
            continue;
        } else if (!strncmp(l, "SKp ", 4)) {
            calls.back().p = Words(l, 3);
        } else if (!strncmp(l, "SKv ", 4)) {
            calls.back().vel = Words(l, 3);
        } else if (!strncmp(l, "SKb ", 4)) {
            calls.back().nrm = Words(l, 3);
        } else if (!strncmp(l, "SKr ", 4)) {
            calls.back().before = Words(l, 3);
        } else if (!strncmp(l, "SXr ", 4)) {
            calls.back().after = Words(l, 3);
        } else if (!strncmp(l, "SXv", 3)) {
            unsigned j = unsigned(strtoul(l + 3, nullptr, 10));
            calls.back().verts[j] = Words(l, 3);
        }
    }
    fclose(f);

    std::map<std::string, std::unique_ptr<SkidRibbon>> ribbons;
    const float mood[3] = { 1, 1, 1 };
    int fieldDiff = 0, vertDiff = 0, checked = 0, shown = 0;
    float worst = 0;
    for (const Call& c : calls) {
        if (c.p.size() < 4 || c.vel.size() < 4 || c.nrm.size() < 4 || c.before.size() < 25 || c.after.size() < 25)
            continue;
        std::unique_ptr<SkidRibbon>& r = ribbons[c.ribbon];
        if (!r) r.reset(new SkidRibbon());
        // the fields the tick sets (+0x12c34 width, +0x12c40 on / fresh, +0x12c64)
        r->width = F(c.before[(0x12c34 - 0x12c04) / 4]);
        const uint32_t flags = c.before[(0x12c40 - 0x12c04) / 4];
        r->on = (flags & 0xff) != 0;
        r->fresh = ((flags >> 8) & 0xff) != 0;
        r->intensity = F(c.before[(0x12c64 - 0x12c04) / 4]);
        float p[4], vel[4], n[4];
        for (int k = 0; k < 4; ++k) p[k] = F(c.p[k]), vel[k] = F(c.vel[k]), n[k] = F(c.nrm[k]);
        r->Add(p, vel, n, 6, mood);
        // the fields after: count, head, last, lastPair, length, dir
        auto W = [&](float x) { uint32_t w; memcpy(&w, &x, 4); return w; };
        std::vector<std::pair<int, uint32_t>> cmp = { { 0x12c04, r->count }, { 0x12c08, r->head },
                                                       { 0x12c3c, W(r->length) } };
        for (int k = 0; k < 4; ++k) {
            cmp.push_back({ 0x12c14 + 4 * k, W(r->last[k]) });
            cmp.push_back({ 0x12c24 + 4 * k, W(r->lastPair[k]) });
            cmp.push_back({ 0x12c54 + 4 * k, W(r->dir[k]) });
        }
        for (auto& q : cmp) {
            const uint32_t want = c.after[(q.first - 0x12c04) / 4];
            if (want != q.second) {
                ++fieldDiff;
                if (shown < 30) {
                    ++shown;
                    printf("call %u %s: +%05x game %08x (%g) port %08x (%g)\n", c.n, c.ribbon.c_str(), q.first, want,
                           F(want), q.second, F(q.second));
                }
            }
        }
        for (auto& kv : c.verts) {
            const uint32_t* mine = reinterpret_cast<const uint32_t*>(&r->v[kv.first]);
            for (size_t k = 0; k < 12 && k < kv.second.size(); ++k) {
                ++checked;
                if (mine[k] != kv.second[k]) {
                    ++vertDiff;
                    float d = F(mine[k]) - F(kv.second[k]);
                    if (d < 0) d = -d;
                    if (d > worst) worst = d;
                    if (shown < 30) {
                        ++shown;
                        printf("call %u %s: vertex %u word %zu game %08x (%g) port %08x (%g)\n", c.n, c.ribbon.c_str(),
                               kv.first, k, kv.second[k], F(kv.second[k]), mine[k], F(mine[k]));
                    }
                }
            }
        }
    }
    printf("%zu calls, %zu ribbons; field differences %d; vertex words %d checked, %d differ (largest %g)\n",
           calls.size(), ribbons.size(), fieldDiff, checked, vertDiff, worst);
    return 0;
}
