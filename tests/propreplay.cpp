// Replays the smashable props' test 0x4e8f70 against run 25 (D3PROXY_PROPS,
// BOOT_TO_MENU.md section 81): per logged call with a prop set ("PRs"),
// the city instance is found by the GEBI record's position and model
// ("PRg"); the program's type record (city.cpp PropType) is compared with
// the traced type entry ("PRt": +0x10 parts, +0x1c, +0x20, +0x42); the
// momentum of the moving body is recomputed (props.cpp Momentum) from the
// response call of the same physics step (D3PROXY_RESP "RSa": velocity and
// mass) against the stored one (the set's +0x10c in "PXh"); Props::Hit
// against the result ("PX": 9 dropped, 10 solid) and the broken bit after.
//
//   propreplay <phys.txt> <city.d3c>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../city.h"
#include "../props.h"

namespace
{
struct Line
{
    std::vector<std::string> w;
};
std::map<std::string, std::map<unsigned, Line>> g_lines;  // tag -> n -> words after the tag and n
std::vector<std::vector<std::string>> g_rs;               // RS / RSd / RSa in order: tag n words

std::vector<uint8_t> ReadFile(const char* path)
{
    std::vector<uint8_t> d;
    FILE* f = fopen(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d.resize(size_t(n));
    if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}

void Read(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    static char line[1 << 17];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != 'P' && line[0] != 'R') continue;
        std::vector<std::string> w;
        char word[64];
        int used = 0;
        const char* p = line;
        while (sscanf(p, "%63s%n", word, &used) == 1) { w.push_back(word); p += used; }
        if (w.size() < 2) continue;
        if (w[0] == "RS" || w[0] == "RSd" || w[0] == "RSa") { g_rs.push_back(w); continue; }
        if (w[0][0] != 'P') continue;
        Line l;
        l.w.assign(w.begin() + 2, w.end());
        g_lines[w[0]][unsigned(strtoul(w[1].c_str(), nullptr, 10))] = l;
    }
    fclose(f);
}

uint32_t H(const std::string& s) { return uint32_t(strtoul(s.c_str(), nullptr, 16)); }
float F(uint32_t w) { float x; memcpy(&x, &w, 4); return x; }
uint32_t W(float x) { uint32_t w; memcpy(&w, &x, 4); return w; }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { printf("propreplay <phys.txt> <city.d3c>\n"); return 1; }
    Read(argv[1]);
    CityPlacement city;
    if (!city.Load(ReadFile(argv[2]))) { printf("city: %s\n", city.Error().c_str()); return 1; }
    Props props;
    props.Init(city);
    int calls = 0, checked = 0, bad = 0;
    for (const auto& pr : g_lines["PR"]) {
        const unsigned n = pr.first;
        ++calls;
        if (!g_lines["PRs"].count(n) || !g_lines["PRg"].count(n) || !g_lines["PX"].count(n)) continue;
        // "PR" words: dyn site ecx eax mgr; "PRg": addr + 8 words
        const std::vector<std::string>& w = pr.second.w;
        const unsigned dyn = unsigned(strtoul(w[0].c_str(), nullptr, 10));
        const uint32_t ecx = H(w[2]), eax = H(w[3]);
        const std::vector<std::string>& g = g_lines["PRg"][n].w;
        const uint32_t px = H(g[1]), py = H(g[2]), pz = H(g[3]), model = H(g[4]) & 0xffff;
        int inst = -1;
        for (size_t i = 0; i < city.instances.size(); ++i) {
            const CityInstance& in = city.instances[i];
            if (in.model == model && W(in.position[0]) == px && W(in.position[1]) == py && W(in.position[2]) == pz)
                inst = int(i);
        }
        printf("PR %u: model %04x at (%.3f %.3f %.3f): instance %d", n, model, F(px), F(py), F(pz), inst);
        if (inst < 0) { printf(" NOT FOUND\n"); ++bad; continue; }
        printf(" range %d\n", city.instances[size_t(inst)].range);
        // the type record
        if (g_lines["PRt"].count(n)) {
            const std::vector<std::string>& t = g_lines["PRt"][n].w;  // addr + words
            const uint32_t parts = H(t[1 + 0x10 / 4]), shatter = H(t[1 + 0x1c / 4]), thr = H(t[1 + 0x20 / 4]);
            const uint32_t part0 = H(t[1 + 0x40 / 4]) >> 16;
            const CityPlacement::PropType* mine = nullptr;
            for (const CityPlacement::PropType& pt : city.propTypes)
                if (pt.model == model) mine = &pt;
            if (!mine) { printf("  type: none in the program\n"); ++bad; }
            else {
                const bool same = mine->parts == parts && W(mine->shatter) == shatter && W(mine->threshold) == thr &&
                                  mine->firstPartModel == part0;
                printf("  type: parts %u shatter %g threshold %g first part %04x: %s\n", parts, F(shatter), F(thr), part0,
                       same ? "equal" : "DIFFERENT");
                if (!same) ++bad;
            }
        }
        // the momentum: the response call of this step with the same pair
        float momentum = 0;
        const std::vector<std::string>& h = g_lines["PXh"][n].w;  // addr + 9 words from +0xf4
        const uint32_t stored = H(h[1 + (0x10c - 0xf4) / 4]);
        bool haveRs = false;
        for (size_t i = 0; i + 2 < g_rs.size(); ++i) {
            if (g_rs[i][0] != "RS" || strtoul(g_rs[i][2].c_str(), nullptr, 10) != dyn) continue;
            const std::string rn = g_rs[i][1];
            const std::vector<std::string>* d = nullptr;
            const std::vector<std::string>* a = nullptr;
            for (size_t j = i + 1; j < g_rs.size() && j < i + 4; ++j) {
                if (g_rs[j][1] != rn) continue;
                if (g_rs[j][0] == "RSd") d = &g_rs[j];
                if (g_rs[j][0] == "RSa") a = &g_rs[j];
            }
            if (!d || !a || d->size() < 9) continue;
            // RSd words after tag, n, addr: +4 A, +0x14 B
            const uint32_t A = H((*d)[3 + 1]), B = H((*d)[3 + 5]);
            if (!((A == eax && B == ecx) || (A == ecx && B == eax))) continue;
            const std::vector<std::string>& aw = *a;
            float vel[3];
            for (int k = 0; k < 3; ++k) vel[k] = F(H(aw[3 + 0x90 / 4 + k]));
            momentum = Props::Momentum(vel, F(H(aw[3 + 0x1c0 / 4])));
            haveRs = true;
        }
        if (haveRs) {
            printf("  momentum: port %.9g trace %.9g: %s\n", momentum, F(stored),
                   W(momentum) == stored ? "equal" : "DIFFERENT");
            if (W(momentum) != stored) ++bad;
        } else {
            momentum = F(stored);
            printf("  momentum: no response call found, the stored %.9g used\n", momentum);
        }
        const bool dropped = props.Hit(inst, momentum);
        const uint32_t result = H(g_lines["PX"][n].w[0]);
        const bool brokenAfter = g_lines["PX"][n].w.size() > 2 && g_lines["PX"][n].w[2] == "1";
        printf("  result: port %s, game %u; broken after: port %d, game %d\n", dropped ? "dropped (9)" : "solid (10)",
               result, props.Broken(inst), brokenAfter);
        if ((dropped ? 9u : 10u) != result || props.Broken(inst) != brokenAfter) ++bad;
        ++checked;
    }
    printf("%d calls, %d with a prop checked, %d differences\n", calls, checked, bad);
    return bad ? 2 : 0;
}
