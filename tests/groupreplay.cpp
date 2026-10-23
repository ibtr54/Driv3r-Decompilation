// Replays the articulated-group solver 0x5c2600 on the traced calls of run 11
// (D3PROXY_GROUP: "GB*" before, "GA*" after each call): the traced member
// list, joint arrays, member bodies and group records are rebuilt in this
// process (the pointers between them moved to the copies), the solver runs,
// and the result is compared with the traced one word by word.
//
//   groupreplay <phys.txt> <Driv3r.exe> [first [count]]
//
// This first version runs the game's own solver (gamecode.h) to prove the
// harness against the trace; the port is checked against the same data.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../articulated.h"
#include "../panels.h"
#include "gamecode.h"

namespace
{
struct Dump
{
    uint32_t addr = 0;
    std::vector<uint32_t> words;
};
// tag -> index -> dump
std::map<std::string, std::map<unsigned, Dump>> g_dumps;

void Read(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    static char line[1 << 16];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != 'G' && line[0] != 'H') continue;
        char tag[8];
        unsigned n = 0, addr = 0;
        int used = 0;
        if (sscanf(line, "%7s %u %x%n", tag, &n, &addr, &used) < 3) continue;
        Dump d;
        d.addr = addr;
        const char* p = line + used;
        char* end = nullptr;
        for (;;) {
            const unsigned long v = strtoul(p, &end, 16);
            if (end == p) break;
            d.words.push_back(static_cast<uint32_t>(v));
            p = end;
        }
        g_dumps[tag][n] = d;
    }
    fclose(f);
}

// One call's records rebuilt here: blocks of 16-byte aligned words, each
// with the traced address it stands for.
struct Block
{
    const char* name;
    uint32_t traced;
    uint32_t* words;
    size_t count;
};
struct State
{
    std::vector<Block> blocks;
    uint32_t list = 0;  // the copy of the member list

    ~State()
    {
        for (Block& b : blocks) _aligned_free(b.words);
    }
    uint32_t Add(const char* name, uint32_t traced, const uint32_t* words, size_t count)
    {
        Block b{ name, traced, static_cast<uint32_t*>(_aligned_malloc(count * 4 + 16, 16)), count };
        memcpy(b.words, words, count * 4);
        blocks.push_back(b);
        return reinterpret_cast<uint32_t>(b.words);
    }
    // traced pointers -> copies (or back), for the words that hold them
    void Move(bool toCopies)
    {
        for (Block& b : blocks)
            for (size_t i = 0; i < b.count; ++i)
                for (const Block& t : blocks) {
                    const uint32_t from = toCopies ? t.traced : reinterpret_cast<uint32_t>(t.words);
                    if (b.words[i] == from) {
                        b.words[i] = toCopies ? reinterpret_cast<uint32_t>(t.words) : t.traced;
                        break;
                    }
                }
    }
};

void Build(unsigned n, State& s)
{
    const Dump& list = g_dumps["GBL"][n];
    const int members = static_cast<int>(list.words[0]);
    s.list = s.Add("list", list.addr, list.words.data(), list.words.size());
    s.Add("q", g_dumps["GB20"][n].addr, g_dumps["GB20"][n].words.data(), g_dumps["GB20"][n].words.size());
    s.Add("qd", g_dumps["GB24"][n].addr, g_dumps["GB24"][n].words.data(), g_dumps["GB24"][n].words.size());
    std::vector<uint32_t> bodies;
    for (int k = 0; k < members; ++k) {
        const Dump& b = g_dumps["GBb"][n * 16 + k];
        const Dump& r = g_dumps["GBr"][n * 16 + k];
        s.Add("body", b.addr, b.words.data(), b.words.size());
        s.Add("rec", r.addr, r.words.data(), r.words.size());
        bodies.push_back(b.addr);
    }
    s.Add("members", list.words[0x28 / 4], bodies.data(), bodies.size());
    s.Move(true);
}

int Compare(unsigned n, State& s, int& shown)
{
    s.Move(false);
    const int members = static_cast<int>(g_dumps["GAL"][n].words[0]);
    std::vector<const Dump*> want = { &g_dumps["GAL"][n], &g_dumps["GA20"][n], &g_dumps["GA24"][n] };
    for (int k = 0; k < members; ++k) {
        want.push_back(&g_dumps["GAb"][n * 16 + k]);
        want.push_back(&g_dumps["GAr"][n * 16 + k]);
    }
    int bad = 0;
    for (size_t j = 0; j < want.size(); ++j) {
        const Block& b = s.blocks[j];
        for (size_t i = 0; i < want[j]->words.size() && i < b.count; ++i) {
            if (b.words[i] == want[j]->words[i]) continue;
            ++bad;
            if (shown++ < 12)
                printf("    %s%d +0x%03zx: %08x (%g), traced %08x (%g)\n", b.name, j < 3 ? 0 : static_cast<int>(j - 3) / 2,
                       i * 4, b.words[i], GameCode::Float(b.words[i]), want[j]->words[i],
                       GameCode::Float(want[j]->words[i]));
        }
    }
    return bad;
}
}  // namespace


