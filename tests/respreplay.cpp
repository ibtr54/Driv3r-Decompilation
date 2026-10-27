// Replays the contact response 0x5c5260 against run 21 (D3PROXY_RESP,
// BOOT_TO_MENU.md section 78; driv3r-re\tools\physics\README.md): per call
// "RS" the arguments, the narrow phase's result ("RSr"), A and B before
// ("RSa" / "RSb"); "RP" the contact record and "RPi" the impulse (only when
// one was applied); "RX" the return value (5 impulse, 6 none) and A / B
// after the push-out ("RXa" / "RXb"). The port (contact.cpp Respond through
// PairResponse) is run on the bodies before and compared field by field.
// Calls with a grouped body are counted but not replayed.
//
//   respreplay <phys.txt> [shown]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../contact.h"
#include "../physics.h"

namespace
{
std::map<std::string, std::map<unsigned, std::vector<uint32_t>>> g_dumps;  // tag -> n -> words after the address
std::map<std::string, std::map<unsigned, std::vector<uint32_t>>> g_lines;  // "RS" / "RX": the fields

void Read(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    static char line[1 << 17];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != 'R') continue;
        char tag[8];
        unsigned n = 0;
        int used = 0;
        if (sscanf(line, "%7s %u%n", tag, &n, &used) < 2) continue;
        const std::string t = tag;
        std::vector<uint32_t> w;
        const char* p = line + used;
        char* end = nullptr;
        for (;;) {
            const unsigned long v = strtoul(p, &end, t == "RS" && w.empty() ? 10 : 16);
            if (end == p) break;
            w.push_back(static_cast<uint32_t>(v));
            p = end;
        }
        if (t == "RS" || t == "RX") {
            g_lines[t][n] = w;
        } else if (!w.empty()) {
            w.erase(w.begin());  // the address
            g_dumps[t][n] = w;
        }
    }
    fclose(f);
}

float F(uint32_t w) { float x; memcpy(&x, &w, 4); return x; }
uint32_t W(float x) { uint32_t w; memcpy(&w, &x, 4); return w; }

void LoadBody(const std::vector<uint32_t>& w, RigidBody& b)
{
    auto f = [&](int off) { return F(w[off / 4]); };
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) b.matrix[r][c] = f(0x10 + r * 16 + c * 4);
    for (int i = 0; i < 4; ++i) {
        b.pos[i] = f(0x80 + 4 * i); b.vel[i] = f(0x90 + 4 * i); b.q[i] = f(0xa0 + 4 * i); b.L[i] = f(0xb0 + 4 * i);
        b.force[i] = f(0xc0 + 4 * i); b.torque[i] = f(0xd0 + 4 * i); b.accel[i] = f(0xe0 + 4 * i);
        b.w[i] = f(0x150 + 4 * i); b.dq[i] = f(0x160 + 4 * i); b.inertia[i] = f(0x170 + 4 * i);
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) { b.R[r][c] = f(0xf0 + r * 16 + c * 4); b.invI[r][c] = f(0x120 + r * 16 + c * 4); }
    for (int i = 0; i < 6; ++i) b.sleep[i] = f(0x1a0 + 4 * i);
    b.mass = f(0x1c0);
    b.state = static_cast<int>(w[0x1c4 / 4]);
    b.pairClass = static_cast<int>(w[0x70 / 4]);
}

int g_shown = 0, g_show = 30;
// Differences of a field; prints the first ones.
int Field(unsigned n, const char* what, const char* name, const float* mine, const std::vector<uint32_t>& traced, int off,
          int count, double& worst)
{
    int bad = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t t = traced[off / 4 + i];
        if (W(mine[i]) == t) continue;
        ++bad;
        const double scale = std::fabs(F(t)) > 1e-3 ? std::fabs(F(t)) : 1e-3;
        const double rel = std::fabs(mine[i] - F(t)) / scale;
        if (rel > worst) worst = rel;
        if (g_shown++ < g_show)
            printf("  RS %u %s %s[%d]: port %08x (%.9g) trace %08x (%.9g)\n", n, what, name, i, W(mine[i]), mine[i], t,
                   F(t));
    }
    return bad;
}
int CompareBody(unsigned n, const char* what, const RigidBody& b, const std::vector<uint32_t>& t, double& worst)
{
    int bad = 0;
    bad += Field(n, what, "pos", b.pos, t, 0x80, 3, worst);
    bad += Field(n, what, "vel", b.vel, t, 0x90, 3, worst);
    bad += Field(n, what, "L", b.L, t, 0xb0, 3, worst);
    bad += Field(n, what, "w", b.w, t, 0x150, 3, worst);
    bad += Field(n, what, "dq", b.dq, t, 0x160, 4, worst);
    bad += Field(n, what, "matrix", &b.matrix[0][0], t, 0x10, 16, worst);
    return bad;
}

