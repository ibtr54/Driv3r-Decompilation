// The player's vehicle (BOOT_TO_MENU.md section 58). Vehicles\<City>.vvs
// (game setup vtbl+0x3c) is a CHNK file: one entry per vehicle, tagged with
// the vehicle id, holding the vehicle's UPVH chunk, then the vehicles' MDPC
// packages in the same order (the k-th entry's models are the k-th MDPC).
//
// UPVH (read by 0x5126a9.., the vehicle object at ebx):
//   +0x00 'AWHF', +0x0c version (>= 5), +0x10 part count, +0x14 vehicle id,
//   +0x20 / +0x22 / +0x24 / +0x26 u16 counts of the tables after the parts;
//   +0x30 the parts (0x20 each, [+0x1c]), then the hardpoints (+0x22 x 0x20:
//   vec4 position, vec4 axis; [+0x24]), +0x24 x 0x50 ([+0x20]), +0x20 x
//   0x10 ([+0x28]), +0x26 x 0x40 ([+0x2c]), then the physics block
//   "PDL001.002.003a" ([+0x820], relocated by 0x5a5a50: +0 entry count, +8
//   entry table, 16-byte entries {shape count, shape offset from the entry};
//   a shape is 0x60 bytes: size vec4, 4 x 4 matrix, material).
//   Part (0x20): +0x00 u16 type (1 body, 2 wheel, 8 / 3 / 9 / 4 others),
//   +0x02 u16 id, +0x12 u16 0xfc00 | model index (no model otherwise), +0x14
//   u16 physics entry, +0x18 u16 hardpoint. A wheel (0x512bfc): its slot
//   from the id (0x1c -> 0, 0x1e -> 1, 0x1b -> 2, 0x1d -> 3, 0x2b -> 4,
//   0x2a -> 5; table 0x5133b4 / 0x513398), the hardpoint copied to the
//   vehicle's +0x89c + slot x 16, the first two floats of its physics
//   entry's shape to +0x8fc / +0x914 + slot x 4 (radius, width).
#pragma once

#include <cstdint>
#include <vector>

#include "mdpc.h"

struct VehicleWheel
{
    int model = -1;
    int slot = -1;            // 0..5 (0x512c24), -1: an id without a slot
    float hardpoint[3] = {};  // in the vehicle's space
    float radius = 0, width = 0;
};

struct VehicleData
{
    int id = 0;
    int mdpc = -1;            // the file's k-th MDPC chunk
    std::vector<int> body;    // models of the parts of type 1, in part order
    std::vector<VehicleWheel> wheels;  // the parts of type 2, in part order
    // The exhaust tips (x, y, z in the vehicle's space, part after part):
    // the parts of type 3 with id 6, whose u16 +0x16 indexes the table of
    // +0x20 x 0x10 (vec4 points). Vehicle 6: four tips (+-0.300 / +-0.443,
    // -0.133, -2.256), where the original's four exhaust emitters sit
    // (section 60; the code that copies them to the vehicle's +0x1d4
    // entries was not found).
    std::vector<float> exhausts;
};

// Every vehicle entry of a .vvs file (false: not one).
bool ReadVehicles(const std::vector<uint8_t>& file, std::vector<VehicleData>& out);

// The vehicle as the game creates it (0x50e940, from 0x476480 for Take A
// Ride): pose (x, y, z, heading), the vehicle faces (sin h, 0, cos h); on
// the ground (0x4e9da0 within 10 units of the pose) at the ground height
// plus 0x513c10 = radius - (hardpoint y + compression) of the wheel in slot
// 0 (the first slot with a wheel), level; the wheels at their hardpoints
// moved up by the compression, not turned. The traced creation frame
// (start point 0, vehicle 6) has exactly this. compression: the wheel slots'
// lift, RestCompression + WheelOffset (0x5115c0 adds the slot's offset to
// the hardpoint; 0x513c10 subtracts 0x5138c0).
// Row-vector matrices (Direct3D): body, then 16 floats per wheel in
// VehicleData::wheels order, to be applied to the models' vertices as
// stored (the wheel model sits at one hardpoint, kept in the 4th row of its
// slot 0 submesh matrix as (x, -z, y): the others get it moved there, turned 180
// degrees about y when on the other side; traced at the creation frame).
void VehicleCreationPose(const VehicleData& v, const MdpcPackage& p, const float pose[4], float ground, float compression,
                         float body[16], std::vector<float>& wheels, float& height);

// The handling (BOOT_TO_MENU.md section 61): the vehicle model's table of
// 190 floats at +0x102c (0x511780): the default block of the id (0x5142a0:
// 0x7a3570 for the cars, others for ids 0x13..0x5b, some of them built in
// .bss by static initialisers), then the id's record of
// Configs\Vehicles\BigVO3\<city>.BO3 ('BVO3', version 2, u16 offsets per id
// at +0x20 from +0x108 to 'DVO3' records: count at +8, then {u16 key, 0xaaaa,
// f32 value} up to 0xaaaaffff; keys >= 0xbe skipped, 0x514360); the loader's
// vtbl+0x48 afterwards changed nothing in the traces. Key 0 is the mass, 1
// the spring, 3 and 0x19 the wheel offset, 0x8c the gravity term. False when
// the executable is not the one decoded.
const int kHandlingKeys = 0xbe;
bool VehicleHandling(const std::vector<uint8_t>& exe, const std::vector<uint8_t>& bo3, int id, float out[kHandlingKeys]);
// 0x513d20: the vehicle model's class (+8) by id: 0x115, 0x116 (boats), 0x81
// (id 0x31), 0x82 (id 0x56), else 0x11b (the cars).
int HandlingClass(int id);
// 0x5138c0: the static compression, -[0x8c] x [0] x 0.01 / [1], clamped to
// 0..1 (vehicle 6: 0.104801).
float RestCompression(const float t[kHandlingKeys]);
// 0x511780: every wheel slot's offset (y of +0x92c + 0x10 x slot):
// -[3] x [0x19] for class 0x11b, else 0 (vehicle 6: -0.0757526).
float WheelOffset(const float t[kHandlingKeys], int cls);
