// Replays a part's tear-off against run 20 (D3PROXY_TEAR, BOOT_TO_MENU.md
// section 76; driv3r-re\tools\physics\README.md): the bonnet's body leaving
// the car's group (0x5acea0 at 0x5ad77d: "GX" with the body and its group
// record before, "GXB" after), the group's end with the car leaving it
// (0x5acea0 at 0x5aad18), the loose part's motion of 0x4f98b0 state 3
// ("TE" before: the slot's +0x1f80 / +0x1f90 in "TEs"; "TXb" after) and the
// bonnet timer's push (0x5b11c0 at 0x4fc25a: "TI" J, "TIb" after).
//
//   tearreplay <phys.txt>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "../articulated.h"
#include "../contact.h"
#include "../panels.h"
#include "../physics.h"

namespace
{
std::map<std::string, std::map<unsigned, std::vector<uint32_t>>> g_dumps;  // tag -> n -> words after the address
std::map<std::string, std::map<unsigned, std::vector<std::string>>> g_lines;  // "GX" / "TE" / "TI": the fields

void Read(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    static char line[1 << 17];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != 'G' && line[0] != 'T' && line[0] != 'D') continue;
        char tag[8];
        unsigned n = 0;
        int used = 0;
        if (sscanf(line, "%7s %u%n", tag, &n, &used) < 2) continue;
        const std::string t = tag;
        if (t == "GX" || t == "TE" || t == "TI" || t == "DE") {
            std::vector<std::string> w;
            char word[32];
            int u = 0;
            const char* p = line + used;
            while (sscanf(p, "%31s%n", word, &u) == 1) { w.push_back(word); p += u; }
            g_lines[t][n] = w;
            continue;
        }
        unsigned addr = 0;
        int u = 0;
        if (sscanf(line + used, "%x%n", &addr, &u) < 1) continue;
        std::vector<uint32_t> w;
        const char* p = line + used + u;
        char* end = nullptr;
        for (;;) {
            const unsigned long v = strtoul(p, &end, 16);
            if (end == p) break;
            w.push_back(static_cast<uint32_t>(v));
            p = end;
        }
        g_dumps[t][n] = w;
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
}

int g_bad = 0, g_shown = 0;
void Field(const char* what, const char* name, const float* mine, const std::vector<uint32_t>& traced, int off, int n)
{
    for (int i = 0; i < n; ++i) {
        const uint32_t t = traced[off / 4 + i];
        if (W(mine[i]) == t) continue;
        ++g_bad;
        if (g_shown++ < 40)
            printf("  %s %s[%d]: port %08x (%.9g) trace %08x (%.9g)\n", what, name, i, W(mine[i]), mine[i], t, F(t));
    }
}
// The fields a free body's state is made of.
int CompareBody(const char* what, const RigidBody& b, const std::vector<uint32_t>& t)
{
    const int before = g_bad;
    Field(what, "pos", b.pos, t, 0x80, 3);
    Field(what, "vel", b.vel, t, 0x90, 3);
    Field(what, "q", b.q, t, 0xa0, 4);
    Field(what, "L", b.L, t, 0xb0, 3);
    Field(what, "R", &b.R[0][0], t, 0xf0, 12);
    Field(what, "invI", &b.invI[0][0], t, 0x120, 12);
    Field(what, "w", b.w, t, 0x150, 3);
    Field(what, "dq", b.dq, t, 0x160, 4);
    if (static_cast<uint32_t>(b.state) != t[0x1c4 / 4]) {
        ++g_bad;
        printf("  %s state: port %x trace %x\n", what, b.state, t[0x1c4 / 4]);
    }
    const int bad = g_bad - before;
    printf("%s: %s\n", what, bad ? "DIFFERENT" : "equal");
    return bad;
}

