// The vehicle's damage (BOOT_TO_MENU.md section 73): what a crash does to
// the damage records of the vehicle slot (class 0x7886d8), and when a part
// leaves the body. The slot's contact listener (slot +0xc, vtable 0x78865c)
// gets every contact of the car's body with its impulse (method 1, 0x503fb0):
//   - 0x5054b0: the impulse over the car's mass, its length s and direction;
//     only s >= 5.75 goes on; the contact point in the world (the point in
//     body A's frame turned by the contact's rows +0x30, plus A's position);
//   - (head-on hits on surfaces 0xb / 0x18 set two timers +0x189c / +0x18a0
//     and strong ones call 0x5063f0; neither touches the records);
//   - a hinged part's body hit (not the car's): that record alone takes the
//     hit, at distance 1 of reach 1 (0x504905);
//   - the point and the direction in the car's frame (0x5056a0: the car's
//     matrix and its rigid inverse), the side weight 0x509970 of the point's
//     x across the width, s raised by up to |z weight x direction x|;
//   - every UPVH part with a damage record (walking the part tree, records
//     past the slot's count end it) still on the car or hinged: the part
//     test 0x5099c0;
//   - 0x4fd480: the records of the front and the back groups (0x4f4790 /
//     0x4f4840) all take the group's largest +4 / +8.
// The part test 0x5099c0: the point's offset from the part (its physics
// shape's matrix in the part's frame, or the hinged body's matrix in the
// car's) must lie within 0.75 x width in x, 0.675 x length in z and the
// shape's reach (|size| x [+0x228] x 1.5) shrunk along the hit's direction;
// then a cool-down of 0.25 s (+0x1c), the damage 0x50a130 added to the two
// halves +4 / +8 (split by the side weight), the dent +0x2c, the impact
// +0x20 and the slot's total +0x1f78; the visual stage 0x4f5210; then the
// part is torn off (0x4f98b0 state 3) when the hit (the larger of the
// record's impact and this hit's raw strength) reaches 0.09375 x (1 - tear
// scale x [+0x94]) and the dent passes 1.5 x (1 - ...), else hinged (state
// 2) when the hit reaches 0.09375 (twice that for ids 1 and 0x1f..0x39)
// and the part may hinge (0x4f4620).
// The dent (BOOT_TO_MENU.md section 76): at the slot's set-up 0x5091b0
// picks the body's largest physics box (the largest |half size|^2 from
// shape 1 on, at most 6 shapes, none for the vehicles of 0x513ec0) and its
// longest axis (slot +0x1f6a / +0x1f6b) with the scales [0x26] / [0x27] of
// the handling table (+0x1f6c / +0x1f70); every 0x4fd480(1) then shortens
// that axis by the front and back groups' damage: half = (2 rest - (back +
// front)) / 2, the centre moved by (back - front) / 2 along the game's z
// (front = (+0x544 + +0x540) / 2 x scale, back likewise): the car's own
// box gives in where it was hit.
// The wheel hits 0x507980 (section 87): s before the raise above, on the
// nearest wheel; a wheel comes off at 1.5 (Wheel, WheelHit).
// Not ported: the loose wheel's body (0x506cd0), the explosion contacts
// (contact +0xf8 bit 0), the effects and sounds (0x5063f0, 0x50b100, sparks
// 0x567840.., the scrape 0x56ff10), the visual dents 0x4f5890 (vertex
// deformation).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "physics.h"
#include "vehicle.h"

// One damage record (slot +0x24 + k x 0x34, vtable 0x7885fc; the k-th part
// of the UPVH tree has +0x1a == k).
struct DamageRecord
{
    float half[2] = {};       // +0x04 / +0x08: damage of the part's two halves, 0..1
    int state = 0;            // +0x0c (u16): 0 on the car, 1 body, 2 hinged, 3 torn off (0x4f98b0)
    int word0e = 0;           // +0x0e (u16)
    uint32_t flags = 0;       // +0x10: bit 1 glass (0x4f5210), bit 2 set by the hinge
    uint32_t body = 0;        // +0x14: the part's body (non-zero from state 1 on)
    float f18 = 0;            // +0x18: 20 once hinged
    float cooldown = 0;       // +0x1c: 0.25 after a hit, less 1/120 a tick (0x4fbcc0)
    float impact = 0;         // +0x20: the hits' raw strength x 0.25, summed
    float f24 = 0, f28 = 0;   // +0x24, +0x28: (+4 + +8) / 2 once hinged
    float dent = 0;           // +0x2c: the hits' damage x 0.25, summed
    uint8_t stage = 0, worst = 0;  // +0x30 / +0x31: 0x4f5210's visual stage
    uint8_t b32 = 0, b33 = 0;
    // Kept up to date by the caller for a part with a body: its matrix
    // (body +0x10) and its joint value (the group list's +0x20 entry).
    float bodyMatrix[16] = {};
    float jointQ = 0;
};

// A call of the part state machine 0x4f98b0 the damage asks for.
struct DamageRequest
{
    int record = -1;
    int state = 0;        // 2 hinge, 3 tear off
    float matrix[16] = {};  // its matrix argument (the hinged body's in the car's frame, else the identity)
};

class VehicleDamage
{
public:
    // The slot's records for vehicle v (0x4f67e0 makes them zeroed: one per
    // part with +0x1a set) and the values the tests read: size = the slot's
    // +0x4a0 / +0x4a4 / +0x4a8 (the body's bounding box), handling = the
    // parameter block at slot +0x490 (the vehicle's handling table: [0x8a]
    // reach, [0x8b] strength, [0x25] crush).
    void Init(const VehicleData& v, const float size[3], const float handling[kHandlingKeys]);

