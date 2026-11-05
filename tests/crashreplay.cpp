// The end-to-end crash replay (BOOT_TO_MENU.md section 94): the program's
// whole car pipeline (carworld.h: the slot's pre-tick, the car's tick and
// controls, the contacts with the city and the props, the slot's contact
// listener with its damage, hinged and torn-off parts, loose wheels, the
// group's solver, the integration) runs from the car's creation with the
// traced controls, one CarWorld::Step per 1/120 s tick, as main.cpp
// StepCar does. At every tick whose 25 Hz block runs (the ticks the physics
// proxy dumps as "K n" / "Kb n" at the car tick's entry) the car's body is
// compared with the original's: position, velocity, rotation, spin.
//
//   crashreplay <phys.txt> <city.d3c> <Miami.vvs> [calls]
//
// CRASH_RELOAD=1: each 25 Hz window starts from the traced state (the car
// object and the body from K n / Kb n), so every line is one window's error
// alone; windows in which the original's car is in a group (hinged panels:
// "RSag" in the window) are marked, the program's group is its own.
// CRASH_UPVH=1: the old hardpoints. CRASH_ALL=1: every call printed. CRASH_EVENTS=0: no event lines.
// CRASH_OBJ=x,z,r: the static objects near (x, z) with their shapes as bits.
// CRASH_SHAPES=t: the car's shapes before tick t (section 97).
// CRASH_J=1: every applied impulse, its contact and the body after it as bits
// (BOOT_TO_MENU.md section 95; not with CRASH_WHEELFAST). CRASH_M=n: the
// body's matrix and position as bits before each tick of the window after
// call n (against a traced "NPa"; section 96).
// The proxy's forcing switches of the traced run must be given again:
// CRASH_TEAR=q,t (D3PROXY_TEARFAST: the bonnet timer's limits; tracetear.ps1's
// default -Fast "-10,1", runs 19..25) and CRASH_WHEELFAST=x
// (D3PROXY_WHEELFAST: the six wheels' damage set to x at the first damage
// call; run 36: 1.4).
//
// Event lines: the program's damage events ("Car damage", "Wheel ... came
// off", hits) with the call they happened in, and the traced ones the trace
// can date: the contact responses per call ("RS n call": with a group when
// "RSag" follows), the tear-off ("TE"), the group's end ("GX").
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "../carworld.h"
#include "../city.h"
#include "../collision.h"
#include "../props.h"
#include "../vehicle.h"
#include "restwords.h"