// 0x5acea0 on a traced body and record: a group of one member.
void Leave(const std::vector<uint32_t>& body, const std::vector<uint32_t>& rec, RigidBody& b)
{
    LoadBody(body, b);
    std::vector<float> r(0x3c0 / 4, 0.0f);
    memcpy(r.data(), rec.data(), std::min<size_t>(rec.size(), r.size()) * 4);
    Articulated::Group g;
    Articulated::Group::Member m;
    m.body = &b;
    m.rec = r.data();
    g.members.push_back(m);
    g.q.push_back(0);
    g.qd.push_back(0);
    g.Link();
    g.Leave(0);
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { printf("tearreplay <phys.txt>\n"); return 1; }
    Read(argv[1]);
    if (g_lines["TE"].empty()) { printf("no TE lines (D3PROXY_TEAR)\n"); return 1; }
    // every member leaving (GX n): the port's body against the traced one
    std::map<unsigned, RigidBody> left;
    for (const auto& gx : g_lines["GX"]) {
        const unsigned n = gx.first;
        if (!g_dumps["GXb"].count(n) || !g_dumps["GXg"].count(n) || !g_dumps["GXB"].count(n)) continue;
        RigidBody b;
        Leave(g_dumps["GXb"][n], g_dumps["GXg"][n], b);
        char what[64];
        snprintf(what, sizeof(what), "GX %u (site %s, joint type %u)", n, gx.second[0].c_str(),
                 g_dumps["GXg"][n][0x330 / 4]);
        CompareBody(what, b, g_dumps["GXB"][n]);
        left[n] = b;
    }
    // the tear-off: the part's body after it left (GX 0), the car after the
    // group's end (GX 1), the slot's +0x1f80 / +0x1f90 from "TEs" (+0x1f60..)
    for (const auto& te : g_lines["TE"]) {
        const unsigned n = te.first;
        if (!g_dumps["TEs"].count(n) || !g_dumps["TXb"].count(n) || left.size() < 2) continue;
        RigidBody panel = left.begin()->second, car = std::next(left.begin())->second;
        const std::vector<uint32_t>& s = g_dumps["TEs"][n];
        float prevVel[4], prevW[4];
        for (int i = 0; i < 4; ++i) { prevVel[i] = F(s[(0x20 / 4) + i]); prevW[i] = F(s[(0x30 / 4) + i]); }
        TornPanelMotion(panel, car, prevVel, prevW);
        char what[64];
        snprintf(what, sizeof(what), "TE %u (state 3 at %s): the part's motion", n, te.second[0].c_str());
        CompareBody(what, panel, g_dumps["TXb"][n]);
        // the timer's push
        if (g_lines["TI"].count(n) && g_dumps["TIb"].count(n) && g_dumps["TXc"].count(n)) {
            RigidBody c2;
            LoadBody(g_dumps["TXc"][n], c2);
            RigidBody p2;
            LoadBody(g_dumps["TXb"][n], p2);
            TornPanelKick(p2, c2);
            const std::vector<std::string>& ti = g_lines["TI"][n];  // dyn body J(4) point(4)
            std::vector<uint32_t> J;
            for (int i = 2; i < 5 && i < static_cast<int>(ti.size()); ++i)
                J.push_back(static_cast<uint32_t>(strtoul(ti[i].c_str(), nullptr, 16)));
            float mine[3];
            for (int k = 0; k < 3; ++k) mine[k] = p2.mass * (-0.375f * c2.vel[k]);
            if (J.size() == 3) Field("TI", "J", mine, J, 0, 3);
            CompareBody("TI: the push", p2, g_dumps["TIb"][n]);
        }
    }
    // The car's contacts with the loose part afterwards (the damage
    // listener's "DE" calls with B = the torn body; D3PROXY_DAMAGE): the
    // two-body response (contact.cpp Respond with B moving) against the
    // traced impulse. The listener runs after the impulse, so the bodies'
    // velocities are taken back by J (v - J / m, L - r x J): a rough check
    // only. Run 20 (2026-10-23): far off for most calls (the listener's
    // bodies are not the response's inputs: other contacts of the step,
    // the car's group before the tear-off); the response 0x5c5260 needs its
    // own hook to be checked (the static case is not trace-checked either;
    // the first car contact of run 20, the car still free, is within 0.1%).
    if (!g_lines["GX"].empty()) {
        const uint32_t torn = static_cast<uint32_t>(strtoul(g_lines["GX"].begin()->second[2].c_str(), nullptr, 16));
        int n = 0;
        double worst = 0, sum = 0;
        for (const auto& de : g_lines["DE"]) {
            const std::vector<std::string>& a = de.second;  // "ecx" X A B contact impulse
            if (a.size() < 6 || static_cast<uint32_t>(strtoul(a[3].c_str(), nullptr, 16)) != torn) continue;
            const unsigned k = de.first;
            if (!g_dumps["DEc"].count(k) || !g_dumps["DEi"].count(k) || !g_dumps["DEb1"].count(k) ||
                !g_dumps["DEb2"].count(k))
                continue;
            const std::vector<uint32_t>& c = g_dumps["DEc"][k];
            const std::vector<uint32_t>& ji = g_dumps["DEi"][k];
            RigidBody A, B;
            LoadBody(g_dumps["DEb1"][k], A);
            LoadBody(g_dumps["DEb2"][k], B);
            BodyContact bc;
            float rows[3][4];
            for (int r = 0; r < 3; ++r)
                for (int j = 0; j < 4; ++j) rows[r][j] = F(c[(0x30 + 16 * r) / 4 + j]);
            const float la[3] = { F(c[0x80 / 4]), F(c[0x84 / 4]), F(c[0x88 / 4]) };
            const float lb[3] = { F(c[0xc0 / 4]), F(c[0xc4 / 4]), F(c[0xc8 / 4]) };
            float ta[3], tb[3];
            VecMat3(la, rows, ta);
            VecMat3(lb, rows, tb);
            for (int i = 0; i < 3; ++i) {
                bc.pointA[i] = A.pos[i] + ta[i];
                bc.pointB[i] = B.pos[i] + tb[i];
                bc.normal[i] = F(c[0x20 / 4 + i]);
            }
            bc.depth = 0;
            bc.surfaceA = static_cast<int>(c[0x90 / 4]);
            bc.surfaceB = static_cast<int>(c[0xd0 / 4]);
            const float J[3] = { F(ji[0]), F(ji[1]), F(ji[2]) };
            // back to before the impulse
            float ra[3], rb[3], aa[3], ab[3];
            for (int i = 0; i < 3; ++i) { ra[i] = bc.pointA[i] - A.pos[i]; rb[i] = bc.pointB[i] - B.pos[i]; }
            aa[0] = ra[1] * J[2] - ra[2] * J[1]; aa[1] = ra[2] * J[0] - ra[0] * J[2]; aa[2] = ra[0] * J[1] - ra[1] * J[0];
            ab[0] = rb[1] * J[2] - rb[2] * J[1]; ab[1] = rb[2] * J[0] - rb[0] * J[2]; ab[2] = rb[0] * J[1] - rb[1] * J[0];
            for (int i = 0; i < 3; ++i) {
                A.vel[i] -= J[i] / A.mass;
                B.vel[i] += J[i] / B.mass;
                A.L[i] -= aa[i];
                B.L[i] += ab[i];
            }
            A.UpdateSpin();
            B.UpdateSpin();
            float mine[3];
            if (!PairResponse(A, B, bc, mine)) {
                if (n < 8) printf("  DE %u: the port answers no impulse (traced |J| %.4f)\n", k,
                                  std::sqrt(J[0] * J[0] + J[1] * J[1] + J[2] * J[2]));
                ++n;
                worst = 1;
                continue;
            }
            double d = 0, l = 0;
            for (int i = 0; i < 3; ++i) { d += (mine[i] - J[i]) * (mine[i] - J[i]); l += J[i] * J[i]; }
            const double rel = l > 0 ? std::sqrt(d / l) : 0;
            if (rel > worst) worst = rel;
            sum += rel;
            if (n < 8) printf("  DE %u (surfaces %d / %d): J port (%.4f %.4f %.4f) trace (%.4f %.4f %.4f), %.2g off\n", k,
                              bc.surfaceA, bc.surfaceB, mine[0], mine[1], mine[2], J[0], J[1], J[2], rel);
            ++n;
        }
        if (n) printf("car vs the loose part: %d impulses, relative difference mean %.3g, worst %.3g\n", n, sum / n, worst);
    }
    printf("%d differences\n", g_bad);
    return g_bad ? 2 : 0;
}
