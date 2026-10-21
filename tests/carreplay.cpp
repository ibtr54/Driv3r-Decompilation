// carreplay <phys.txt>: replays traced calls of the car dynamics 0x4b6a00
// through carsim.cpp (BOOT_TO_MENU.md section 62). The trace (driv3r-re\tools\
// physics proxy, D3PROXY_PHYS) has, for each call n, the object at the entry
// ("E n obj <0xc10 bytes>") and the exit ("X"), the body ("Eb" / "Xb"), the
// globals 0x8af9f0.. ("Eg" / "Xg"), the wheel ray records and answers at the
// exit ("Xr", 0x8b3458..) and the handling table once ("H"). For each call:
// load the entry, answer the rays with the traced answers, run Dynamics and
// compare the object, the body and the globals with the exit, bit for bit.
// The vehicle model ("M4", argument 3) gives the wheel data and "M4c" the
// vehicle id; the ray records are checked, then the traced ones are used
// (older traces have no model: only the direction and length are checked).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../carsim.h"

static float Fl(unsigned u) { float f; memcpy(&f, &u, 4); return f; }

struct Rec { unsigned addr = 0; std::vector<unsigned> w; };
static std::map<std::string, std::map<unsigned, Rec>> g_recs;

static void LoadBody(const std::vector<unsigned>& w, RigidBody& b)
{
    auto f = [&](int off) { return Fl(w[off / 4]); };
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

// The body fields an impulse changes, at their game offsets.
struct BodyField { const char* name; int off; int count; };
static const BodyField kBodyFields[] = {
    { "vel", 0x90, 3 }, { "L", 0xb0, 3 }, { "accel", 0xe0, 4 }, { "w", 0x150, 3 }, { "dq", 0x160, 4 },
};
static float BodyGet(const RigidBody& b, int off)
{
    if (off >= 0x90 && off < 0xa0) return b.vel[(off - 0x90) / 4];
    if (off >= 0xb0 && off < 0xc0) return b.L[(off - 0xb0) / 4];
    if (off >= 0xe0 && off < 0xf0) return b.accel[(off - 0xe0) / 4];
    if (off >= 0x150 && off < 0x160) return b.w[(off - 0x150) / 4];
    if (off >= 0x160 && off < 0x170) return b.dq[(off - 0x160) / 4];
    return 0;
}

// Answers the rays with the traced records and answers.
class TracedRays : public WheelRayCaster
{
public:
    const std::vector<unsigned>* xr = nullptr;
    std::string notes;
    bool otherBody = false;
    bool checkStart = false;
    void Cast(WheelRay* rays, WheelHit* hits, int count) override
    {
        const std::vector<unsigned>& w = *xr;
        for (int i = 0; i < count; ++i) {
            const unsigned* r = &w[i * 0x2c / 4];
            float start[4], dir[4];
            for (int k = 0; k < 4; ++k) { start[k] = Fl(r[1 + k]); dir[k] = Fl(r[5 + k]); }
            const float len = Fl(r[9]);
            const uint16_t filter = static_cast<uint16_t>(r[10] & 0xffff);
            if (checkStart && memcmp(start, rays[i].start, 12) != 0) {
                char t[160];
                snprintf(t, sizeof(t), " ray%d start %.9g %.9g %.9g / %.9g %.9g %.9g", i, rays[i].start[0],
                         rays[i].start[1], rays[i].start[2], start[0], start[1], start[2]);
                notes += t;
            }
            if (memcmp(dir, rays[i].dir, 12) != 0 || memcmp(&len, &rays[i].length, 4) != 0 || filter != rays[i].filter) {
                char t[160];
                snprintf(t, sizeof(t), " ray%d dir/len %.9g %.9g %.9g %.9g / %.9g %.9g %.9g %.9g", i, rays[i].dir[0],
                         rays[i].dir[1], rays[i].dir[2], rays[i].length, dir[0], dir[1], dir[2], len);
                notes += t;
            }
            memcpy(rays[i].start, start, 16);
            const unsigned* h = &w[(0x210 + i * 0x30) / 4];
            hits[i].type = static_cast<int>(h[0]);
            hits[i].distance = Fl(h[1]);
            hits[i].f8 = Fl(h[2]);
            hits[i].surface = static_cast<int>(h[3]);
            for (int k = 0; k < 4; ++k) hits[i].normal[k] = Fl(h[4 + k]);
            hits[i].body = nullptr;
            if (h[0] == 2 && h[8] != 0) otherBody = true;
        }
    }
};

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: carreplay phys.txt [verbose | create | driver]\n"); return 2; }
    // "driver": CarDriver's steering for "right" from the centre at speed,
    // against the run 8 control dumps ("C n caller c0 c1 ...": c1 at speed
    // above 10, before the low-speed factor applies).
    if (argc > 2 && !strcmp(argv[2], "driver")) {
        static const unsigned kTraced[] = { 0x3e385971, 0x3e8a4115, 0x3eb85571, 0x3ee669cd, 0x3f0a3f14, 0x3f214943,
                                            0x3f385371, 0x3f4f5d9f, 0x3f6667cd, 0x3f7d71fb, 0x3f800000 };
        CarDriver d;
        CarDriverKeys k;
        k.right = true;
        int ok = 0;
        for (unsigned t : kTraced) {
            float c[5];
            bool hb;
            d.Update(k, 6, 17.0f, c, hb);
            unsigned u;
            memcpy(&u, &c[1], 4);
            ok += u == t;
            printf("%08x/%08x ", u, t);
        }
        printf("\ndriver: %d of %zu steps equal\n", ok, sizeof(kTraced) / sizeof(kTraced[0]));
        return ok == int(sizeof(kTraced) / sizeof(kTraced[0])) ? 0 : 1;
    }
    const bool verbose = argc > 2 && argv[2][0] == 'v';
    FILE* f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }
    static char line[1 << 17];
    std::vector<unsigned> table;
    while (fgets(line, sizeof(line), f)) {
        char kind[8] = {};
        int len = 0;
        if (sscanf(line, "%7s %n", kind, &len) != 1) continue;
        const std::string k = kind;
        if (k != "E" && k != "X" && k != "Eb" && k != "Xb" && k != "Xr" && k != "Eg" && k != "Xg" && k != "H" &&
            k != "M4" && k != "M4c") continue;
        char* p = line + len;
        Rec r;
        const unsigned n = strtoul(p, &p, 10);
        r.addr = static_cast<unsigned>(strtoul(p, &p, 16));
        while (*p == ' ') ++p;
        if (!strncmp(p, "unreadable", 10)) continue;
        while (*p && *p != '\n') { r.w.push_back(static_cast<unsigned>(strtoul(p, &p, 16))); while (*p == ' ') ++p; }
        if (k == "H") table = r.w;
        else g_recs[k][n] = std::move(r);
    }
    fclose(f);
    if (table.size() < 0xbe) { fprintf(stderr, "no handling table\n"); return 2; }
    std::vector<float> T(table.size());
    for (size_t i = 0; i < table.size(); ++i) T[i] = Fl(table[i]);
    const bool haveModel = g_recs["M4"].count(0) && g_recs["M4"][0].w.size() >= 0x1400 / 4 && g_recs["M4c"].count(0);
    printf("%s\n", haveModel ? "model in the trace" : "no model in the trace");

    // "create": CarSim::Create from the model at the first call's position,
    // compared with the object at the first traced call (fields that the
    // ticks before it changed are listed too).
    if (argc > 2 && !strcmp(argv[2], "create") && haveModel && g_recs["E"].count(0) && g_recs["Eb"].count(0)) {
        const std::vector<unsigned>& m = g_recs["M4"][0].w;
        auto mf = [&](int off) { return Fl(m[off / 4]); };
        CarSim car;
        car.table = T.data();
        car.vehicleId = static_cast<int>(g_recs["M4c"][0].w[0x14 / 4]);
        for (int i = 0; i < 6; ++i) {
            car.slots[i] = static_cast<int>(m[(0x884 + 4 * i) / 4]);
            for (int c = 0; c < 4; ++c) {
                car.hardpoint[i][c] = mf(0x89c + 16 * i + 4 * c);
                car.wheelOffset[i][c] = mf(0x92c + 16 * i + 4 * c);
            }
            car.radius[i] = mf(0x8fc + 4 * i);
            car.width[i] = mf(0x914 + 4 * i);
        }
        car.altDrop = mf(0x98c);
        RigidBody body, traced;
        LoadBody(g_recs["Eb"][0].w, traced);
        car.Create(&body, traced.pos, -3.14159274f);
        const std::vector<unsigned>& E = g_recs["E"][0].w;
        int n = 0;
        for (int off = 0x10; off < CarSim::kObjectSize; off += 4) {
            unsigned a;
            memcpy(&a, car.obj + off, 4);
            if (a != E[off / 4]) {
                printf(" obj+%x %08x/%08x", off, a, E[off / 4]);
                if (++n % 6 == 0) printf("\n");
            }
        }
        printf("\n%d dwords differ\n", n);
        for (int i = 0; i < 4; ++i)
            printf("q[%d] %08x/%08x  inertia %.9g/%.9g\n", i, *reinterpret_cast<unsigned*>(&body.q[i]),
                   *reinterpret_cast<unsigned*>(&traced.q[i]), body.inertia[i], traced.inertia[i]);
        return 0;
    }

    int exact = 0, differ = 0, skipped = 0;
    std::map<int, int> worst;   // S offset -> count
    for (auto& e : g_recs["E"]) {
        const unsigned n = e.first;
        if (!g_recs["X"].count(n) || !g_recs["Eb"].count(n) || !g_recs["Xb"].count(n) || !g_recs["Xr"].count(n) ||
            !g_recs["Eg"].count(n) || !g_recs["Xg"].count(n)) { ++skipped; continue; }
        const std::vector<unsigned>& E = e.second.w;
        const std::vector<unsigned>& X = g_recs["X"][n].w;
        const std::vector<unsigned>& Eg = g_recs["Eg"][n].w;
        const std::vector<unsigned>& Xg = g_recs["Xg"][n].w;
        RigidBody body;
        LoadBody(g_recs["Eb"][n].w, body);
        RigidBody expect;
        LoadBody(g_recs["Xb"][n].w, expect);

        CarSim car;
        memcpy(car.obj, E.data(), CarSim::kObjectSize);
        car.body = &body;
        car.table = T.data();
        car.slots[0] = car.slots[1] = car.slots[2] = car.slots[3] = 1;
        if (haveModel) {
            const std::vector<unsigned>& m = g_recs["M4"][0].w;
            auto mf = [&](int off) { return Fl(m[off / 4]); };
            car.vehicleId = static_cast<int>(g_recs["M4c"][0].w[0x14 / 4]);
            for (int i = 0; i < 6; ++i) {
                car.slots[i] = static_cast<int>(m[(0x884 + 4 * i) / 4]);
                for (int c = 0; c < 4; ++c) {
                    car.hardpoint[i][c] = mf(0x89c + 16 * i + 4 * c);
                    car.wheelOffset[i][c] = mf(0x92c + 16 * i + 4 * c);
                }
                car.radius[i] = mf(0x8fc + 4 * i);
                car.width[i] = mf(0x914 + 4 * i);
            }
            car.altDrop = mf(0x98c);
        }
        car.setupValue = Fl(Eg[(0x8afa08 - 0x8af9f0) / 4]);
        car.flag8afa04 = static_cast<uint8_t>(Eg[(0x8afa04 - 0x8af9f0) / 4] & 0xff);
        // (written only by some calls: they keep their values otherwise)
        car.g8af9f8 = Fl(Eg[(0x8af9f8 - 0x8af9f0) / 4]);
        car.g8af9fc = Fl(Eg[(0x8af9fc - 0x8af9f0) / 4]);
        TracedRays rays;
        rays.xr = &g_recs["Xr"][n].w;
        rays.checkStart = haveModel;
        car.Dynamics(rays);

        std::string diff = rays.notes;
        if (rays.otherBody) diff += " (a wheel on a body)";
        // The object: every dword but the stale 4th components of the
        // accumulators (S+0x4c, S+0x5c).
        for (int off = 0x10; off < CarSim::kObjectSize; off += 4) {
            const int s = off - 0x10;
            if (s == 0x4c || s == 0x5c) continue;
            unsigned a;
            memcpy(&a, car.obj + off, 4);
            if (a != X[off / 4]) {
                ++worst[s];
                if (diff.size() < 1500) {
                    char t[96];
                    snprintf(t, sizeof(t), " S+%x %08x(%.7g)/%08x(%.7g)", s, a, Fl(a), X[off / 4], Fl(X[off / 4]));
                    diff += t;
                }
            }
        }
        for (const BodyField& fl : kBodyFields)
            for (int k = 0; k < fl.count; ++k) {
                const int off = fl.off + 4 * k;
                const float a = BodyGet(body, off), b = BodyGet(expect, off);
                if (memcmp(&a, &b, 4) != 0) {
                    ++worst[0x10000 + off];
                    char t[96];
                    snprintf(t, sizeof(t), " body.%s[%d] %.9g/%.9g", fl.name, k, a, b);
                    diff += t;
                }
            }
        const float gx[4] = { car.g8af9f8, car.g8af9fc, car.g8afa00, car.g8afa14 };
        const unsigned go[4] = { 0x8af9f8, 0x8af9fc, 0x8afa00, 0x8afa14 };
        for (int i = 0; i < 4; ++i) {
            unsigned a;
            memcpy(&a, &gx[i], 4);
            if (a != Xg[(go[i] - 0x8af9f0) / 4]) {
                ++worst[0x20000 + i];
                char t[64];
                snprintf(t, sizeof(t), " G_%x %.9g/%.9g", go[i], gx[i], Fl(Xg[(go[i] - 0x8af9f0) / 4]));
                diff += t;
            }
        }
        if (diff.empty()) ++exact;
        else {
            ++differ;
            if (verbose || differ <= 5) printf("call %u:%s\n", n, diff.substr(0, 1500).c_str());
        }
    }
    printf("calls: exact %d, differ %d, skipped %d\n", exact, differ, skipped);
    for (auto& w : worst) {
        if (w.first >= 0x20000) printf("  G[%d] differed %d times\n", w.first - 0x20000, w.second);
        else if (w.first >= 0x10000) printf("  body+%x differed %d times\n", w.first - 0x10000, w.second);
        else printf("  S+%x differed %d times\n", w.first, w.second);
    }
    return differ ? 1 : 0;
}