struct Tally
{
    int calls = 0, grouped = 0, exact = 0, close = 0, impulseMismatch = 0;
    double worst = 0;
};
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { printf("respreplay <phys.txt> [shown]\n"); return 1; }
    if (argc > 2) g_show = atoi(argv[2]);
    Read(argv[1]);
    if (g_lines["RS"].empty()) { printf("no RS lines (D3PROXY_RESP)\n"); return 1; }
    std::map<std::string, Tally> tally;
    int total = 0;
    for (const auto& rs : g_lines["RS"]) {
        const unsigned n = rs.first;
        if (!g_lines["RX"].count(n) || !g_dumps["RSr"].count(n) || !g_dumps["RSa"].count(n) || !g_dumps["RSd"].count(n))
            continue;
        const std::vector<uint32_t>& d = g_dumps["RSd"][n];
        const std::vector<uint32_t>& r = g_dumps["RSr"][n];
        const std::vector<uint32_t>& sa = g_dumps["RSa"][n];
        const bool bodyB = g_dumps["RSb"].count(n) && g_dumps["RSb"][n].size() >= 0x1e0 / 4;
        char kind[64];
        snprintf(kind, sizeof(kind), "%08x / %s", d[1], bodyB ? "body" : "static");
        Tally& t = tally[kind];
        ++t.calls;
        if (g_dumps["RSag"].count(n) || g_dumps["RSbg"].count(n)) { ++t.grouped; continue; }
        RigidBody A, B;
        LoadBody(sa, A);
        if (bodyB) LoadBody(g_dumps["RSb"][n], B);
        // the narrow phase's result: points relative to the bodies (+0x80;
        // a static object's matrix row 3), normal from B to A, depth; the
        // surfaces as the response ends with them (the contact record +0x90
        // / +0xd0, after the vehicle rename), else as passed in
        BodyContact c;
        for (int i = 0; i < 3; ++i) {
            c.pointA[i] = A.pos[i] + F(r[i]);
            c.pointB[i] = (bodyB ? B.pos[i] : 0.0f) + F(r[4 + i]);
            c.normal[i] = F(r[8 + i]);
        }
        c.depth = F(r[0x30 / 4]);
        const bool applied = g_dumps["RP"].count(n) != 0;
        c.surfaceA = static_cast<int>(applied ? g_dumps["RP"][n][0x90 / 4] : d[2]);
        c.surfaceB = static_cast<int>(applied ? g_dumps["RP"][n][0xd0 / 4] : d[6]);
        float J[3] = {};
        const bool mine = PairResponse(A, bodyB ? &B : nullptr, c, J);
        int bad = 0;
        if (mine != applied) {
            ++t.impulseMismatch;
            ++bad;
            if (g_shown++ < g_show)
                printf("  RS %u (%s): port %s, game %s (ret %u)\n", n, kind, mine ? "impulse" : "none",
                       applied ? "impulse" : "none", g_lines["RX"][n][0]);
        } else if (applied) {
            double worst = 0;
            bad += Field(n, "J", "J", J, g_dumps["RPi"][n], 0, 3, worst);
            bad += CompareBody(n, "A", A, g_dumps["RXa"][n], worst);
            if (bodyB) bad += CompareBody(n, "B", B, g_dumps["RXb"][n], worst);
            if (worst > t.worst) t.worst = worst;
            if (worst <= 1e-4) ++t.close;
            if (worst > 1e-3) printf("  RS %u (%s): off by %.3g\n", n, kind, worst);
        }
        if (!bad) ++t.exact;
        ++total;
    }
    int bad = 0;
    for (const auto& k : tally) {
        const Tally& t = k.second;
        printf("%-17s %3d calls, %d grouped (skipped), %3d exact, %3d within 1e-4, %d impulse / none mismatches, "
               "worst %.3g\n",
               k.first.c_str(), t.calls, t.grouped, t.exact, t.close, t.impulseMismatch, t.worst);
        bad += t.calls - t.grouped - t.exact;
    }
    printf("%d of %d replayed calls differ\n", bad, total);
    return bad ? 2 : 0;
}