namespace
{
// A traced body record (0x1e0 bytes) to and from RigidBody's fields (the
// game's offsets; the words RigidBody does not name stay in the record).
void LoadBody(const uint32_t* w, RigidBody& b)
{
    auto f = [&](int off) { return GameCode::Float(w[off / 4]); };
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
void StoreBody(const RigidBody& b, uint32_t* w)
{
    auto f = [&](int off, float v) { w[off / 4] = GameCode::Bits(v); };
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) f(0x10 + r * 16 + c * 4, b.matrix[r][c]);
    for (int i = 0; i < 4; ++i) {
        f(0x80 + 4 * i, b.pos[i]); f(0x90 + 4 * i, b.vel[i]); f(0xa0 + 4 * i, b.q[i]); f(0xb0 + 4 * i, b.L[i]);
        f(0xc0 + 4 * i, b.force[i]); f(0xd0 + 4 * i, b.torque[i]); f(0xe0 + 4 * i, b.accel[i]);
        f(0x150 + 4 * i, b.w[i]); f(0x160 + 4 * i, b.dq[i]); f(0x170 + 4 * i, b.inertia[i]);
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) { f(0xf0 + r * 16 + c * 4, b.R[r][c]); f(0x120 + r * 16 + c * 4, b.invI[r][c]); }
    for (int i = 0; i < 6; ++i) f(0x1a0 + 4 * i, b.sleep[i]);
    f(0x1c0, b.mass);
    w[0x1c4 / 4] = static_cast<uint32_t>(b.state);
}

// The port's view of a rebuilt state: the members' bodies (loaded into
// RigidBody), records and parents, the joint arrays copied from the list's.
struct PortGroup
{
    Articulated::Group group;
    std::vector<RigidBody> bodies;
    std::vector<uint32_t*> blocks;
};
void MakeGroup(State& s, PortGroup& pg)
{
    Articulated::Group& g = pg.group;
    uint32_t* list = reinterpret_cast<uint32_t*>(s.list);
    const int n = static_cast<int>(list[0]);
    uint32_t* bodies = reinterpret_cast<uint32_t*>(list[0x28 / 4]);
    const float* q = reinterpret_cast<const float*>(list[0x20 / 4]);
    const float* qd = reinterpret_cast<const float*>(list[0x24 / 4]);
    pg.bodies.resize(n);
    for (int k = 0; k < n; ++k) {
        Articulated::Group::Member m;
        uint32_t* body = reinterpret_cast<uint32_t*>(bodies[k]);
        LoadBody(body, pg.bodies[k]);
        pg.blocks.push_back(body);
        m.body = &pg.bodies[k];
        m.rec = reinterpret_cast<float*>(body[0x1d8 / 4]);
        const uint32_t parent = reinterpret_cast<uint32_t*>(m.rec)[0x32c / 4];
        for (int j = 0; j < n; ++j)
            if (bodies[j] == parent) m.parent = j;
        g.members.push_back(m);
        g.q.push_back(q[k]);
        g.qd.push_back(qd[k]);
    }
    g.toggle = static_cast<int>(list[2]);
}

// The port's bodies, joint arrays and toggle back into the state.
void PutGroup(const PortGroup& pg, State& s)
{
    const Articulated::Group& g = pg.group;
    uint32_t* list = reinterpret_cast<uint32_t*>(s.list);
    float* q = reinterpret_cast<float*>(list[0x20 / 4]);
    float* qd = reinterpret_cast<float*>(list[0x24 / 4]);
    for (size_t k = 0; k < g.q.size(); ++k) {
        q[k] = g.q[k];
        qd[k] = g.qd[k];
        StoreBody(pg.bodies[k], pg.blocks[k]);
    }
    list[2] = static_cast<uint32_t>(g.toggle);
}

// Two rebuilt states word by word (the port's against the game's).
int CompareStates(State& mine, State& game, int& shown)
{
    mine.Move(false);
    game.Move(false);
    int bad = 0;
    for (size_t j = 0; j < mine.blocks.size(); ++j) {
        const Block& a = mine.blocks[j];
        const Block& b = game.blocks[j];
        std::string offs;
        for (size_t i = 0; i < a.count; ++i) {
            if (a.words[i] == b.words[i]) continue;
            ++bad;
            char t[16];
            snprintf(t, sizeof(t), " %zx", i * 4);
            offs += t;
            if (shown++ < (getenv("GROUP_SHOW") ? atoi(getenv("GROUP_SHOW")) : 12))
                printf("    %s%d +0x%03zx: %08x (%g), game %08x (%g)\n", a.name, j < 3 ? 0 : static_cast<int>(j - 3) / 2,
                       i * 4, a.words[i], GameCode::Float(a.words[i]), b.words[i], GameCode::Float(b.words[i]));
        }
        if (!offs.empty() && getenv("GROUP_OFFSETS"))
            printf("    %s%d:%s\n", a.name, j < 3 ? 0 : static_cast<int>(j - 3) / 2, offs.c_str());
    }
    return bad;
}
}  // namespace