namespace {

float Fl(unsigned u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

std::vector<uint8_t> ReadAll(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Rec
{
    unsigned addr = 0;
    std::vector<unsigned> w;
};
std::map<std::string, std::map<unsigned, Rec>> g_recs;

void LoadBody(const std::vector<unsigned>& w, RigidBody& b)
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
    LoadRest(w, b);
    b.mass = f(0x1c0);
    b.state = static_cast<int>(w[0x1c4 / 4]);
}

int g_call = 0;       // the 25 Hz call the program is in
int g_tick = 0;       // the 1/120 s tick
bool g_events = true;
void EventLog(const char* fmt, ...)
{
    if (!g_events) return;
    // only the damage events (the contact / group / smoke lines are many)
    if (strncmp(fmt, "Car damage", 10) != 0 && strncmp(fmt, "Wheel ", 6) != 0 && strncmp(fmt, "Car hit", 7) != 0 &&
        strncmp(fmt, "Loose part", 10) != 0 && strncmp(fmt, "Hinged part", 11) != 0)
        return;
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    printf("      [program, call %d] %s\n", g_call, buf);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: crashreplay phys.txt city.d3c Miami.vvs [calls]\n");
        return 2;
    }
    const int maxCalls = argc > 4 ? atoi(argv[4]) : 100000;
    const bool reload = getenv("CRASH_RELOAD") != nullptr;
    const bool all = getenv("CRASH_ALL") != nullptr;
    g_events = !(getenv("CRASH_EVENTS") && getenv("CRASH_EVENTS")[0] == '0');

    // the trace: the 25 Hz states, the controls, the table, the model and
    // the datable events
    std::vector<unsigned> table;
    std::map<unsigned, int> rsPerCall, rsGroupPerCall;  // contact responses per 25 Hz call
    std::vector<std::pair<unsigned, std::string>> traced;  // (call, text)
    {
        FILE* f = fopen(argv[1], "r");
        if (!f) { perror(argv[1]); return 2; }
        static char line[1 << 17];
        unsigned lastRsCall = 0;
        while (fgets(line, sizeof(line), f)) {
            char kind[8] = {};
            int len = 0;
            if (sscanf(line, "%7s %n", kind, &len) != 1) continue;
            const std::string k = kind;
            if (k == "RS") {
                unsigned n = 0, call = 0;
                sscanf(line + len, "%u %u", &n, &call);
                ++rsPerCall[call];
                lastRsCall = call;
                continue;
            }
            if (k == "RSag") {
                ++rsGroupPerCall[lastRsCall];
                continue;
            }
            if (k == "TE" || k == "GX") {
                unsigned n = 0, a = 0, call = 0;
                if (k == "TE") sscanf(line + len, "%u %x %u", &n, &a, &call);
                else sscanf(line + len, "%u %x %u", &n, &a, &call);
                std::string t(line);
                while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
                traced.push_back({ call, t.substr(0, 60) });
                continue;
            }
            if (k != "K" && k != "Kb" && k != "Eb" && k != "H" && k != "M4" && k != "M4c" && k != "C") continue;
            char* p = line + len;
            Rec r;
            const unsigned n = strtoul(p, &p, 10);
            r.addr = static_cast<unsigned>(strtoul(p, &p, 16));
            while (*p == ' ') ++p;
            if (!strncmp(p, "unreadable", 10)) continue;
            while (*p && *p != '\n') {
                r.w.push_back(static_cast<unsigned>(strtoul(p, &p, 16)));
                while (*p == ' ') ++p;
            }
            if (k == "H") table = r.w;
            else g_recs[k][n] = std::move(r);
        }
        fclose(f);
    }
    if (table.size() < 0xbe || !g_recs["M4"].count(0) || !g_recs["M4c"].count(0)) {
        fprintf(stderr, "the trace has no handling table or model\n");
        return 2;
    }
    std::vector<float> T(table.size());
    for (size_t i = 0; i < table.size(); ++i) T[i] = Fl(table[i]);
    const std::vector<unsigned>& m = g_recs["M4"][0].w;
    const int id = static_cast<int>(g_recs["M4c"][0].w[0x14 / 4]);
    const float wheelOffset = Fl(m[(0x92c + 4) / 4]);  // the model's +0x92c (slot 0, y): 0x511780's offset
    // the states to compare: K / Kb (car tick entry, 25 Hz ticks), else Eb
    const bool haveK = !g_recs["Kb"].empty();
    const auto& S = haveK ? g_recs["Kb"] : g_recs["Eb"];
    printf("vehicle %d, wheel offset %.7f; %zu traced 25 Hz states (%s), %zu control ticks, %zu contact responses\n", id,
           wheelOffset, S.size(), haveK ? "K" : "Eb", g_recs["C"].size(), rsPerCall.size());

    CityPlacement city;
    if (!city.Load(ReadAll(argv[2]))) {
        printf("city: %s\n", city.Error().c_str());
        return 1;
    }
    CollisionWorld world;
    world.Build(city);
    Props props;
    props.Init(city);
    std::vector<VehicleData> vehicles;
    if (!ReadVehicles(ReadAll(argv[3]), vehicles)) return 1;
    const VehicleData* v = nullptr;
    for (const VehicleData& x : vehicles)
        if (x.id == id) v = &x;
    if (!v) { printf("vehicle %d not in %s\n", id, argv[3]); return 1; }

    // the car as main.cpp InitCar makes it, at the traced creation (start
    // point 0: section 61's pose)
    static CarWorld cw;  // (large)
    cw.collision = &world;
    cw.ground = &city.ground;
    cw.props = &props;
    cw.log = &EventLog;
    const float pos[3] = { 1988.15f, 0.487739f, -1081.6f };
    cw.Init(*v, id, T.data(), wheelOffset, pos, -3.14159274f);
    printf("%zu shapes, %zu damage records, %zu wheels\n", cw.shapes.size(), cw.damage.records.size(), v->wheels.size());
    if (const char* e = getenv("CRASH_TEAR")) {
        sscanf(e, "%f,%f", &cw.damage.tearQ, &cw.damage.tearTime);
        printf("CRASH_TEAR: the bonnet timer's limits q %g, %g s\n", cw.damage.tearQ, cw.damage.tearTime);
    }
    // CRASH_J=1: every applied impulse with its contact (bits)
    if (getenv("CRASH_J") && !getenv("CRASH_WHEELFAST")) {
        cw.contacts.onImpulse = [](const BodyContact& c, const float J[3]) {
            auto b = [](float x) { unsigned u; memcpy(&u, &x, 4); return u; };
            printf("      [program, call %d tick %d] J %08x %08x %08x n %08x %08x %08x d %08x pA %.7f %.7f %.7f body %08x %08x %08x v %08x %08x %08x\n", g_call, g_tick,
                   b(J[0]), b(J[1]), b(J[2]), b(c.normal[0]), b(c.normal[1]), b(c.normal[2]), b(c.depth), c.pointA[0], c.pointA[1],
                   c.pointA[2], b(cw.body.pos[0]), b(cw.body.pos[1]), b(cw.body.pos[2]), b(cw.body.vel[0]), b(cw.body.vel[1]), b(cw.body.vel[2]));
            cw.Impact(c, J);
        };
    }
    static float wheelFast = -1;
    if (const char* e = getenv("CRASH_WHEELFAST")) {
        wheelFast = float(atof(e));
        printf("CRASH_WHEELFAST: the wheels' damage %g at the first damage call\n", wheelFast);
        cw.contacts.onImpulse = [](const BodyContact& c, const float J[3]) {
            if (wheelFast >= 0) {
                for (int k = 0; k < 6; ++k) cw.damage.wheels[k].damage = wheelFast;
                wheelFast = -1;
            }
            if (getenv("CRASH_J")) {
                auto b = [](float x) { unsigned u; memcpy(&u, &x, 4); return u; };
                printf("      [program, call %d tick %d] J %08x %08x %08x n %08x %08x %08x d %08x\n", g_call, g_tick, b(J[0]),
                       b(J[1]), b(J[2]), b(c.normal[0]), b(c.normal[1]), b(c.normal[2]), b(c.depth));
                // the point relative to each group member (the traced DEc
                // +0x60 / +0x70 are relative points)
                const Articulated::Group& g = cw.group.group;
                for (size_t k = 0; k < g.members.size(); ++k) {
                    const RigidBody& r = *g.members[k].body;
                    printf("        pA - m%zu (%.7f %.7f %.7f) pB - m%zu (%.7f %.7f %.7f)\n", k, c.pointA[0] - r.pos[0],
                           c.pointA[1] - r.pos[1], c.pointA[2] - r.pos[2], k, c.pointB[0] - r.pos[0], c.pointB[1] - r.pos[1],
                           c.pointB[2] - r.pos[2]);
                }
            }
            cw.Impact(c, J);
        };
    }
    // the program's wheel data (UPVH, Init) against the traced model's
    {
        int diff = 0;
        for (int i = 0; i < 6; ++i) {
            auto mf = [&](int off) { return Fl(m[off / 4]); };
            const int slot = static_cast<int>(m[(0x884 + 4 * i) / 4]);
            for (int c = 0; c < 4; ++c) {
                const float a = cw.car.hardpoint[i][c], b = mf(0x89c + 16 * i + 4 * c);
                const float o = cw.car.wheelOffset[i][c], ob = mf(0x92c + 16 * i + 4 * c);
                if (memcmp(&a, &b, 4) || memcmp(&o, &ob, 4)) {
                    ++diff;
                    printf("  slot %d (model %d / program %d) [%d]: hardpoint %.9g / %.9g, offset %.9g / %.9g\n", i, slot,
                           cw.car.slots[i], c, a, b, o, ob);
                }
            }
            if (memcmp(&cw.car.radius[i], &m[(0x8fc + 4 * i) / 4], 4) || memcmp(&cw.car.width[i], &m[(0x914 + 4 * i) / 4], 4)) {
                ++diff;
                printf("  slot %d: radius %.9g / %.9g, width %.9g / %.9g\n", i, cw.car.radius[i], mf(0x8fc + 4 * i),
                       cw.car.width[i], mf(0x914 + 4 * i));
            }
        }
        printf("wheel data: %d differences to the traced model (program / game)\n", diff);
        // CRASH_UPVH=1: the UPVH hardpoints as the program had them before
        // section 94 (no move by the width), to show the difference
        if (getenv("CRASH_UPVH")) {
            for (const VehicleWheel& w : v->wheels)
                if (w.slot >= 0 && w.slot <= 5)
                    for (int k = 0; k < 3; ++k) cw.car.hardpoint[w.slot][k] = w.hardpoint[k];
            RigidBody b0 = cw.body;
            cw.car.Create(&cw.body, pos, -3.14159274f);
            cw.car.SetAutomatic(true);
            cw.body.group = b0.group;
            cw.body.member = b0.member;
            printf("CRASH_UPVH: the UPVH hardpoints (the program before section 94)\n");
        }
        // CRASH_MODEL=1: the traced model's wheel data instead (carreplay settle's)
        if (getenv("CRASH_MODEL")) {
            auto mf = [&](int off) { return Fl(m[off / 4]); };
            CarSim& car = cw.car;
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
            RigidBody b0 = cw.body;
            car.Create(&cw.body, pos, -3.14159274f);
            car.SetAutomatic(true);
            cw.body.group = b0.group;
            cw.body.member = b0.member;
            printf("CRASH_MODEL: the traced model's wheel data\n");
        }
    }

    // CRASH_OBJ=x,z,r: the program's static objects whose box centre lies
    // within r of (x, z), every shape's type, half size and world matrix as
    // bits (to set against a traced "NPbh" through its object's "NPb";
    // BOOT_TO_MENU.md section 97)
    if (const char* e = getenv("CRASH_OBJ")) {
        float x = 0, z = 0, r = 1;
        sscanf(e, "%f,%f,%f", &x, &z, &r);
        const float lo[3] = { x - r, -1000, z - r }, hi[3] = { x + r, 1000, z + r };
        std::vector<uint32_t> near;
        world.Overlapping(lo, hi, near);
        auto b = [](float v) { unsigned u; memcpy(&u, &v, 4); return u; };
        for (uint32_t o : near) {
            const CollisionWorld::Object& ob = world.ObjectAt(o);
            printf("object %u instance %d prop %d bounded %d box %.3f %.3f %.3f .. %.3f %.3f %.3f\n", o, ob.instance,
                   ob.prop, ob.bounded, ob.lo[0], ob.lo[1], ob.lo[2], ob.hi[0], ob.hi[1], ob.hi[2]);
            for (size_t s = 0; s < ob.count; ++s) {
                const CollisionWorld::Shape& sh = world.ShapeAt(ob.first + s);
                printf("  shape %zu type %d surface %d half %08x %08x %08x world", s, sh.type, sh.surface, b(sh.size[0]),
                       b(sh.size[1]), b(sh.size[2]));
                for (int k = 0; k < 16; ++k)
                    if (k % 4 != 3) printf(" %08x", b(sh.world[k]));
                printf("\n");
            }
        }
    }
    // CRASH_G=n: in the window after call n, every group member's body
    // (pos, vel, q, L, w) and the joint values / rates as bits before ("B")
    // and after ("A") each solver call, to set against the traced "GBb" /
    // "GAb" / "GB20" / "GB24" (BOOT_TO_MENU.md section 98)
    static int groupWindow = getenv("CRASH_G") ? atoi(getenv("CRASH_G")) : -1000;
    if (groupWindow > -1000)
        cw.onGroupStep = [](bool after) {
            if (g_call != groupWindow) return;
            auto b = [](float v) { unsigned u; memcpy(&u, &v, 4); return u; };
            const Articulated::Group& g = cw.group.group;
            for (size_t k = 0; k < g.members.size(); ++k) {
                const RigidBody& r = *g.members[k].body;
                printf("      G%c tick %d m%zu pos %08x %08x %08x vel %08x %08x %08x q %08x %08x %08x %08x L %08x %08x %08x w %08x %08x %08x",
                       after ? 'A' : 'B', g_tick, k, b(r.pos[0]), b(r.pos[1]), b(r.pos[2]), b(r.vel[0]), b(r.vel[1]),
                       b(r.vel[2]), b(r.q[0]), b(r.q[1]), b(r.q[2]), b(r.q[3]), b(r.L[0]), b(r.L[1]), b(r.L[2]),
                       b(r.w[0]), b(r.w[1]), b(r.w[2]));
                if (k) printf(" jq %08x jqd %08x", b(g.q[k]), b(g.qd[k]));
                printf("\n");
            }
        };
    // ... and the loose wheels before their integration ("GW", against the
    // traced "WI": 0x5af710's entry)
    if (groupWindow > -1000)
        cw.onLooseStep = []() {
            if (g_call != groupWindow) return;
            auto b = [](float v) { unsigned u; memcpy(&u, &v, 4); return u; };
            for (const auto& lw : cw.looseWheels) {
                const RigidBody& r = lw.body;
                printf("      GW tick %d w%d pos %08x %08x %08x vel %08x %08x %08x q %08x %08x %08x %08x L %08x %08x %08x w %08x %08x %08x\n",
                       g_tick, lw.wheel, b(r.pos[0]), b(r.pos[1]), b(r.pos[2]), b(r.vel[0]), b(r.vel[1]), b(r.vel[2]),
                       b(r.q[0]), b(r.q[1]), b(r.q[2]), b(r.q[3]), b(r.L[0]), b(r.L[1]), b(r.L[2]), b(r.w[0]), b(r.w[1]),
                       b(r.w[2]));
            }
        };
    const auto& C = g_recs["C"];
    int call = 0, firstOff = -1, compared = 0, shown = 0;
    float worst = 0;
    int worstAt = -1;
    size_t nextTraced = 0;
    long contacts = 0;
    for (int tick = 0; tick < 200000 && call < maxCalls; ++tick) {
        // a 25 Hz tick: compare the state at the car tick's entry
        const bool block = cw.car.OF(0xb7c) + 0.0083333338f > 0.04f;
        if (block) {
            g_call = call;
            auto it = S.find(unsigned(call));
            if (it == S.end()) break;
            RigidBody t;
            LoadBody(it->second.w, t);
            float dp = 0, dv = 0, dq = 0, dw = 0;
            for (int k = 0; k < 3; ++k) {
                dp = std::fmax(dp, std::fabs(cw.body.pos[k] - t.pos[k]));
                dv = std::fmax(dv, std::fabs(cw.body.vel[k] - t.vel[k]));
                dw = std::fmax(dw, std::fabs(cw.body.w[k] - t.w[k]));
            }
            for (int k = 0; k < 4; ++k) dq = std::fmax(dq, std::fabs(cw.body.q[k] - t.q[k]));
            ++compared;
            if (dp > worst) worst = dp, worstAt = call;
            const bool off = dp > 1e-4f || dv > 1e-3f || dq > 1e-4f;
            const bool first = off && firstOff < 0;
            if (first) firstOff = call;
            const int rs = rsPerCall.count(unsigned(call)) ? rsPerCall[unsigned(call)] : 0;
            const int rsg = rsGroupPerCall.count(unsigned(call)) ? rsGroupPerCall[unsigned(call)] : 0;
            if (all || call < 2 || call % 25 == 0 || first || (reload && off)) {
                ++shown;
                printf("call %4d: pos %.4f %.4f %.4f | game %.4f %.4f %.4f | dpos %.2e dvel %.2e dq %.2e dw %.2e; "
                       "contacts %ld (game responses %d%s)%s%s\n",
                       call, cw.body.pos[0], cw.body.pos[1], cw.body.pos[2], t.pos[0], t.pos[1], t.pos[2], dp, dv, dq,
                       dw, contacts, rs, rsg ? ", grouped" : "", cw.group.Active() ? " [program group]" : "",
                       first ? "  <- first departure" : "");
            }
            while (g_events && nextTraced < traced.size() && traced[nextTraced].first <= unsigned(call)) {
                printf("      [game, call %u] %s\n", traced[nextTraced].first, traced[nextTraced].second.c_str());
                ++nextTraced;
            }
            contacts = 0;
            if (reload && haveK && g_recs["K"].count(unsigned(call))) {
                // the window from the traced state: the object (its body
                // pointer kept) and the body (its group membership kept),
                // loaded where the game takes it: after the slot's pre-tick
                // (a tear-off there ends the group and recomputes the car's
                // spin before the K dump; run 21 call 109), before the car's
                // tick (CarWorld::onBeforeCarTick)
                const std::vector<unsigned>* K = &g_recs["K"][unsigned(call)].w;
                const std::vector<unsigned>* Kb = &it->second.w;
                cw.onBeforeCarTick = [K, Kb]() {
                    memcpy(cw.car.obj, K->data(), std::min<size_t>(K->size() * 4, CarSim::kTracedSize));
                    void* g = cw.body.group;
                    const int member = cw.body.member;
                    LoadBody(*Kb, cw.body);
                    cw.body.group = g;
                    cw.body.member = member;
                    cw.onBeforeCarTick = nullptr;
                };
            }
            ++call;
        }
        float c[5] = { 0, 0, 0, 0, 0 };
        bool hb = false;
        auto ct = C.find(static_cast<unsigned>(tick));
        if (ct != C.end() && ct->second.w.size() >= 6) {
            for (int k = 0; k < 5; ++k) c[k] = Fl(ct->second.w[k]);
            hb = (ct->second.w[5] & 0xff) != 0;
        }
        // CRASH_M=n: the body's matrix and position as bits before each tick
        // of the window after call n (to compare with a traced "NPa")
        if (const char* e = getenv("CRASH_M"))
            if (atoi(e) == call - 1) {
                auto b = [](float x) { unsigned u; memcpy(&u, &x, 4); return u; };
                printf("      tick %d M", tick);
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 3; ++k) printf(" %08x", b(cw.body.matrix[r][k]));
                printf(" pos %08x %08x %08x\n", b(cw.body.pos[0]), b(cw.body.pos[1]), b(cw.body.pos[2]));
            }
        g_tick = tick;
        // CRASH_SHAPES=t: the car's shapes (half sizes, local offsets) before
        // tick t (the dent shortens box 1; against a traced "NPah")
        {
            if (getenv("CRASH_SHAPES") && atoi(getenv("CRASH_SHAPES")) == tick)
                for (size_t s = 0; s < cw.shapes.size(); ++s) {
                    auto b = [](float v) { unsigned u; memcpy(&u, &v, 4); return u; };
                    const auto& sh = cw.shapes[s];
                    printf("car shape %zu half %08x %08x %08x local t %08x %08x %08x\n", s, b(sh.size[0]), b(sh.size[1]),
                           b(sh.size[2]), b(sh.local[12]), b(sh.local[13]), b(sh.local[14]));
                }
        }
        contacts += cw.Step(c, hb);
        props.TakeBroken();
        // CRASH_DEBRIS=1: every prop debris body after the tick (bits: pos,
        // vel, q, L, w, the matrix's rows 0..2), against the traced "NPb" /
        // "RSb" of the body the car meets in the next tick (section 104)
        if (getenv("CRASH_DEBRIS"))
            for (int k = 0; k < CarWorld::kDebrisSlots; ++k) {
                const CarWorld::PropDebris& d = cw.propDebris[k];
                if (!d.active) continue;
                auto b = [](float v) { unsigned u; memcpy(&u, &v, 4); return u; };
                const RigidBody& r = d.body;
                printf("      DB tick %d slot %d pos %08x %08x %08x vel %08x %08x %08x q %08x %08x %08x %08x L %08x %08x %08x w "
                       "%08x %08x %08x m %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                       tick, k, b(r.pos[0]), b(r.pos[1]), b(r.pos[2]), b(r.vel[0]), b(r.vel[1]), b(r.vel[2]), b(r.q[0]),
                       b(r.q[1]), b(r.q[2]), b(r.q[3]), b(r.L[0]), b(r.L[1]), b(r.L[2]), b(r.w[0]), b(r.w[1]), b(r.w[2]),
                       b(r.matrix[0][0]), b(r.matrix[0][1]), b(r.matrix[0][2]), b(r.matrix[1][0]), b(r.matrix[1][1]),
                       b(r.matrix[1][2]), b(r.matrix[2][0]), b(r.matrix[2][1]), b(r.matrix[2][2]));
            }
    }
    printf("%d calls compared; first departure (pos 1e-4, vel 1e-3, q 1e-4): %d; worst position difference %.4f at "
           "call %d; program: %zu hinged / torn bodies, %zu loose wheels, group %s\n",
           compared, firstOff, worst, worstAt, cw.group.bodies.size(), cw.looseWheels.size(),
           cw.group.Active() ? "active" : "none");
    return 0;
}