    // 0x503fb0 for a contact of our body A with the impulse J on A: A is the
    // car's body (hitRecord -1) or the body of the hinged part of record
    // hitRecord; car = the car's matrix (0x5056a0), carMass its body's mass,
    // massA A's, point the contact point in the world, surface the
    // contact's +0xd0. Appends the state changes to make.
    void Impact(const VehicleData& v, const float car[16], float carMass, int hitRecord, float massA,
                const float J[3], const float point[3], int surface, std::vector<DamageRequest>& out);

    // The slot pre-tick's share (0x4fbcc0): the records' cool-downs, and the
    // bonnet (a hinged part of id 4) torn off after its joint stayed beyond
    // 0.687 for 4 s (one timer for the slot, +0x1898). car: the car's matrix.
    void Tick(const VehicleData& v, const float car[16], std::vector<DamageRequest>& out);

    // 0x5091b0's dent choice for vehicle id with the car's physics shapes
    // (in the program's space; the half sizes are the PDL's): the shape and
    // axis, the scales from handling [0x26] / [0x27], the rest shapes. The
    // dents then go to *shapes (the program's live shapes).
    void InitDent(int vehicleId, std::vector<VehicleData::Shape>* shapes, const float handling[kHandlingKeys]);
    // 0x4f9810 (state 3): the slot's total takes the record's share,
    // clamp01(1 - impact) / count, when the damage switch allows it.
    void TearShare(int record);

    std::vector<DamageRecord> records;
    std::vector<int> recordPart;  // slot +0x480: the part of each record
    // The wheels (slot +0x17f8 + 0xc x slot, BOOT_TO_MENU.md section 87):
    // the hits 0x507980 add up, a cool-down of 0.25 s between them (less
    // 1/120 a pre-tick, 0x4fbcc0); at 1.5 the wheel comes off (0x506cd0
    // sets gone). burst is set only by the spawn description (0x4fb3e0).
    struct Wheel
    {
        float damage = 0;    // +0x17f8
        float cooldown = 0;  // +0x17fc
        bool burst = false;  // +0x1800
        bool gone = false;   // +0x1801
    };
    Wheel wheels[6];
    // 0x5115c0: each wheel slot's place in the car (offset +0x92c +
    // hardpoint +0x89c, w 1), in part order (the slot's list +0x470); the
    // scale [+0x490] +0x2f4 (handling [0xbd]).
    std::vector<std::pair<int, std::array<float, 4>>> wheelPlaces;
    float wheelScale = 0;
    int wheelOff = -1;  // the wheel the last Impact knocked off (-1: none)
    void WheelHit(const float L[4], float s);  // 0x507980
    float size[3] = {};           // +0x4a0..+0x4a8
    float reach = 1, strength = 1, crushScale = 0.25f;  // [+0x490] +0x228 / +0x22c / +0x94
    float hitScale = 1;           // +0x1888 (1 in the traces)
    float tearScale = 1;          // +0x1890 (1 in the traces)
    int linked = -1;              // +0x18a4: a record that takes the hits of parts 2 / 0x21 / 0x22 / 0x25
    float total = 0;              // +0x1f78
    float openTime = 0;           // +0x1898: how long the bonnet has been open
    float tearQ = 0.687223434f, tearTime = 4.0f;  // the timer's limits ([0x78f1c4], [0x78e8a4])
    bool glass = false;           // +0x1f64 (byte)
    float front[2] = {}, back[2] = {};  // +0x540 / +0x544, +0x548 / +0x54c (0x4fd480)
    int dentShape = -1, dentAxis = 0;   // +0x1f6a / +0x1f6b
    float dentScale[2] = {};            // +0x1f6c / +0x1f70 (front, back)
    std::vector<VehicleData::Shape> dentRest;            // the shapes as loaded (the model's, slot +0x444)
    std::vector<VehicleData::Shape>* dentShapes = nullptr;  // the live ones (slot +0x18d0)
    float prevVel[4] = {}, prevW[4] = {};  // +0x1f80 / +0x1f90: the car's at the last pre-tick (0x503e00)

    // Intermediates of the last Impact, for the tests: s after the raise,
    // the point and the direction in the car's frame, the side weight.
    float lastS = 0, lastPoint[4] = {}, lastDir[4] = {}, lastSide = 0;
    bool lastPassed = false;

    // 0x5099c0 (public for the tests): the part p (record k, damage entry e)
    // against the hit; M the hinged body's matrix in the car's frame (or the
    // identity). Returns the state to ask for (0: none).
    int PartTest(const VehicleData& v, const VehicleData::Part& p, int k, const VehicleData::DamageEntry& e, float s,
                 const float dir[4], float side, const float M[16], const float point[4], bool own);

private:
    float Apply(DamageRecord& target, DamageRecord& hit, float s, float dist, float reachR, float side, float f,
                bool own, bool addTotal);
    void Stage(DamageRecord& r, int vehicleId);
    void Share(const VehicleData& v);
};

// The part predicates (type, id, the part's +0x08 flags).
bool DamageFrontGroup(int type, int id, uint32_t flags);  // 0x4f4790
bool DamageBackGroup(int type, int id, uint32_t flags);   // 0x4f4840
bool DamageMayTear(int type, int id, uint32_t flags);     // 0x4f4570
bool DamageMayHinge(int type, int id, uint32_t flags);    // 0x4f4620