namespace
{
// Mode "hinge" (run 12, D3PROXY_PARTS): the traced hinges in order, from
// the car alone: AddMember with the traced arguments of 0x5a1890 (edi = the
// car, arg 1 the panel's body, args 2..7 the hinge points and axes), then
// the new member's record, its body and the member order against the dumps
// right after the call.
bool g_chain = false;
std::vector<VehicleData> g_vehicles;
const VehicleData* g_vehicle = nullptr;
float g_width = 0;

// Vehicle 6 from the .vvs (the traced car); the slot's width +0x4a0.
bool LoadVehicle(const char* vvs)
{
    std::vector<uint8_t> file;
    if (FILE* f = fopen(vvs, "rb")) {
        fseek(f, 0, SEEK_END);
        file.resize(static_cast<size_t>(ftell(f)));
        fseek(f, 0, SEEK_SET);
        if (fread(file.data(), 1, file.size(), f) != file.size()) file.clear();
        fclose(f);
    }
    if (!ReadVehicles(file, g_vehicles)) return false;
    for (const VehicleData& c : g_vehicles)
        if (c.id == 6) g_vehicle = &c;
    if (!g_vehicle || g_vehicle->collision.empty()) return false;
    g_width = g_vehicle->collision[0].size[0] + g_vehicle->collision[0].size[0];
    return true;
}

// The part of the n-th traced creation: its UPVH record ("HBq") or, where
// not dumped (run 12), the part whose damage entry is the traced one.
VehicleData::Part TracedPart(unsigned n, const VehicleData::DamageEntry& e)
{
    VehicleData::Part p;
    if (g_dumps["HBq"].count(n)) {
        const std::vector<uint32_t>& q = g_dumps["HBq"][n].words;
        p.type = q[0] & 0xffff;
        p.id = q[0] >> 16;
        p.hardpoint = q[6] & 0xffff;
        p.record = q[6] >> 16;
        return p;
    }
    for (const VehicleData::Part& c : g_vehicle->parts)
        if (c.record >= 0 && c.record < int(g_vehicle->damage.size()) && c.hardpoint >= 0 &&
            !memcmp(g_vehicle->damage[c.record].matrix, e.matrix, 64))
            p = c;
    return p;
}

int RunHinges()
{
    auto& before = g_dumps["HJc"];
    if (!before.count(0)) { printf("no hinge dumps\n"); return 1; }
    std::vector<RigidBody> bodies(16);
    std::vector<std::vector<float>> recs;
    recs.reserve(16);
    std::vector<uint32_t> addrs;  // traced body address per body slot
    Articulated::Group g;
    LoadBody(before[0].words.data(), bodies[0]);
    recs.emplace_back(before[0].words.size() ? 0x3c0 / 4 : 0);
    const Dump& carRec = g_dumps["HJcr"][0];
    recs[0].assign(0x3c0 / 4, 0.0f);
    memcpy(recs[0].data(), carRec.words.data(), 0x3c0);
    g.members.push_back({ &bodies[0], recs[0].data(), -1, {} });
    g.q.push_back(0);
    g.qd.push_back(0);
    addrs.push_back(before[0].addr);
    // the fields AddMember writes in the new record (the pointers at +0x324
    // / +0x32c left out)
    std::vector<int> fields;
    for (int o = 0; o < 0x160; o += 4) fields.push_back(o);
    for (int o = 0x260; o < 0x280; o += 4) fields.push_back(o);
    for (int o : { 0x328, 0x330, 0x334, 0x338, 0x350, 0x354, 0x358, 0x35c, 0x360, 0x364, 0x368, 0x378 }) fields.push_back(o);
    int total = 0, bad = 0;
    for (unsigned n = 0; g_dumps["HJp"].count(n); ++n) {
        const size_t slot = g.members.size();
        LoadBody(g_dumps["HJp"][n].words.data(), bodies[slot]);
        recs.emplace_back(0x3c0 / 4, 0.0f);
        addrs.push_back(g_dumps["HJp"][n].addr);
        float v[6][3];
        for (int i = 0; i < 6; ++i) {
            const Dump& d = g_dumps["HJv" + std::to_string(i + 2)][n];
            for (int k = 0; k < 3; ++k) v[i][k] = GameCode::Float(d.words[k]);
        }
        if (g_chain) {
            // mode "chain": the port's own body and hinge inputs (from the
            // vehicle file, the traced damage entry, pose, car matrix and
            // record values), the car as the port keeps it
            VehicleData::DamageEntry e;
            const std::vector<uint32_t>& pw = g_dumps["HBp"][n].words;
            for (int i = 0; i < 16; ++i) e.matrix[i] = GameCode::Float(pw[i]);
            for (int i = 0; i < 3; ++i) e.inertia[i] = GameCode::Float(pw[16 + i]);
            e.mass = GameCode::Float(pw[19]);
            float X[16], car[16];
            for (int i = 0; i < 16; ++i) {
                X[i] = g_dumps["HBx"].count(n) ? GameCode::Float(g_dumps["HBx"][n].words[i]) : (i % 5 == 0 ? 1.0f : 0.0f);
                car[i] = GameCode::Float(g_dumps["HBm"][n].words[i]);
            }
            PanelBody(e, X, car, bodies[0], bodies[slot]);
            const VehicleData::Part p = TracedPart(n, e);
            PanelHinge h;
            if (!PanelHingeInputs(*g_vehicle, p, X, GameCode::Float(g_dumps["HBr"][n].words[1]),
                                  GameCode::Float(g_dumps["HBr"][n].words[2]), g_width, h)) {
                printf("hinge %u: no hinge inputs\n", n);
                return 1;
            }
            memcpy(v[0], h.cp, 12); memcpy(v[1], h.cax, 12); memcpy(v[2], h.cref, 12);
            memcpy(v[3], h.pp, 12); memcpy(v[4], h.pax, 12); memcpy(v[5], h.pref, 12);
        }
        g.AddMember(0, &bodies[slot], recs.back().data(), v[0], v[1], v[2], v[3], v[4], v[5], 1);
        // the new record and body against the dumps
        const Dump& wantRec = g_dumps["HJPR"][n];
        const float* rec = recs.back().data();
        int b = 0;
        for (int o : fields) {
            const uint32_t have = GameCode::Bits(rec[o / 4]), want = wantRec.words[o / 4];
            if (have == want) continue;
            if (b++ < 10) printf("  hinge %u rec +0x%03x: %08x (%g), traced %08x (%g)\n", n, o, have, GameCode::Float(have), want, GameCode::Float(want));
        }
        uint32_t body[0x1e0 / 4];
        memcpy(body, g_dumps["HJP"][n].words.data(), sizeof(body));
        const std::vector<uint32_t> traced(body, body + 0x1e0 / 4);
        StoreBody(bodies[slot], body);
        for (int o = 0x10; o < 0x1c8; o += 4) {
            if (body[o / 4] == traced[o / 4]) continue;
            if (b++ < 20) printf("  hinge %u body +0x%03x: %08x (%g), traced %08x (%g)\n", n, o, body[o / 4], GameCode::Float(body[o / 4]), traced[o / 4], GameCode::Float(traced[o / 4]));
        }
        // the member order
        const Dump& order = g_dumps["HJL28"][n];
        for (size_t k = 0; k < g.members.size() && k < order.words.size(); ++k) {
            const size_t s = static_cast<size_t>(g.members[k].body - &bodies[0]);
            if (addrs[s] != order.words[k]) { ++b; printf("  hinge %u member %zu is the wrong body\n", n, k); }
        }
        ++total;
        if (b) { ++bad; printf("hinge %u: %d differences\n", n, b); }
    }
    printf("%d / %d hinges exact\n", total - bad, total);
    if (g_chain && g_dumps["GBL"].count(0)) {
        // with the joints' settings (PanelJoint), against the group at the
        // first solver call after the crash
        for (int n = 0; n < total; ++n) {
            VehicleData::DamageEntry e;
            for (int i = 0; i < 16; ++i) e.matrix[i] = GameCode::Float(g_dumps["HBp"][n].words[i]);
            const VehicleData::Part p = TracedPart(n, e);
            for (size_t k = 0; k < g.members.size(); ++k)
                if (addrs[static_cast<size_t>(g.members[k].body - &bodies[0])] == g_dumps["HJp"][n].addr)
                    PanelJoint(g, static_cast<int>(k), p.type, p.id);
        }
        int d = 0;
        for (size_t k = 0; k < g.members.size(); ++k) {
            const size_t s = static_cast<size_t>(g.members[k].body - &bodies[0]);
            const Dump& gb = g_dumps["GBb"][static_cast<unsigned>(k)];
            const Dump& gr = g_dumps["GBr"][static_cast<unsigned>(k)];
            if (gb.addr != addrs[s]) { printf("  first call: member %zu is another body\n", k); ++d; continue; }
            if (GameCode::Bits(g.q[k]) != g_dumps["GB20"][0].words[k] ||
                GameCode::Bits(g.qd[k]) != g_dumps["GB24"][0].words[k]) {
                printf("  first call: member %zu q / qd %g %g, traced %g %g\n", k, g.q[k], g.qd[k],
                       GameCode::Float(g_dumps["GB20"][0].words[k]), GameCode::Float(g_dumps["GB24"][0].words[k]));
                ++d;
            }
            uint32_t body[0x1e0 / 4];
            memcpy(body, gb.words.data(), sizeof(body));
            StoreBody(bodies[s], body);
            for (int o = 0x10; o < 0x1c8; o += 4)
                if (body[o / 4] != gb.words[o / 4] && d++ < 30)
                    printf("  first call: member %zu body +0x%03x: %g, traced %g\n", k, o,
                           GameCode::Float(body[o / 4]), GameCode::Float(gb.words[o / 4]));
            if (k == 0) continue;
            const float* rec = recs[s].data();
            for (int o : fields)
                if (GameCode::Bits(rec[o / 4]) != gr.words[o / 4] && d++ < 30)
                    printf("  first call: member %zu rec +0x%03x: %g, traced %g\n", k, o, rec[o / 4],
                           GameCode::Float(gr.words[o / 4]));
        }
        printf("first solver call: %s (%d differences)\n", d ? "differs" : "exact", d);
        if (d) return 1;
    }
    return bad ? 1 : 0;
}
}  // namespace

