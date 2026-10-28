// Replays the contact response 0x5c5260 against run 21 (D3PROXY_RESP,
// BOOT_TO_MENU.md section 78; driv3r-re\tools\physics\README.md): per call
// "RS" the arguments, the narrow phase's result ("RSr"), A and B before
// ("RSa" / "RSb"); "RP" the contact record and "RPi" the impulse (only when
// one was applied); "RX" the return value (5 impulse, 6 none) and A / B
// after the push-out ("RXa" / "RXb"). The port (contact.cpp Respond through
// PairResponse) is run on the bodies before and compared field by field.
// A grouped A (the car with hinged panels, "RSag") is replayed with its
// group rebuilt from the member dumps ("RSm" / "RSmg" bodies and records in
// list order, "RSmq" / "RSmqd" the joint values and rates, "RSl" the list;
// BOOT_TO_MENU.md section 80), every member and qd compared afterwards
// ("RXm", "RXmqd"); runs without the joint arrays (before run 24) skip them.
//
//   respreplay <phys.txt> [shown]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../articulated.h"
#include "../contact.h"
#include "../physics.h"
#include "restwords.h"

namespace
{
std::map<std::string, std::map<unsigned, std::vector<uint32_t>>> g_dumps;  // tag -> n -> words after the address
std::map<std::string, std::map<unsigned, std::vector<uint32_t>>> g_lines;  // "RS" / "RX": the fields
// the repeated member dumps of one call, in order, with their addresses
struct Dump
{
    uint32_t addr;
    std::vector<uint32_t> words;
};
std::map<std::string, std::map<unsigned, std::vector<Dump>>> g_multi;
// the props' test calls (D3PROXY_PROPS): the physics step, both sides, result
struct PropCall
{
    unsigned dyn;
    uint32_t ecx, eax, result;
};
std::map<unsigned, PropCall> g_propCalls;
// A prop dropped this pair's contact in this step (0x4e8f70 returned 9).
bool PropDropped(unsigned dyn, uint32_t a, uint32_t b)
{
    for (const auto& p : g_propCalls) {
        const PropCall& c = p.second;
        if (c.dyn == dyn && c.result == 9 && ((c.ecx == a && c.eax == b) || (c.ecx == b && c.eax == a))) return true;
    }
    return false;
}

void Read(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    static char line[1 << 17];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'P' && (line[1] == 'R' || line[1] == 'X') && line[2] == ' ') {
            // D3PROXY_PROPS: "PR n dyn site ecx eax mgr", "PX n result"
            unsigned n = 0, dyn = 0, site = 0, ecx = 0, eax = 0, result = 0;
            if (line[1] == 'R' && sscanf(line + 3, "%u %u %x %x %x", &n, &dyn, &site, &ecx, &eax) == 5)
                g_propCalls[n] = { dyn, ecx, eax, 0 };
            else if (line[1] == 'X' && sscanf(line + 3, "%u %x", &n, &result) == 2 && g_propCalls.count(n))
                g_propCalls[n].result = result;
            continue;
        }
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
            const uint32_t addr = w[0];
            w.erase(w.begin());  // the address
            if (t == "RSm" || t == "RSmg" || t == "RXm" || t == "RXmg" || t == "RPm" || t == "RPmg")
                g_multi[t][n].push_back({ addr, w });
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
    LoadRest(w, b);
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
    if (traced.size() < static_cast<size_t>(off / 4 + count)) {
        printf("  RS %u %s %s: dump too short (%zu words)\n", n, what, name, traced.size());
        return 1;
    }
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
    bad += Field(n, what, "rest", &b.rest[0][0], t, 0x180, 8, worst);
    const int ints[3][2] = { { b.state, 0x1c4 }, { b.restCount, 0x1a0 }, { b.restHold, 0x1a4 } };
    static const char* names[3] = { "state", "restCount", "restHold" };
    for (int i = 0; i < 3; ++i) {
        if (t.size() <= static_cast<size_t>(ints[i][1] / 4)) return bad + 1;
        if (static_cast<uint32_t>(ints[i][0]) == t[ints[i][1] / 4]) continue;
        ++bad;
        worst = worst > 1 ? worst : 1;
        if (g_shown++ < g_show) printf("  RS %u %s %s: port %x trace %x\n", n, what, names[i], ints[i][0], t[ints[i][1] / 4]);
    }
    return bad;
}

struct Tally
{
    int calls = 0, grouped = 0, exact = 0, close = 0, impulseMismatch = 0, props = 0, changed = 0;
    double worst = 0;
};

