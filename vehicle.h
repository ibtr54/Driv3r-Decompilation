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
    // The burst tyre's rim (BOOT_TO_MENU.md section 67): the model of the
    // first part of type 8 and id 0x48 (0x5139f0, kept by the slot at
    // +0x17f4 by 0x503da7); -1: none. Vehicle 6: part 1, model 23, centred
    // at the origin (the wheel models sit at a hardpoint).
    int rim = -1;
    // The body's collision shapes (BOOT_TO_MENU.md section 65): the PDL
    // entry of the part of type 4 (entry 0 in every vehicle checked; vehicle
    // 6: a bounding box and four boxes of surface 11), as 0x5a0b60 gives
    // them to the body. local: the shape's matrix in the vehicle's space
    // (turned from the PDL's z-up space, vehicle.cpp).
    struct Shape
    {
        int type = 0, surface = 0;  // +0x50: 0 box, 1 cylinder, 2 sphere; +0x51
        float size[3] = {};
        float local[16] = {};
    };
    std::vector<Shape> collision;
    // The damage parts (BOOT_TO_MENU.md section 72): every part (u16 +0x00
    // type, +0x02 id, +0x14 physics entry, +0x18 hardpoint, +0x1a damage
    // record; 0xffff: none), the hardpoints (point and axis in the vehicle's
    // space) and the damage records' entries (+0x24 x 0x50: the part's
    // matrix in the vehicle, its inertia diagonal and mass).
    // For the impact test (0x503fb0 / 0x5099c0, damage.h) also +0x04 (bit 0:
    // no children; the walk steps over +0x0c parts otherwise), +0x08 (flags
    // the damage predicates read: 0x400000 / 0x800000 group, 0x3f0000 and
    // bit 0 hinge, bit 1 tear), +0x0c (the part and its children) and the
    // physics entry's first shape (PDL, as stored: size and matrix).
    struct Part
    {
        int type = 0, id = 0, entry = -1, hardpoint = -1, record = -1;
        uint32_t flags4 = 0, flags8 = 0;
        int span = 1;
        int shapes = 0;
        float shapeSize[3] = {};
        float shapeMatrix[16] = {};
        // All the entry's shapes as the body's collision shapes are kept
        // (local: the PDL matrix turned to y-up, in the vehicle's space).
        std::vector<Shape> collision;
    };
    std::vector<Part> parts;
    struct Hardpoint
    {
        float pos[4] = {}, axis[4] = {};
    };
    std::vector<Hardpoint> hardpoints;
    struct DamageEntry
    {
        float matrix[16] = {};
        float inertia[3] = {};
        float mass = 0;
    };
    std::vector<DamageEntry> damage;
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
// 0x513db0 (the draw's wheel lift, BOOT_TO_MENU.md section 66): for class
// 0x11b min(compression, [0xbc]) - [0x19] x [3] (the default [0xbc] is 0.3),
// else 0. At rest it is RestCompression + WheelOffset.
float WheelLift(const float t[kHandlingKeys], int cls, float compression);

// A wheel's World relative to the body (0x4ff530 and the draw's placement
// matrix, BOOT_TO_MENU.md section 66): (vertex - pivot) x turn x RotX(spin) x
// RotY(steer) + hardpoint + (0, lift, 0); the pivot and the turn as in
// VehicleCreationPose. The game: placement (slot+0x464, the part's +0x1c) x
// T(-h) x rotation x T(0, lift, 0) x T(h) x body, h the part's hardpoint;
// the traced creation frame fixes the placement to T(-pivot) x turn x T(h).
// D3DX rotations (row vectors): RotX rows (1 0 0) (0 c s) (0 -s c), RotY
// rows (c 0 -s) (0 1 0) (s 0 c).
void VehicleWheelLocal(const VehicleWheel& w, const MdpcPackage& p, float spin, float steer, float lift, float out[16]);
// A burst tyre (the slot's byte +0x1800 + 0xc x slot; 0x4ff530's other
// branch and the draw 0x4fd7f0): the rim model (VehicleData::rim) with no
// placement, turned by mirror x RotX(spin) x RotY(steer) (mirror = rows 0
// and 2 negated by 0x4195f0(-1) for slots 0, 1 and 4; steer 0 for slots
// other than 0 and 2), at hardpoint + (0, lift, 0): the draw replaces the
// matrix's translation with the hardpoint through the wheel matrix.
void VehicleRimLocal(const VehicleWheel& w, float spin, float steer, float lift, float out[16]);
