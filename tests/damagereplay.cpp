// Replays the impact handler 0x503fb0 (damage.h) on the traced calls of run
// 15 (D3PROXY_DAMAGE): each call's slot ("DEs"), contact ("DEc"), impulse
// ("DEi") and our body ("DEb1") are loaded into a VehicleDamage, Impact runs,
// and the result is compared with the slot after the call ("DXs") and with
// the part tests' arguments ("PA*": strength, direction, side weight, the
// point and the hinged part's matrix in the car's frame).
//
//   damagereplay <phys.txt> <Driv3r.exe> <miami.vvs> <miami.BO3> [first [count]]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../articulated.h"
#include "../contact.h"
#include "../damage.h"
#include "../panels.h"
#include "../vehicle.h"

namespace
{
struct Dump
{
    uint32_t addr = 0;
    std::vector<uint32_t> words;
    long order = 0;  // the line number
};
std::map<std::string, std::map<unsigned, Dump>> g_dumps;
// "DE" / "PA" argument lines
std::map<unsigned, std::vector<uint32_t>> g_de, g_pa;
std::map<unsigned, unsigned> g_paCall;  // PA n -> DE n
std::map<unsigned, long> g_deOrder;
// every body dump of the hinged panel (any tag), by line
struct BodySeen
{
    long order;
    uint32_t addr;
    std::vector<uint32_t> words;
};
std::vector<BodySeen> g_bodies;

const char* kTags[] = { "DEc", "DEi", "DEb1", "DEb2", "DEs", "DXs", "PAq", "PAr", "PAv5", "PAm7", "PXr", "NPah" };

void Read(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    static char line[1 << 17];
    long order = 0;
    while (fgets(line, sizeof(line), f)) {
        ++order;
        char tag[8];
        unsigned n = 0;
        int used = 0;
        if (sscanf(line, "%7s %u%n", tag, &n, &used) < 2) continue;
        const std::string t = tag;
        if (t == "PA") {
            // PA n dmg m eax X a1..a12 (m decimal)
            unsigned call = 0;
            int u = 0;
            if (sscanf(line + used, " dmg %u%n", &call, &u) < 1) continue;
            std::vector<uint32_t> w;
            const char* p = line + used + u;
            char word[32];
            while (sscanf(p, "%31s%n", word, &u) == 1) {
                p += u;
                char* end = nullptr;
                const unsigned long v = strtoul(word, &end, 16);
                if (*end == 0) w.push_back(uint32_t(v));
            }
            g_paCall[n] = call;
            g_pa[n] = w;  // eax, a1..a12
            continue;
        }
        if (t == "DE") {
            // DE n ecx X a b c d / PA n dmg m eax X a1..a12
            std::vector<uint32_t> w;
            const char* p = line + used;
            char word[32];
            int u = 0;
            while (sscanf(p, "%31s%n", word, &u) == 1) {
                p += u;
                char* end = nullptr;
                const unsigned long v = strtoul(word, &end, 16);
                if (*end == 0) w.push_back(uint32_t(v));
            }
            g_de[n] = w;
            g_deOrder[n] = order;
            continue;
        }
        bool body = t == "GBb" || t == "GAb" || t == "DEb2" || t == "HJC" || t == "HJc" || t == "HBb";
        bool known = body;
        for (const char* k : kTags) known = known || t == k;
        if (!known) continue;
        Dump d;
        unsigned addr = 0;
        int u2 = 0;
        if (sscanf(line + used, "%x%n", &addr, &u2) < 1) continue;
        d.addr = addr;
        d.order = order;
        const char* p = line + used + u2;
        char* end = nullptr;
        for (;;) {
            const unsigned long v = strtoul(p, &end, 16);
            if (end == p) break;
            d.words.push_back(uint32_t(v));
            p = end;
        }
        if (body) g_bodies.push_back({ order, addr, d.words });
        if (t != "GBb" && t != "GAb") g_dumps[t][n] = std::move(d);
    }
    fclose(f);
}

float F(uint32_t w) { float x; memcpy(&x, &w, 4); return x; }
int g_renames = 0;  // surface renames checked
uint32_t W(float x) { uint32_t w; memcpy(&w, &x, 4); return w; }

std::vector<uint8_t> ReadAll(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void LoadRecord(const uint32_t* w, DamageRecord& r)
{
    r.half[0] = F(w[1]); r.half[1] = F(w[2]);
    r.state = int(w[3] & 0xffff); r.word0e = int(w[3] >> 16);
    r.flags = w[4]; r.body = w[5];
    r.f18 = F(w[6]); r.cooldown = F(w[7]); r.impact = F(w[8]);
    r.f24 = F(w[9]); r.f28 = F(w[10]); r.dent = F(w[11]);
    r.stage = uint8_t(w[12]); r.worst = uint8_t(w[12] >> 8); r.b32 = uint8_t(w[12] >> 16); r.b33 = uint8_t(w[12] >> 24);
}

int g_bad = 0, g_shown = 0;
void Check(const char* what, unsigned n, int k, uint32_t got, uint32_t want)
{
    if (got == want) return;
    ++g_bad;
    if (g_shown++ < 60)
        printf("  call %u %s%s%d: port %08x (%.9g) trace %08x (%.9g)\n", n, what, k >= 0 ? " " : "", k, got, F(got), want,
               F(want));
}
}  // namespace

// "pairs" (DMG_PAIRS set): the car's contacts with its hinged bonnet in the
// trace (body B = the panel) against box / box tests of the car's shapes
// and the panel's under three guesses for the panel shapes' frame:
// 0 local x E, 1 local x E^-1, 2 local (E: the record's damage entry),
// all in the program's shape space; then (BOOT_TO_MENU.md section 75) both
// sides' shapes in the game's space ("game space", the bonnet's frame 0 as
// the car's, 1 the PDL rows, 2 identity; DMG_SWAP swaps its x / z half
// sizes). The traced points: the record's +0x80 / +0xc0 turned by its rows
// +0x30, added to the bodies' positions.
void RunPairs(const VehicleData& v, int record, unsigned limit)
{
    const VehicleData::Part* part = nullptr;
    for (const VehicleData::Part& p : v.parts)
        if (p.record == record) part = &p;
    if (!part) return;
    printf("record %d: part id 0x%x, %zu shapes\n", record, part->id, part->collision.size());
    for (size_t i = 0; i < v.collision.size(); ++i) {
        const VehicleData::Shape& s = v.collision[i];
        printf("  car shape %zu: type %d surface %d size (%.4f %.4f %.4f) at (%.4f %.4f %.4f)\n", i, s.type, s.surface,
               s.size[0], s.size[1], s.size[2], s.local[12], s.local[13], s.local[14]);
    }
    for (const VehicleData::Part& p : v.parts)
        for (const VehicleData::Shape& s : p.collision)
            printf("  part id 0x%x record %d: shape type %d surface %d size (%.4f %.4f %.4f) at (%.4f %.4f %.4f)\n", p.id,
                   p.record, s.type, s.surface, s.size[0], s.size[1], s.size[2], s.local[12], s.local[13], s.local[14]);
    for (size_t i = 0; i < part->collision.size(); ++i) {
        const VehicleData::Shape& s = part->collision[i];
        printf("  part shape %zu: type %d surface %d size (%.4f %.4f %.4f) at (%.4f %.4f %.4f)\n", i, s.type, s.surface,
               s.size[0], s.size[1], s.size[2], s.local[12], s.local[13], s.local[14]);
        printf("    rows (%.4f %.4f %.4f) (%.4f %.4f %.4f) (%.4f %.4f %.4f); PDL rows", s.local[0], s.local[1], s.local[2],
               s.local[4], s.local[5], s.local[6], s.local[8], s.local[9], s.local[10]);
        for (int k = 0; k < 16; ++k) printf(" %.4f", part->shapeMatrix[k]);
        printf("\n");
    }
    float inv[16];
    InverseRows(v.damage[record].matrix, false, inv);
    unsigned shown = 0;
    for (const auto& de : g_de) {
        const unsigned n = de.first;
        if (g_dumps["DEb2"].count(n) == 0 || de.second.size() < 3 || de.second[2] == 0) continue;
        const std::vector<uint32_t>& B = g_dumps["DEb2"][n].words;
        const std::vector<uint32_t>& A = g_dumps["DEb1"][n].words;
        const std::vector<uint32_t>& c = g_dumps["DEc"][n].words;
        if (B[0x1c0 / 4] == 0 || F(B[0x1c0 / 4]) > 100.0f) continue;  // the panel weighs 5
        if (shown++ >= limit) break;
        float car[16], pan[16];
        for (int i = 0; i < 16; ++i) { car[i] = F(A[4 + i]); pan[i] = F(B[4 + i]); }
        // the points are kept in the contact's frame: turned by the rows at
        // +0x30 (as 0x5054b0 does) they are offsets from the bodies
        float rows[3][4];
        for (int r = 0; r < 3; ++r)
            for (int j = 0; j < 4; ++j) rows[r][j] = F(c[(0x30 + 16 * r) / 4 + j]);
        const float la[3] = { F(c[0x80 / 4]), F(c[0x84 / 4]), F(c[0x88 / 4]) };
        const float lb[3] = { F(c[0xc0 / 4]), F(c[0xc4 / 4]), F(c[0xc8 / 4]) };
        float ta[3], tb[3];
        VecMat3(la, rows, ta);
        VecMat3(lb, rows, tb);
        // relative to the bodies' positions (+0x80 for type 0, else +0x40)
        const float* posA = reinterpret_cast<const float*>(&A[(A[0] == 0 ? 0x80 : 0x40) / 4]);
        const float* posB = reinterpret_cast<const float*>(&B[(B[0] == 0 ? 0x80 : 0x40) / 4]);
        printf("call %u: surfaces %u / %u, frame row0 (%.4f %.4f %.4f), A point (%.4f %.4f %.4f), B point (%.4f %.4f %.4f)\n"
               "   (matrix vs position: A %.4f %.4f %.4f, B %.4f %.4f %.4f)\n",
               n, c[0x90 / 4], c[0xd0 / 4], F(c[0]), F(c[1]), F(c[2]), posA[0] + ta[0], posA[1] + ta[1], posA[2] + ta[2],
               posB[0] + tb[0], posB[1] + tb[1], posB[2] + tb[2], car[12] - posA[0], car[13] - posA[1], car[14] - posA[2],
               pan[12] - posB[0], pan[13] - posB[1], pan[14] - posB[2]);
        for (int guess = 0; guess < 4; ++guess) {
            if (guess == 3) {
                // the game's space: the game turns every PDL shape row
                // (x, y, z) -> (x, z, -y) (run 16's live shapes); since
                // 2026-10-23 the program does too (vehicle.cpp)
                auto raw = [](const float* l, float* out) { memcpy(out, l, 16 * sizeof(float)); };
                // the bonnet's shape frame: 0 as the car's shapes, 1 the
                // PDL's own rows (z-up), 2 identity
                for (int frame = 0; frame < 3; ++frame) {
                    BodyContact best;
                    bool found = false;
                    size_t bi = 0;
                    for (size_t i = 1; i < v.collision.size(); ++i) {
                        float la2[16], lb2[16], wa[16], wb[16];
                        raw(v.collision[i].local, la2);
                        const float* l = part->collision[0].local;
                        if (frame == 0) raw(l, lb2);
                        else if (frame == 1)
                            for (int r2 = 0; r2 < 4; ++r2) {
                                lb2[4 * r2] = l[4 * r2];
                                lb2[4 * r2 + 1] = -l[4 * r2 + 2];
                                lb2[4 * r2 + 2] = l[4 * r2 + 1];
                                lb2[4 * r2 + 3] = l[4 * r2 + 3];
                            }
                        else
                            for (int k2 = 0; k2 < 16; ++k2) lb2[k2] = (k2 % 5 == 0) ? 1.0f : 0.0f;
                        Articulated::MatMul(la2, car, wa);
                        Articulated::MatMul(lb2, pan, wb);
                        BodyContact k;
                        const float* hs = part->collision[0].size;
                        const float swapped[3] = { hs[2], hs[1], hs[0] };
                        if (!BoxBoxContact(v.collision[i].size, wa, getenv("DMG_SWAP") ? swapped : hs, wb, k)) continue;
                        if (!found || best.depth < k.depth) { best = k; found = true; bi = i; }
                    }
                    if (found)
                        printf("   game space %d: shape %zu normal (%.4f %.4f %.4f) depth %.5f, A (%.4f %.4f %.4f) B (%.4f %.4f %.4f)\n",
                               frame, bi, best.normal[0], best.normal[1], best.normal[2], best.depth, best.pointA[0],
                               best.pointA[1], best.pointA[2], best.pointB[0], best.pointB[1], best.pointB[2]);
                    else
                        printf("   game space %d: no contact\n", frame);
                }
                continue;
            }
            BodyContact best;
            bool found = false;
            size_t bi = 0, bj = 0;
            for (size_t i = 1; i < v.collision.size(); ++i) {
                float wa[16];
                Articulated::MatMul(v.collision[i].local, car, wa);
                for (size_t j = 0; j < part->collision.size(); ++j) {
                    float l[16], wb[16];
                    if (guess == 0) Articulated::MatMul(part->collision[j].local, v.damage[record].matrix, l);
                    else if (guess == 1) Articulated::MatMul(part->collision[j].local, inv, l);
                    else memcpy(l, part->collision[j].local, sizeof(l));
                    Articulated::MatMul(l, pan, wb);
                    BodyContact k;
                    if (!BoxBoxContact(v.collision[i].size, wa, part->collision[j].size, wb, k)) continue;
                    if (guess == 2)
                        printf("      pair %zu (type %d surface %d) / %zu: depth %.5f normal (%.4f %.4f %.4f) A (%.4f %.4f %.4f)\n",
                               i, v.collision[i].type, v.collision[i].surface, j, k.depth, k.normal[0], k.normal[1],
                               k.normal[2], k.pointA[0], k.pointA[1], k.pointA[2]);
                    if (!found || best.depth < k.depth) { best = k; found = true; bi = i; bj = j; }
                }
            }
            if (found)
                printf("   guess %d: shapes %zu (surface %d) / %zu (surface %d) normal (%.4f %.4f %.4f) depth %.5f, A (%.4f %.4f %.4f) B (%.4f %.4f %.4f)\n",
                       guess, bi, v.collision[bi].surface, bj, part->collision[bj].surface,
                       best.normal[0], best.normal[1], best.normal[2], best.depth, best.pointA[0], best.pointA[1],
                       best.pointA[2], best.pointB[0], best.pointB[1], best.pointB[2]);
            else
                printf("   guess %d: no contact\n", guess);
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        printf("damagereplay <phys.txt> <Driv3r.exe> <miami.vvs> <miami.BO3> [first [count]]\n");
        return 1;
    }
    Read(argv[1]);
    std::vector<VehicleData> vehicles;
    if (!ReadVehicles(ReadAll(argv[3]), vehicles)) { printf("cannot read %s\n", argv[3]); return 1; }
    const VehicleData* v = nullptr;
    for (const VehicleData& x : vehicles)
        if (x.id == 6) v = &x;
    if (!v) { printf("no vehicle 6\n"); return 1; }
    float handling[kHandlingKeys];
    if (!VehicleHandling(ReadAll(argv[2]), ReadAll(argv[4]), 6, handling)) { printf("no handling\n"); return 1; }
    printf("handling [0x8a] %g [0x8b] %g [0x25] %g\n", handling[0x8a], handling[0x8b], handling[0x25]);
    if (getenv("DMG_PAIRS")) {
        RunPairs(*v, atoi(getenv("DMG_PAIRS")), 6);
        return 0;
    }
    const unsigned first = argc > 5 ? unsigned(atoi(argv[5])) : 0;
    const unsigned count = argc > 6 ? unsigned(atoi(argv[6])) : 100000;

    int calls = 0, equal = 0, tested = 0, requests = 0;
    // the dents (BOOT_TO_MENU.md section 76): the port's dented shape after
    // every call, against the game's live car shapes (D3PROXY_NARROW "NPah"
    // of pairs with A = the car's body), given in the game's space
    std::vector<std::pair<uint32_t, uint32_t>> dents;  // the dented axis' half size, the centre's game z
    int dentShape = 1, dentAxis = 0;
    for (const auto& de : g_de) {
        const unsigned n = de.first;
        if (n < first || n >= first + count) continue;
        const std::vector<uint32_t>& a = de.second;  // ecx, body A, body B, contact, impulse
        const uint32_t ecx = a[0];
        const uint32_t slotAddr = ecx - 0xc;
        const std::vector<uint32_t>& S = g_dumps["DEs"][n].words;
        const std::vector<uint32_t>& X = g_dumps["DXs"][n].words;
        const std::vector<uint32_t>& c = g_dumps["DEc"][n].words;
        const std::vector<uint32_t>& J = g_dumps["DEi"][n].words;
        const std::vector<uint32_t>& A = g_dumps["DEb1"][n].words;
        if (S.size() < 0x2600 / 4 || X.size() < 0x2600 / 4 || c.size() < 0x40 || A.size() < 0x78) continue;
        ++calls;
        const int nrec = int(S[0x488 / 4]);
        const float size[3] = { F(S[0x4a0 / 4]), F(S[0x4a4 / 4]), F(S[0x4a8 / 4]) };
        if (calls == 1 && !v->collision.empty()) {
            // the program's: twice the bounding shape's half sizes, PDL z-up
            const float* h = v->collision[0].size;
            const float mine[3] = { h[0] + h[0], h[2] + h[2], h[1] + h[1] };
            for (int i = 0; i < 3; ++i) Check("size from the PDL", n, i, W(mine[i]), W(size[i]));
        }
        VehicleDamage d;
        d.Init(*v, size, handling);
        if (int(d.records.size()) != nrec) { printf("call %u: %zu records, the slot has %d\n", n, d.records.size(), nrec); return 1; }
        for (int k = 0; k < nrec; ++k) LoadRecord(&S[(0x24 + 0x34 * k) / 4], d.records[k]);
        d.hitScale = F(S[0x1888 / 4]);
        d.tearScale = F(S[0x1890 / 4]);
        d.linked = int(S[0x18a4 / 4]);
        d.total = F(S[0x1f78 / 4]);
        d.glass = ((S[0x1f64 / 4]) & 0xff) != 0;
        d.front[0] = F(S[0x540 / 4]); d.front[1] = F(S[0x544 / 4]);
        d.back[0] = F(S[0x548 / 4]); d.back[1] = F(S[0x54c / 4]);
        // the hinged parts' bodies: their last dump up to this call's entry
        // (this call's own "DEb2" when the panel is the other body)
        const long entry = g_dumps["DEs"][n].order;
        for (int k = 0; k < nrec; ++k) {
            if (!d.records[k].body) continue;
            const BodySeen* best = nullptr;
            for (const BodySeen& b : g_bodies)
                if (b.addr == d.records[k].body && b.order < entry && (!best || b.order > best->order)) best = &b;
            if (best) {
                for (int i = 0; i < 16; ++i) d.records[k].bodyMatrix[i] = F(best->words[4 + i]);
                {
                    // the dump is the group solver's output: the step rebuilds
                    // the matrix from q and pos (0x5a9bf0) before the contacts
                    RigidBody b;
                    for (int i = 0; i < 4; ++i) {
                        b.pos[i] = F(best->words[0x80 / 4 + i]);
                        b.q[i] = F(best->words[0xa0 / 4 + i]);
                    }
                    b.UpdateMatrix();
                    memcpy(d.records[k].bodyMatrix, b.matrix, sizeof(b.matrix));
                }
            }
        }
        // 0x5054b0's point: A's position + the contact's point in A's frame
        // turned by the contact's rows (+0x30); the explosion bit (+0xf8) clear
        if (c[0xf8 / 4] & 1) { printf("call %u: explosion contact, skipped\n", n); continue; }
        const float* pos = reinterpret_cast<const float*>(&A[(A[0] != 0 ? 0x40 : 0x80) / 4]);
        const float local[3] = { F(c[0x80 / 4]), F(c[0x84 / 4]), F(c[0x88 / 4]) };
        float rows[3][4];
        for (int r = 0; r < 3; ++r)
            for (int j = 0; j < 4; ++j) rows[r][j] = F(c[(0x30 + 16 * r) / 4 + j]);
        float turned[3];
        VecMat3(local, rows, turned);
        const float point[3] = { pos[0] + turned[0], pos[1] + turned[1], pos[2] + turned[2] };
        float car[16];
        for (int i = 0; i < 16; ++i) car[i] = F(A[4 + i]);
        const float carMass = F(A[0x1c0 / 4]);
        const float Jv[3] = { F(J[0]), F(J[1]), F(J[2]) };
        std::vector<DamageRequest> req;
        std::vector<VehicleData::Shape> shapes = v->collision;
        d.InitDent(6, &shapes, handling);
        if (calls == 1) {
            Check("slot +1f6a", n, -1, uint32_t(d.dentShape), (S[0x1f68 / 4] >> 16) & 0xff);
            Check("slot +1f6b", n, -1, uint32_t(d.dentAxis), (S[0x1f68 / 4] >> 24) & 0xff);
            Check("slot +1f6c", n, -1, W(d.dentScale[0]), S[0x1f6c / 4]);
            Check("slot +1f70", n, -1, W(d.dentScale[1]), S[0x1f70 / 4]);
        }
        // the surface rename (0x4e9160, BOOT_TO_MENU.md section 77): the
        // car's side was 0xb before the callback; its bottom is slot
        // +0x1894, the other side "static" when its +4 is -1 (the ground)
        {
            const std::vector<uint32_t>& B = g_dumps["DEb2"][n].words;
            const float nrm[3] = { F(c[0x50 / 4]), F(c[0x54 / 4]), F(c[0x58 / 4]) };
            const bool ground = B.size() > 1 && B[1] == 0xffffffffu;
            const int mine = VehicleSurface(0xb, 0, point, car, F(S[0x1894 / 4]), nrm, ground);
            ++g_renames;
            if (static_cast<uint32_t>(mine) != c[0x90 / 4]) {
                ++g_bad;
                if (g_shown++ < 60) printf("  call %u: surface port %x, trace %x\n", n, mine, c[0x90 / 4]);
            }
            if (calls == 1 && !v->collision.empty())
                Check("bottom (slot +0x1894)", n, -1, W(VehicleBottom(v->collision[0])), S[0x1894 / 4]);
        }
        d.Impact(*v, car, carMass, -1, carMass, Jv, point, int(c[0xd0 / 4]), req);
        if (d.dentShape >= 0) {
            dentShape = d.dentShape;
            dentAxis = d.dentAxis;
            const VehicleData::Shape& sh = shapes[d.dentShape];
            const std::pair<uint32_t, uint32_t> dent(W(sh.size[d.dentAxis]), W(sh.local[14]));
            if (dents.empty() || dents.back() != dent) dents.push_back(dent);
        }

        // the part tests' arguments of this call
        bool sawPa = false;
        for (const auto& pa : g_pa) {
            if (g_paCall[pa.first] != n) continue;
            const std::vector<uint32_t>& p = pa.second;  // eax, a1..a12
            if (!sawPa) {
                sawPa = true;
                ++tested;
                Check("s", n, -1, W(d.lastS), p[4]);
                Check("side", n, -1, W(d.lastSide), p[6]);
                for (int i = 0; i < 3; ++i) Check("point", n, i, W(d.lastPoint[i]), p[8 + i]);
                const std::vector<uint32_t>& v5 = g_dumps["PAv5"][pa.first].words;
                for (int i = 0; i < 3; ++i) Check("dir", n, i, W(d.lastDir[i]), v5[i]);
            }
            // the part and record the game tested
            const uint32_t part = p[2], rec = p[3];
            const int k = int((rec - (slotAddr + 0x24)) / 0x34);
            const int pi = int((part - (S[0x454 / 4] + 0x30)) / 0x20);
            if (pi < 0 || pi >= int(v->parts.size()) || v->parts[pi].record != k)
                printf("  call %u: PA part %d record %d do not match the UPVH\n", n, pi, k);
            if (d.records[k].state >= 2 && d.records[k].body) {
                const std::vector<uint32_t>& m7 = g_dumps["PAm7"][pa.first].words;
                float carInv[16], M[16];
                InverseRows(car, false, carInv);
                Articulated::MatMul(d.records[k].bodyMatrix, carInv, M);
                for (int i = 0; i < 16; ++i) Check("hinged M", n, i, W(M[i]), m7[i]);
            }
        }
        // the records after the call (fields the state machine writes are
        // left out where the trace's state changed)
        bool same = true;
        const int before = g_bad;
        for (int k = 0; k < nrec; ++k) {
            const uint32_t* w = &X[(0x24 + 0x34 * k) / 4];
            const uint32_t* w0 = &S[(0x24 + 0x34 * k) / 4];
            const DamageRecord& r = d.records[k];
            const int traced = int(w[3] & 0xffff), was = int(w0[3] & 0xffff);
            Check("rec +4", n, k, W(r.half[0]), w[1]);
            Check("rec +8", n, k, W(r.half[1]), w[2]);
            Check("rec +1c", n, k, W(r.cooldown), w[7]);
            Check("rec +20", n, k, W(r.impact), w[8]);
            Check("rec +2c", n, k, W(r.dent), w[11]);
            Check("rec +30", n, k, uint32_t(r.stage) | uint32_t(r.worst) << 8, w[12] & 0xffff);
            int asked = 0;
            for (const DamageRequest& q : req)
                if (q.record == k) asked = q.state;
            if (traced != was || asked) {
                ++requests;
                printf("  call %u record %d: state %d -> %d in the trace, port asks %d\n", n, k, was, traced, asked);
                if (traced != (asked ? asked : was)) ++g_bad;
            }
        }
        Check("slot +540", n, -1, W(d.front[0]), X[0x540 / 4]);
        Check("slot +544", n, -1, W(d.front[1]), X[0x544 / 4]);
        Check("slot +548", n, -1, W(d.back[0]), X[0x548 / 4]);
        Check("slot +54c", n, -1, W(d.back[1]), X[0x54c / 4]);
        Check("slot +1f78", n, -1, W(d.total), X[0x1f78 / 4]);
        Check("slot +1f64", n, -1, d.glass ? 1u : 0u, X[0x1f64 / 4] & 0xff);
        same = g_bad == before;
        if (same) ++equal;
    }
    // the game's distinct dented shapes in the order seen (the car's shape
    // set: the first NPah of each pair is side A, the car)
    if (!dents.empty() && !g_dumps["NPah"].empty()) {
        int found = 0, seen = 0;
        std::pair<uint32_t, uint32_t> last(0, 0);
        for (const auto& np : g_dumps["NPah"]) {
            const std::vector<uint32_t>& w = np.second.words;
            if (w.size() < size_t(dentShape + 1) * 24) continue;
            const uint32_t* x = &w[dentShape * 24];
            const std::pair<uint32_t, uint32_t> got(x[dentAxis], x[4 + 14]);
            if (got == last) continue;
            last = got;
            ++seen;
            bool ok = false;
            for (const auto& dd : dents) ok = ok || dd == got;
            if (ok) ++found;
            printf("  the game's shape %d after NP %u: half %.6f, centre z %.6f: %s\n", dentShape, np.first,
                   F(got.first), F(got.second), ok ? "made by the port too" : "NOT among the port's");
        }
        printf("dents: %d of the game's %d shape states made by the port (%zu port states)\n", found, seen,
               dents.size());
    }
    printf("surface renames: %d checked\n", g_renames);
    printf("%d calls: %d equal, %d with part tests, %d state changes; %d differences\n", calls, equal, tested, requests,
           g_bad);
    return g_bad ? 2 : 0;
}
