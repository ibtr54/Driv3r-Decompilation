// The drawn dent (damage.cpp VehicleDent / DamageShowsDent) against the
// game: the part test 0x4f4510 (ecx = the part's type, eax = its id, the
// part's +0x08 flags on the stack, the answer in al) called through
// gamecode.h for every type 0..15, id 0..0x7f and a set of flags; then the
// clamp of 0x4fd7f0 (0x4fe36b, inline in the draw: not callable) on the
// special values. Also lists the dent of every record part of each vehicle
// in Vehicles\Miami.vvs with a record half of 0.5.
//
//   denttest <Driv3r.exe> [Miami.vvs]
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "../damage.h"
#include "../vehicle.h"
#include "gamecode.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: denttest <Driv3r.exe> [Miami.vvs]\n");
        return 1;
    }
    if (!GameCode::Load(argv[1])) {
        printf("cannot load %s\n", argv[1]);
        return 1;
    }
    const uint32_t flags[] = { 0, 1, 2, 3, 0x100, 0x101, 0x40000185, 0x40000184, 0x3f0001, 0x800000, 0xffffffff };
    int tests = 0, diffs = 0;
    for (int type = 0; type < 16; ++type)
        for (int id = 0; id < 0x80; ++id)
            for (uint32_t f : flags) {
                GameRegs g;
                g.ecx = uint32_t(type);
                g.eax = uint32_t(id);
                GameRegs out;
                if (!GameCode::Call(0x4f4510, g, { f }, &out)) {
                    printf("the call faulted (type %d id 0x%x flags 0x%x)\n", type, id, f);
                    return 1;
                }
                const bool game = (out.eax & 0xff) != 0, port = DamageShowsDent(type, id, f);
                ++tests;
                if (game != port) {
                    if (++diffs <= 20) printf("DIFF type %d id 0x%x flags 0x%x: game %d port %d\n", type, id, f, game, port);
                }
            }
    printf("0x4f4510: %d cases, %d differences\n", tests, diffs);

    // The clamp: 0 below 0, 1 above 1, the value itself otherwise (the x87
    // compares of 0x4fe36b..0x4fe5a9 let -0 and NaN through).
    VehicleData::Part part;
    part.type = 1;
    part.id = 0x26;
    part.flags8 = 1;
    const float in[] = { -1.0f, -0.0f, 0.0f, 0.25f, 1.0f, 1.5f, NAN };
    const float want[] = { 0.0f, -0.0f, 0.0f, 0.25f, 1.0f, 1.0f, NAN };
    int clampDiffs = 0;
    for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); ++i) {
        DamageRecord r;
        r.half[0] = in[i];
        r.half[1] = 0.75f;  // the second half is not drawn
        const float d = VehicleDent(part, r);
        if (GameCode::Bits(d) != GameCode::Bits(want[i])) {
            ++clampDiffs;
            printf("DIFF clamp %g: %g\n", in[i], d);
        }
    }
    printf("clamp: %zu values, %d differences\n", sizeof(in) / sizeof(in[0]), clampDiffs);

    if (argc > 2) {
        std::ifstream file(argv[2], std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<VehicleData> vehicles;
        if (!ReadVehicles(d, vehicles)) {
            printf("cannot read %s\n", argv[2]);
            return 1;
        }
        DamageRecord half;
        half.half[0] = 0.5f;
        for (const VehicleData& v : vehicles) {
            printf("vehicle %d:", v.id);
            for (const VehicleData::Part& p : v.parts)
                if (p.type == 1 && p.record >= 0) printf(" %d:0x%x=%g", p.record, p.id, VehicleDent(p, half));
            printf("\n");
        }
    }
    return diffs || clampDiffs ? 2 : 0;
}