// A call's group rebuilt from its member dumps: the bodies, the records
// (0x3c0 bytes as words), the parents (record +0x32c names the parent's
// body), q, qd and the toggle (list +8).
struct TracedGroup
{
    Articulated::Group group;
    std::vector<RigidBody> bodies;
    std::vector<std::vector<uint32_t>> recs;
    std::vector<uint32_t> addrs;
};
bool BuildGroup(unsigned n, TracedGroup& tg)
{
    const std::vector<Dump>& ms = g_multi["RSm"][n];
    const std::vector<Dump>& gs = g_multi["RSmg"][n];
    if (ms.empty() || ms.size() != gs.size() || !g_dumps["RSmq"].count(n) || !g_dumps["RSmqd"].count(n) ||
        !g_dumps["RSl"].count(n))
        return false;
    const size_t m = ms.size();
    const std::vector<uint32_t>& q = g_dumps["RSmq"][n];
    const std::vector<uint32_t>& qd = g_dumps["RSmqd"][n];
    if (q.size() < m || qd.size() < m) return false;
    tg.bodies.resize(m);
    tg.recs.resize(m);
    for (size_t k = 0; k < m; ++k) {
        if (ms[k].words.size() < 0x1e0 / 4 || gs[k].words.size() < 0x3c0 / 4) return false;
        LoadBody(ms[k].words, tg.bodies[k]);
        tg.recs[k] = gs[k].words;
        tg.addrs.push_back(ms[k].addr);
    }
    Articulated::Group& g = tg.group;
    for (size_t k = 0; k < m; ++k) {
        Articulated::Group::Member mm;
        mm.body = &tg.bodies[k];
        mm.rec = reinterpret_cast<float*>(tg.recs[k].data());
        const uint32_t parent = tg.recs[k][0x32c / 4];
        for (size_t j = 0; j < m; ++j)
            if (tg.addrs[j] == parent) mm.parent = static_cast<int>(j);
        g.members.push_back(mm);
        g.q.push_back(F(q[k]));
        g.qd.push_back(F(qd[k]));
    }
    g.toggle = static_cast<int>(g_dumps["RSl"][n][2]);
    g.Link();
    return true;
}
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
        if (sa.size() < 0x1e0 / 4 || r.size() < 0x58 / 4 || d.size() < 7) {
            printf("  RS %u: short dumps\n", n);
            continue;
        }
        const bool bodyB = g_dumps["RSb"].count(n) && g_dumps["RSb"][n].size() >= 0x1e0 / 4;
        const bool groupedA = g_dumps["RSag"].count(n) != 0;
        char kind[64];
        snprintf(kind, sizeof(kind), "%08x%s / %s", d[1], groupedA ? " grouped" : "", bodyB ? "body" : "static");
        Tally& t = tally[kind];
        ++t.calls;
        if (g_dumps["RSbg"].count(n)) { ++t.grouped; continue; }
        // the game dropped the contact through a smashable prop (props.cpp,
        // checked by propreplay)
        if (PropDropped(g_lines["RS"][n][0], d[1], d[5])) {
            ++t.props;
            continue;
        }
        // the damage listener changed a group during the call (a panel
        // hinged, the other body grouped): the dumps after no longer match
        // the members before
        if (groupedA && (g_multi["RXm"][n].size() != g_multi["RSm"][n].size() ||
                         (g_dumps["RXl"].count(n) && g_dumps["RSl"].count(n) &&
                          g_dumps["RXl"][n][0] != g_dumps["RSl"][n][0]))) {
            ++t.changed;
            continue;
        }
        TracedGroup tg;
        RigidBody freeA, B;
        RigidBody* pa = &freeA;
        if (groupedA) {
            if (!BuildGroup(n, tg)) { ++t.grouped; continue; }
            pa = nullptr;
            for (size_t k = 0; k < tg.addrs.size(); ++k)
                if (tg.addrs[k] == d[1]) pa = &tg.bodies[k];
            if (!pa) { ++t.grouped; continue; }
        } else {
            LoadBody(sa, freeA);
        }
        RigidBody& A = *pa;
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
        const bool applied = g_dumps["RP"].count(n) != 0 && g_dumps["RP"][n].size() >= 0x100 / 4;
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
            if (groupedA) {
                // every member and the joint rates
                const std::vector<Dump>& after = g_multi["RXm"][n];
                for (size_t k = 0; k < tg.bodies.size() && k < after.size(); ++k) {
                    char what[16];
                    snprintf(what, sizeof(what), "m%zu", k);
                    bad += CompareBody(n, what, tg.bodies[k], after[k].words, worst);
                }
                if (g_dumps["RXmqd"].count(n))
                    bad += Field(n, "group", "qd", tg.group.qd.data(), g_dumps["RXmqd"][n], 0,
                                 static_cast<int>(tg.group.qd.size()), worst);
            } else {
                bad += CompareBody(n, "A", A, g_dumps["RXa"][n], worst);
            }
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
        printf("%-17s %3d calls, %d grouped (skipped), %d dropped by a prop, %d with a group changed by the listener, "
               "%3d exact, %3d within 1e-4, %d impulse / none mismatches, worst %.3g\n",
               k.first.c_str(), t.calls, t.grouped, t.props, t.changed, t.exact, t.close, t.impulseMismatch, t.worst);
        bad += t.calls - t.grouped - t.props - t.changed - t.exact;
    }
    printf("%d of %d replayed calls differ\n", bad, total);
    return bad ? 2 : 0;
}