namespace
{
// Mode "panels" (run 12 / 14, D3PROXY_PARTS): every traced part creation:
// the body from the traced damage entry, pose and car (PanelBody) against
// the body after 0x4f8ef0, the hinge inputs from the vehicle file
// (PanelHingeInputs) against the traced arguments of 0x5a1890.
int RunPanels(const char* vvs)
{
    if (!LoadVehicle(vvs)) { printf("cannot read vehicle 6 from %s\n", vvs); return 1; }
    const VehicleData* v = g_vehicle;
    const float width = g_width;
    int total = 0, bad = 0;
    for (unsigned n = 0; g_dumps["HBp"].count(n); ++n) {
        int b = 0;
        auto floats = [](const Dump& d, float* out, int count) {
            for (int i = 0; i < count; ++i) out[i] = GameCode::Float(d.words[i]);
        };
        VehicleData::DamageEntry e;
        floats(g_dumps["HBp"][n], e.matrix, 16);
        for (int i = 0; i < 3; ++i) e.inertia[i] = GameCode::Float(g_dumps["HBp"][n].words[16 + i]);
        e.mass = GameCode::Float(g_dumps["HBp"][n].words[19]);
        float X[16], car[16];
        if (g_dumps["HBx"].count(n)) floats(g_dumps["HBx"][n], X, 16);
        else for (int i = 0; i < 16; ++i) X[i] = (i % 5 == 0) ? 1.0f : 0.0f;  // run 12: not dumped (the identity)
        floats(g_dumps["HBm"][n], car, 16);
        RigidBody carBody, body;
        LoadBody(g_dumps["HJc"][n].words.data(), carBody);
        PanelBody(e, X, car, carBody, body);
        uint32_t words[0x1e0 / 4];
        memcpy(words, g_dumps["HBb"][n].words.data(), sizeof(words));
        const std::vector<uint32_t> traced(words, words + 0x1e0 / 4);
        StoreBody(body, words);
        for (int o = 0x10; o < 0x1c8; o += 4) {
            if (o >= 0x50 && o < 0x80) continue;  // not RigidBody's
            if (words[o / 4] == traced[o / 4]) continue;
            if (b++ < (getenv("GROUP_SHOW") ? atoi(getenv("GROUP_SHOW")) : 12))
                printf("  part %u body +0x%03x: %08x (%g), traced %08x (%g)\n", n, o, words[o / 4],
                       GameCode::Float(words[o / 4]), traced[o / 4], GameCode::Float(traced[o / 4]));
        }
        // the part (UPVH, 0x20 bytes) and its record's damage values
        const VehicleData::Part p = TracedPart(n, e);
        const float d4 = GameCode::Float(g_dumps["HBr"][n].words[1]), d8 = GameCode::Float(g_dumps["HBr"][n].words[2]);
        PanelHinge h;
        if (!PanelHingeInputs(*v, p, X, d4, d8, width, h)) {
            printf("  part %u: no hinge (type %d id 0x%x hardpoint %d record %d)\n", n, p.type, p.id, p.hardpoint, p.record);
            ++b;
        } else {
            const float* mine[6] = { h.cp, h.cax, h.cref, h.pp, h.pax, h.pref };
            const char* names[6] = { "cp", "cax", "cref", "pp", "pax", "pref" };
            for (int i = 0; i < 6; ++i) {
                const Dump& d = g_dumps["HJv" + std::to_string(i + 2)][n];
                for (int k = 0; k < 3; ++k) {
                    if (GameCode::Bits(mine[i][k]) == d.words[k]) continue;
                    if (b++ < 24)
                        printf("  part %u %s[%d]: %08x (%g), traced %08x (%g)\n", n, names[i], k,
                               GameCode::Bits(mine[i][k]), mine[i][k], d.words[k], GameCode::Float(d.words[k]));
                }
            }
        }
        ++total;
        if (b) { ++bad; printf("part %u (id 0x%x): %d differences\n", n, p.id, b); }
    }
    printf("%d / %d parts exact\n", total - bad, total);
    return bad ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: groupreplay <game|dynamics> <phys.txt> <Driv3r.exe> [first [count]]\n");
        return 1;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string mode = argv[1];
    if (!GameCode::Load(argv[3])) { fprintf(stderr, "cannot load %s\n", argv[3]); return 1; }
    Read(argv[2]);
    if (mode == "hinge") return RunHinges();
    if (mode == "chain") {
        if (!LoadVehicle(argc > 4 ? argv[4] : "D:/l/gta-vc/DRIV3R/Vehicles/miami.vvs")) return 1;
        g_chain = true;
        return RunHinges();
    }
    if (mode == "panels") return RunPanels(argc > 4 ? argv[4] : "D:/l/gta-vc/DRIV3R/Vehicles/miami.vvs");
    if (mode == "kcheck") {
        // (2026-10-23, BOOT_TO_MENU.md section 76) the port's own
        // consistency at a contact on member 0 (the car): K from
        // ContactInertia (contact rows C, point p) against the velocity
        // change ContactImpulse makes there for P = e_z, the point taken as
        // the world offset r (pw) or as r in the frame (pf = r C')
        State s;
        Build(argc > 4 ? static_cast<unsigned>(atoi(argv[4])) : 0, s);
        for (int variant = 0; variant < 8; ++variant) {
            PortGroup port;
            MakeGroup(s, port);
            Articulated::Group& g = port.group;
            RigidBody& b = *g.members[0].body;
            const float a = 0.6f, c0 = cosf(a), s0 = sinf(a);
            const float C0[3][4] = { { c0, 0.3f * s0, -s0, 0 }, { 0, 1.0f, 0, 0 }, { s0, 0, c0, 0 } };
            // orthonormalise (rows), then C or its transpose
            float Cr[3][4] = {};
            {
                float u[3] = { C0[0][0], C0[0][1], C0[0][2] };
                float l = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                for (float& x : u) x /= l;
                float n[3] = { C0[2][0], C0[2][1], C0[2][2] };
                float d = n[0] * u[0] + n[1] * u[1] + n[2] * u[2];
                for (int i = 0; i < 3; ++i) n[i] -= d * u[i];
                l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (float& x : n) x /= l;
                const float t[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };
                for (int i = 0; i < 3; ++i) { Cr[0][i] = u[i]; Cr[1][i] = t[i]; Cr[2][i] = n[i]; }
            }
            const bool transposed = (variant & 1) != 0, inFrame = (variant & 2) != 0, readT = (variant & 4) != 0;
            float C[3][4] = {};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) C[i][j] = transposed ? Cr[j][i] : Cr[i][j];
            const float r[3] = { 0.4f, -0.3f, 1.9f };
            float pf[3];
            VecMat3(r, C, pf);
            float K[3][4];
            Articulated::SMat x1;
            g.ContactInertia(0, C, inFrame ? pf : r, K, x1);
            auto vel = [&](float out[3]) {
                const float wr[3] = { b.w[1] * r[2] - b.w[2] * r[1], b.w[2] * r[0] - b.w[0] * r[2],
                                      b.w[0] * r[1] - b.w[1] * r[0] };
                for (int i = 0; i < 3; ++i) {
                    const float v = b.vel[i] + wr[i];
                    out[i] = v;
                }
            };
            float v0[3], v1[3];
            vel(v0);
            const float P[3] = { 0, 0, 1.0f };
            g.ContactImpulse(0, x1, P, false);
            vel(v1);
            float d[3];
            for (int i = 0; i < 3; ++i) d[i] = v1[i] - v0[i];
            float df[3];
            for (int i = 0; i < 3; ++i)
                df[i] = readT ? C[0][i] * d[0] + C[1][i] * d[1] + C[2][i] * d[2]
                              : C[i][0] * d[0] + C[i][1] * d[1] + C[i][2] * d[2];
            printf("rows %s, point %s, read %s: K column 2 (%.6g %.6g %.6g) row 2 (%.6g %.6g %.6g); dv (%.6g %.6g %.6g)\n",
                   transposed ? "C'" : "C", inFrame ? "r C'" : "world", readT ? "C'" : "C", K[0][2], K[1][2], K[2][2],
                   K[2][0], K[2][1], K[2][2], df[0], df[1], df[2]);
        }
        return 0;
    }
    const unsigned first = argc > 4 ? static_cast<unsigned>(atoi(argv[4])) : 0;
    const unsigned count = argc > 5 ? static_cast<unsigned>(atoi(argv[5])) : 600;
    if (mode == "free") {
        // the port's solver alone from the traced state of call first, for
        // count steps, against the traced joint values (the car's own
        // impulses and the contacts are left out)
        State s;
        Build(first, s);
        PortGroup port;
        MakeGroup(s, port);
        for (unsigned n = first; n < first + count && g_dumps["GB20"].count(n + 1); ++n) {
            port.group.Step(GameCode::Float(0x3c088889));
            for (auto& m : port.group.members) m.body->UpdateMatrix();
            const std::vector<uint32_t>& q = g_dumps["GB20"][n + 1].words;
            const std::vector<uint32_t>& qd = g_dumps["GB24"][n + 1].words;
            printf("step %u:", n + 1);
            for (size_t k = 1; k < port.group.q.size() && k < q.size(); ++k)
                printf(" q %.4f (%.4f) qd %.3f (%.3f)", port.group.q[k], GameCode::Float(q[k]), port.group.qd[k],
                       GameCode::Float(qd[k]));
            const RigidBody& car = port.bodies[0];
            const std::vector<uint32_t>& cb = g_dumps["GBb"][(n + 1) * 16].words;
            printf("  car pos %.4f %.4f %.4f (%.4f %.4f %.4f)\n", car.pos[0], car.pos[1], car.pos[2],
                   GameCode::Float(cb[0x80 / 4]), GameCode::Float(cb[0x84 / 4]), GameCode::Float(cb[0x88 / 4]));
        }
        return 0;
    }
    int exact = 0, total = 0;
    for (unsigned n = first; n < first + count; ++n) {
        if (!g_dumps["GBL"].count(n) || !g_dumps["GAL"].count(n)) break;
        State s;
        Build(n, s);
        GameRegs r;
        int shown = 0, bad = 0;
        if (mode == "game") {
            // the game's solver against the trace
            r.ebx = s.list;
            const uint32_t step = 0x3c088889;  // 1/120, as traced
            if (!GameCode::Call(0x5c2600, r, { step })) {
                printf("call %u: fault at %08x\n", n, GameCode::Game(GameCode::g_faultAddr));
                ++total;
                continue;
            }
            bad = Compare(n, s, shown);
        } else if (mode == "dynamics") {
            // the port's 0x5c2100 against the game's on the same state
            State g;
            Build(n, g);
            if (!GameCode::Call(0x5c2100, r, { g.list })) {
                printf("call %u: fault at %08x\n", n, GameCode::Game(GameCode::g_faultAddr));
                ++total;
                continue;
            }
            PortGroup port;
            MakeGroup(s, port);
            port.group.Dynamics();
            PutGroup(port, s);
            bad = CompareStates(s, g, shown);
        } else if (mode == "impulse") {
            // 0x5b11c0's group branch (0x5c7280 + 0x5c7390): an impulse on
            // member n % count, the port against the game on the same state
            // (the game's world bookkeeping at 0x5b14b3 is cut off: the
            // epilogue is copied there)
            static bool patched = false;
            if (!patched) {
                static const uint8_t ret[] = { 0x5f, 0x5e, 0x5d, 0x5b, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00, 0xc3 };
                uint8_t* at = reinterpret_cast<uint8_t*>(GameCode::At(0x5b14b3));
                DWORD old;
                VirtualProtect(at, sizeof(ret), PAGE_EXECUTE_READWRITE, &old);
                memcpy(at, ret, sizeof(ret));
                patched = true;
            }
            State g;
            Build(n, g);
            uint32_t* glist = reinterpret_cast<uint32_t*>(g.list);
            const int members = static_cast<int>(glist[0]);
            const int k = static_cast<int>(n % static_cast<unsigned>(members));
            const float scale = 1.0f + static_cast<float>(n % 7);
            alignas(16) static float J[4], A[4];
            J[0] = 300.0f * scale; J[1] = -150.0f; J[2] = 900.0f / scale; J[3] = 0;
            A[0] = 20.0f; A[1] = -40.0f * scale; A[2] = 10.0f; A[3] = 0;
            r.eax = reinterpret_cast<uint32_t*>(glist[0x28 / 4])[k];
            if (!GameCode::Call(0x5b11c0, r, { reinterpret_cast<uint32_t>(J), reinterpret_cast<uint32_t>(A) })) {
                printf("call %u: fault at %08x\n", n, GameCode::Game(GameCode::g_faultAddr));
                ++total;
                continue;
            }
            PortGroup port;
            MakeGroup(s, port);
            port.group.Impulse(k, J, A);
            PutGroup(port, s);
            bad = CompareStates(s, g, shown);
        } else if (mode == "contact") {
            // 0x5c67c0 (the inverse inertia at a contact point of member n %
            // count): a contact frame turned by n and a point near the body,
            // the port against the game: K, X1 and the whole state
            State g;
            Build(n, g);
            uint32_t* glist = reinterpret_cast<uint32_t*>(g.list);
            const int members = static_cast<int>(glist[0]);
            const int k = static_cast<int>(n % static_cast<unsigned>(members));
            const float a = 0.37f * static_cast<float>(n % 17), c0 = cosf(a), s0 = sinf(a);
            alignas(16) static float rows[64], point[4], K[12], X1[48];
            const float fr[3][4] = { { c0, 0, -s0, 0 }, { 0, 1.0f, 0, 0 }, { s0, 0, c0, 0 } };
            memset(rows, 0, sizeof(rows));
            memcpy(rows, fr, sizeof(fr));
            point[0] = 0.4f - 0.05f * static_cast<float>(n % 9);
            point[1] = -0.3f;
            point[2] = 1.1f - 0.13f * static_cast<float>(n % 5);
            point[3] = 0;
            const uint32_t body = reinterpret_cast<uint32_t*>(glist[0x28 / 4])[k];
            r.eax = body + 0xf0;
            r.ebx = reinterpret_cast<uint32_t>(K);
            if (!GameCode::Call(0x5c67c0, r,
                                { reinterpret_cast<uint32_t>(X1), body, reinterpret_cast<uint32_t>(rows),
                                  reinterpret_cast<uint32_t>(point) })) {
                printf("call %u: fault at %08x\n", n, GameCode::Game(GameCode::g_faultAddr));
                ++total;
                continue;
            }
            PortGroup port;
            MakeGroup(s, port);
            float mk[3][4];
            Articulated::SMat mx;
            port.group.ContactInertia(k, reinterpret_cast<const float(*)[4]>(rows), point, mk, mx);
            PutGroup(port, s);
            for (int i = 0; i < 12; ++i)
                if (GameCode::Bits(mk[i / 4][i % 4]) != GameCode::Bits(K[i]) && bad++ < 6)
                    printf("    K[%d][%d] %g, game %g\n", i / 4, i % 4, mk[i / 4][i % 4], K[i]);
            const float* mxf = reinterpret_cast<const float*>(&mx);
            for (int i = 0; i < 48; ++i)
                if (GameCode::Bits(mxf[i]) != GameCode::Bits(X1[i]) && bad++ < 6)
                    printf("    X1 +0x%x %g, game %g\n", i * 4, mxf[i], X1[i]);
            bad += CompareStates(s, g, shown);
        } else if (mode == "step") {
            // the port's 0x5c2600 against the trace
            PortGroup port;
            MakeGroup(s, port);
            port.group.Step(GameCode::Float(0x3c088889));
            PutGroup(port, s);
            bad = Compare(n, s, shown);
        } else {
            fprintf(stderr, "unknown mode %s\n", mode.c_str());
            return 1;
        }
        ++total;
        if (!bad) ++exact;
        else printf("call %u: %d words differ\n", n, bad);
    }
    printf("%d / %d calls exact\n", exact, total);
    return exact == total ? 0 : 1;
}
