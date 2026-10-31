// The damage marks (marks.cpp) against the game: the placer 0x50bd20 and the
// re-placer 0x4f5890 called through gamecode.h on hand-built game objects
// (the marks object, the world's buffer pool, the records, a slot with the
// vtable entry 0x50bd20 asks for when a mesh's 8 regions are full), with the
// port doing the same steps on CarMarks / MarkPool. After every step the
// two must agree: region bytes, marks per region, buffers in use (which pool
// buffer), buffer counts / write index, every quad's 48 floats, the
// record's +0x30 / +0x31.
//
//   marktest <Driv3r.exe> [Miami.vvs] [steps]
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <random>

#include "../marks.h"
#include "../vehicle.h"
#include "gamecode.h"

namespace {

// The buffers' render object: vtbl+0x2c gets the buffer's draw size (one
// stack argument).
void __fastcall RenderSize(void*, void*, void*) {}
void* g_renderVtbl[16];
struct RenderObject { void** vtbl; } g_render = { g_renderVtbl };

// The slot (0x50bd20's last argument): vtbl+0x40(&out) returns a
// pointer to a pointer to an object whose +4 is the vehicle (+0x828 the
// marks block) for 0x4f5b10.
uint8_t g_vehicle[0x900];
uint8_t g_vehicleRef[8];
void* g_vehicleRefPtr = g_vehicleRef;
// (one stack argument: the side and region pushed before it are already
// 0x4f5b10's arguments, 0x50bec2..)
void* __fastcall SlotRef(void*, void*, void*) { return &g_vehicleRefPtr; }
// The one-car world of 0x50f270 (pool empty): the slot in the world's
// vehicle list; vtbl+0xdc says it has marks, its +4 subobject's vtbl+4 gives
// its position, vtbl+0xd8 is the game's 0x4f6670, which reads the car's
// matrix through slot +0x440 vtbl+0xb4.
float g_car[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100, 2, -50, 1 };
int __fastcall SlotHasMarks(void*, void*) { return 1; }
void* __fastcall SlotPosition(void*, void*, float* out)
{
    memcpy(out, g_car + 12, 16);
    return out;
}
void* __fastcall CarMatrix(void*, void*, float* out)
{
    memcpy(out, g_car, 64);
    return out;
}
void* g_posVtbl[4];
void* g_carVtbl[0x40];
void** g_carObj = g_carVtbl;
void* g_slotVtbl[0x60];
alignas(16) uint8_t g_slot[0x2600];

struct GameBuffer
{
    uint32_t cap, data, count, at;
    uint16_t word;
    uint16_t pad;
    uint32_t material, render;
    uint8_t own, pad2[3];
};

uint32_t U(const void* p) { return uint32_t(reinterpret_cast<uintptr_t>(p)); }
uint32_t Rd32(const uint8_t* p)
{
    uint32_t x;
    memcpy(&x, p, 4);
    return x;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: marktest <Driv3r.exe> [Miami.vvs] [steps]\n");
        return 1;
    }
    if (!GameCode::Load(argv[1])) {
        printf("cannot load %s\n", argv[1]);
        return 1;
    }
    const char* vvsPath = argc > 2 ? argv[2] : "D:\\l\\gta-vc\\DRIV3R\\Vehicles\\Miami.vvs";
    const int steps = argc > 3 ? atoi(argv[3]) : 4000;
    std::ifstream f(vvsPath, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<VehicleData> vehicles;
    if (!ReadVehicles(file, vehicles)) {
        printf("cannot read %s\n", vvsPath);
        return 1;
    }
    g_renderVtbl[0x2c / 4] = reinterpret_cast<void*>(&RenderSize);
    g_slotVtbl[0x40 / 4] = reinterpret_cast<void*>(&SlotRef);
    *reinterpret_cast<void***>(g_slot) = g_slotVtbl;
    // slot +0x444 -> +4 the vehicle (+0x828 the marks block, +0xc its model
    // -> +0x14 the id for 0x5140d0); 0x50bd20's vtbl+0x40 gives the same
    static uint8_t vmodel[0x20];
    const uint32_t vehicleAddr = U(g_vehicle), vmodelAddr = U(vmodel), wrapAddr = U(g_vehicleRef);
    memcpy(g_vehicleRef + 4, &vehicleAddr, 4);
    memcpy(g_vehicle + 0xc, &vmodelAddr, 4);
    memcpy(g_slot + 0x444, &wrapAddr, 4);
    g_slotVtbl[0xdc / 4] = reinterpret_cast<void*>(&SlotHasMarks);
    g_slotVtbl[0xd8 / 4] = reinterpret_cast<void*>(uintptr_t(GameCode::At(0x4f6670)));
    g_posVtbl[1] = reinterpret_cast<void*>(&SlotPosition);
    void** posObj = g_posVtbl;
    memcpy(g_slot + 4, &posObj, 4);
    g_carVtbl[0xb4 / 4] = reinterpret_cast<void*>(&CarMatrix);
    const uint32_t carObjAddr = U(&g_carObj);
    memcpy(g_slot + 0x440, &carObjAddr, 4);

    int totalSteps = 0, diffs = 0, placed = 0, glassSteps = 0, freed = 0, replaced = 0;
    for (const VehicleData& v : vehicles) {
        if (v.markPoints.empty()) continue;
        const int meshes = int(v.markFirst.size());
        // The game's marks block: total, count, firsts, then the points
        // 16-byte aligned (as 0x4f4d80 finds them).
        std::vector<uint8_t> blockMem(16 + 8 + meshes * 4 + 16 + v.markPoints.size() * 20 + 16);
        uint8_t* block = blockMem.data() + ((16 - (U(blockMem.data()) & 15)) & 15);
        const uint32_t total = uint32_t(v.markPoints.size()), count = uint32_t(meshes);
        memcpy(block, &total, 4);
        memcpy(block + 4, &count, 4);
        for (int i = 0; i < meshes; ++i) memcpy(block + 8 + 4 * i, &v.markFirst[size_t(i)], 4);
        const uint32_t a = (count * 4 - 8) & 0xf, pad = 0x10 - a < 0x10 ? 0x10 - a : 0;
        uint8_t* points = block + pad + count * 4 + 8;
        if ((U(points) & 15) != 0) {
            printf("vehicle %d: points not aligned\n", v.id);
            return 1;
        }
        for (size_t i = 0; i < v.markPoints.size(); ++i) {
            const VehicleData::MarkPoint& q = v.markPoints[i];
            memcpy(points + i * 20, &q.rest, 4);
            memcpy(points + i * 20 + 4, &q.dented, 4);
            memcpy(points + i * 20 + 8, q.normal, 8);
            memcpy(points + i * 20 + 16, &q.weight, 2);
        }
        const uint32_t blockAddr = U(block);
        memcpy(g_vehicle + 0x828, &blockAddr, 4);
        {
            const uint32_t id = uint32_t(v.id);
            memcpy(vmodel + 0x14, &id, 4);
        }
        uint32_t iter[2] = { blockAddr, U(points) };

        // The world (0x8ac45c -> +4): the pool stacks at +0xf30 + side x 0x44.
        static uint8_t world[0x1100];
        static uint8_t manager[8];
        memset(world, 0, sizeof(world));
        {
            const uint32_t slotAddr = U(g_slot), one = 1;
            memcpy(world + 0xc, &slotAddr, 4);  // the world's vehicles: this slot
            memcpy(world + 0x30c, &one, 4);
        }
        const uint32_t worldAddr = U(world);
        memcpy(manager + 4, &worldAddr, 4);
        const uint32_t managerAddr = U(manager);
        memcpy(reinterpret_cast<void*>(uintptr_t(GameCode::At(0x8ac45c))), &managerAddr, 4);
        MarkPool pool;
        static GameBuffer gb[20];
        static uint8_t gdata[20][16 * 0xc0];
        std::map<uint32_t, int> gameIndex;
        for (int i = 0; i < 20; ++i) {
            gb[i] = GameBuffer();
            gb[i].cap = 16;
            gb[i].data = U(gdata[i]);
            gb[i].render = U(&g_render);
            memset(gdata[i], 0, sizeof(gdata[i]));
            // 0x4f33a0: every vertex's colour 1, 1, 1, 1
            for (int vx = 0; vx < 64; ++vx)
                for (int k = 0; k < 4; ++k) {
                    const float one = 1.0f;
                    memcpy(gdata[i] + vx * 0x30 + 0x20 + 4 * k, &one, 4);
                }
            gameIndex[U(&gb[i])] = i;
        }
        for (int side = 0; side < 2; ++side) {
            uint8_t* base = world + 0xf30 + side * 0x44;
            uint32_t top = U(base);
            for (int i : pool.free[side]) {
                const uint32_t p = U(&gb[i]);
                memcpy(reinterpret_cast<void*>(uintptr_t(top)), &p, 4);
                top += 4;
            }
            memcpy(base + 0x40, &top, 4);
        }
        // The marks object (0x4f53d0 / 0x4f5480) and the port's.
        CarMarks port;
        port.Init(v);
        port.active = true;
        static uint8_t M[0x1000];
        memset(M, 0, sizeof(M));
        memset(M + 4, 0xff, 800);
        {
            const uint32_t mAddr = U(M);
            memcpy(g_slot + 0x550, &mAddr, 4);
        }
        for (int m = 0; m < CarMarks::kMeshes; ++m) memcpy(M + 0x324 + m * 0x60, port.anchor[m], 16);

        std::vector<DamageRecord> records(v.damage.size());
        static uint8_t grec[0x14][0x34];
        memset(grec, 0, sizeof(grec));
        const float* car = g_car;

        std::mt19937 rng(uint32_t(v.id) * 7919u + 1);
        auto syncRecord = [&](int m) {
            memcpy(grec[m] + 4, &records[size_t(m)].half[0], 4);
            memcpy(grec[m] + 8, &records[size_t(m)].half[1], 4);
            grec[m][0x30] = records[size_t(m)].stage;
            grec[m][0x31] = records[size_t(m)].worst;
        };
        auto compare = [&](int step, const char* what) {
            int bad = 0;
            for (size_t i = 0; i < v.markPoints.size(); ++i)
                if (M[4 + i] != port.region[i]) {
                    if (++bad <= 3) printf("  point %zu region game %u port %u\n", i, M[4 + i], port.region[i]);
                }
            uint32_t used;
            memcpy(&used, M, 4);
            if (used != port.used) ++bad, printf("  used game %u port %u\n", used, port.used);
            for (int m = 0; m < meshes; ++m) {
                for (int s = 0; s < 2; ++s) {
                    uint32_t ps;
                    memcpy(&ps, M + 0x37c + m * 0x60 + s * 4, 4);
                    if (ps != port.perSide[m][s]) ++bad, printf("  mesh %d side %d per-side game %u port %u\n", m, s, ps, port.perSide[m][s]);
                    for (int r = 0; r < 8; ++r) {
                        uint32_t c, bp;
                        memcpy(&c, M + 0x334 + m * 0x60 + (s * 8 + r) * 4, 4);
                        memcpy(&bp, M + 0xaa4 + m * 0x40 + s * 0x20 + r * 4, 4);
                        if (c != port.marks[m][s][r]) ++bad, printf("  mesh %d side %d region %d marks game %u port %u\n", m, s, r, c, port.marks[m][s][r]);
                        const int gi = bp ? gameIndex[bp] : -1;
                        if (gi != port.buffer[m][s][r]) {
                            ++bad, printf("  mesh %d side %d region %d buffer game %d port %d\n", m, s, r, gi, port.buffer[m][s][r]);
                            continue;
                        }
                        if (gi < 0) continue;
                        const MarkBuffer& pb = pool.buffers[size_t(gi)];
                        if (gb[gi].count != pb.count || gb[gi].at != pb.at)
                            ++bad, printf("  buffer %d count/at game %u/%u port %u/%u\n", gi, gb[gi].count, gb[gi].at, pb.count, pb.at);
                        for (uint32_t qd = 0; qd < 16; ++qd)
                            if (memcmp(gdata[gi] + qd * 0xc0, &pb.v[qd * 4], 0xc0) != 0) {
                                if (++bad <= 6) {
                                    const float* g = reinterpret_cast<const float*>(gdata[gi] + qd * 0xc0);
                                    const float* p = &pb.v[qd * 4].pos[0];
                                    printf("  buffer %d quad %u differs:", gi, qd);
                                    for (int k = 0; k < 48; ++k)
                                        if (GameCode::Bits(g[k]) != GameCode::Bits(p[k])) printf(" [%d] %.9g/%.9g", k, g[k], p[k]);
                                    printf("\n");
                                }
                            }
                    }
                }
                if (grec[m][0x30] != records[size_t(m)].stage || grec[m][0x31] != records[size_t(m)].worst)
                    ++bad, printf("  record %d stage game %u/%u port %u/%u\n", m, grec[m][0x30], grec[m][0x31], records[size_t(m)].stage, records[size_t(m)].worst);
            }
            if (bad) {
                ++diffs;
                if (diffs <= 10) printf("vehicle %d step %d (%s): %d differences\n", v.id, step, what, bad);
            }
            return bad == 0;
        };

        for (int step = 0; step < steps; ++step, ++totalSteps) {
            const int m = int(rng() % uint32_t(meshes));
            uint32_t first, end;
            MarkRange(v, m, first, end);
            const uint32_t op = rng() % 10;
            if (op == 0) {
                // the dent moves: new halves, then 0x4f5890
                for (int k = 0; k < 2; ++k) records[size_t(m)].half[k] = float(rng() % 1001) / 1000.0f;
                syncRecord(m);
                const uint32_t recAddr = U(grec[m]);
                if (!GameCode::Call(0x4f5890, GameRegs(), { U(M), uint32_t(m), recAddr, U(iter), 0, U(car), U(g_slot) })) {
                    printf("vehicle %d step %d: 0x4f5890 faulted at %08x (data %08x)\n", v.id, step,
                           GameCode::Game(GameCode::g_faultAddr), GameCode::g_faultData);
                    return 1;
                }
                // the game's +0x374 / +0x378 are the port's last[]
                float lastG[2];
                memcpy(lastG, M + 0x374 + m * 0x60, 8);
                port.Replace(v, pool, m, records[size_t(m)], car);
                if (memcmp(lastG, port.last[m], 8) != 0) printf("  last differs\n");
                ++replaced;
                if (!compare(step, "replace")) break;
                continue;
            }
            if (first == end) continue;
            const int p = int(first + rng() % (end - first));
            // glass records stay where 0x4f5190 does not call further
            // (+0x31 below 2, or +0x30 already 2)
            DamageRecord& r = records[size_t(m)];
            if (r.worst >= 2 && r.stage < 2) r.worst = uint8_t(rng() % 2), r.stage = 0;
            if (rng() % 8 == 0) r.stage = 2, r.worst = uint8_t(rng() % 4);
            const bool withRecord = rng() % 6 != 0;
            syncRecord(m);
            const uint32_t usedBefore = port.used;
            const uint8_t before = port.region[p];
            if (!GameCode::Call(0x50bd20, GameRegs(),
                                { U(M), uint32_t(p), uint32_t(m), withRecord ? U(grec[m]) : 0u, U(iter), 0, 0, U(car), U(g_slot) })) {
                printf("vehicle %d step %d: 0x50bd20 faulted at %08x (data %08x)\n", v.id, step,
                       GameCode::Game(GameCode::g_faultAddr), GameCode::g_faultData);
                return 1;
            }
            const uint8_t worstBefore = r.worst;
            port.Place(v, pool, p, m, withRecord ? &r : nullptr, car);
            if (before == 0xff && port.region[p] != 0xff) ++placed;
            if (r.worst != worstBefore) ++glassSteps;
            if (port.used < usedBefore) ++freed;
            if (!compare(step, "place")) break;
        }
    }
    printf("marks: %d steps (%d placed, %d glass steps, %d re-placements, %d frees), %d differing steps\n", totalSteps,
           placed, glassSteps, replaced, freed, diffs);
    return diffs ? 2 : 0;
}
