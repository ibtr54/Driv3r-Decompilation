// The damage panels (BOOT_TO_MENU.md section 72): a damaged part of the
// vehicle (bonnet, boot, doors, bumpers) leaves the body as its own rigid
// body hinged to the car, the pair joining the car's articulated group
// (articulated.h). The vehicle slot keeps one damage record per part
// (slot +0x59c + k x 0x34) and moves it through the states of 0x4f98b0;
// state 2 (hinged):
//   - 0x4f8ef0: the part's body, at the part's place on the car (the
//     inverse of its matrix in the vehicle x the part's pose x the car's
//     matrix), with the record's mass and inertia, the car's velocity at
//     that point and the car's spin;
//   - the hinge from the part's hardpoint: point and axis in the car, the
//     same in the part's frame, and reference axes (a direction per part
//     id, turned by -45 degrees about the axis on the part's side); the
//     bumpers (ids 0x23 / 0x24) hinge at the end that broke first;
//   - 0x5a1890 (Group::AddMember), then the joint's limits +-pi/4, start
//     -pi/4, spring, damper, stop gain and restitution by part id (0x4f5040).
#pragma once

#include <deque>
#include <vector>

#include "articulated.h"
#include "physics.h"
#include "vehicle.h"

// 0x66cad5 (D3DXVec3Transform as the game runs it): (x r0 + y r1) + (z r2 +
// w r3).
void Vec4Transform(const float v[4], const float m[16], float out[4]);
// The rigid inverse built in place in 0x4f8ef0 / 0x4f98b0 / 0x5099c0: the
// rotation transposed (each row keeps the source's column 3), the offset
// -(p . row) through 0x4195f0 (-1 times); the third row's sum in one of two
// orders.
void InverseRows(const float m[16], bool thirdXzy, float out[16]);

struct PanelHinge
{
    float cp[3], cax[3], cref[3];  // in the part's frame
    float pp[3], pax[3], pref[3];  // in the car's
};

// 0x4f8ef0: the body of a part with damage entry e at pose X (the part's
// matrix relative to its rest place; the identity in the traces) on a car
// with matrix car (and the car's body for its velocity and spin).
void PanelBody(const VehicleData::DamageEntry& e, const float X[16], const float car[16], const RigidBody& carBody,
               RigidBody& out);

// 0x4f98b0 state 2: the hinge of part p (its hardpoint, damage entry and
// id). damage4 / damage8: the record's +4 / +8 (a bumper hinges at the side
// with the larger); width: the slot's +0x4a0, twice the body's bounding box
// half width. False: the part has no hardpoint or no direction.
bool PanelHingeInputs(const VehicleData& v, const VehicleData::Part& p, const float X[16], float damage4,
                      float damage8, float width, PanelHinge& out);

// The joint's settings after the hinge (0x4f98b0): limits +-pi/4, the
// joint at -pi/4, spring, damper, stop gain and restitution by part id
// (0x4f5040), the impulse flag for the bumpers and part 4.
void PanelJoint(Articulated::Group& g, int member, int partType, int partId);

// The hinge and the joint's settings (0x5a1890, then PanelJoint): the panel
// joins below member parent. Returns its member index.
int HingePanel(Articulated::Group& g, int parent, RigidBody* panel, float* rec, const PanelHinge& h, int partType,
               int partId);

// 0x4f98b0 state 3 for a part with a body that left the group (the
// pointers into the slot: +0x1f80 / +0x1f90 hold the car's velocity and
// spin from the last pre-tick, 0x503e00): with the car's velocity Vc and
// spin Wc, Vh = 0.5 (prev + Vc) and Wh = 0.5 (prevW + Wc); the part's
// velocity is halved toward Vh (V = 0.5 (Vh + V)) at most 5 times while
// longer than 2 |Vh|, its spin likewise toward Wh; then 0x5a0f40 and
// 0x5a1000 set them. The game then hands the body to the debris manager
// [0x8ac450] (0x4e5730: a ring of 30, body +0x7c = 0x4d, listener +0x78 =
// the manager +0x3c).
void TornPanelMotion(RigidBody& panel, const RigidBody& car, const float prevVel[4], const float prevW[4]);
// The bonnet timer's push after its tear-off (0x4fbcc0 at 0x4fc1bc): the
// impulse J = (Vc x -0.375) x the part's mass on the part (0x5b11c0 with a
// zero angular impulse).
void TornPanelKick(RigidBody& panel, const RigidBody& car);

// The car's group while parts hang from it (BOOT_TO_MENU.md section 74):
// the slot counts its hinged parts (+0x18c0, at most 9: 0x4f7ff0); the first
// makes the group (0x4f8100 -> 0x50f1b0 -> 0x5a1820: the car's body as the
// root, Group::MakeRoot), the last one gone ends it (0x4f8030 -> 0x5aad00).
// A grouped body is stepped by the group (0x5a9880 runs 0x5c2600 per group
// and only rebuilds the members' matrices, 0x5a9bf0) and takes its
// impulses through it (0x5b11c0, 0x5c3110).
class CarGroup
{
public:
    bool Active() const { return !group.members.empty(); }
    // 0x4f98b0 state 2 for the part p of record rec (X: the part's pose,
    // half: the record's +4 / +8, width: the slot's +0x4a0): the part's body
    // (PanelBody), the group if none yet, the hinge (HingePanel). False
    // when the slot has 9 parts out or the part has no hinge; then nothing
    // changes.
    bool Hinge(const VehicleData& v, const VehicleData::Part& p, RigidBody& car, const float carMatrix[16],
               const float X[16], const float half[2], float width);
    // One physics sub-step: the group's solver, then every member's matrix.
    void Step(float h);
    // The member index of the body of record k's part, -1 if none.
    int MemberOfRecord(int record) const;
    // The body of record k's part (hinged or torn off), null if none.
    RigidBody* BodyOfRecord(int record);
    // 0x4f98b0 state 3 for the hinged part of record k (BOOT_TO_MENU.md
    // section 76): 0x505c90 (the body leaves the group, 0x5ad710; the
    // slot's count less one, 0x4f8030: the last part out ends the group,
    // 0x5aad00, and the car is free again), then the loose part's motion
    // (TornPanelMotion; prevVel / prevW: the slot's +0x1f80 / +0x1f90).
    // Returns the part's body, now a free body kept in bodies (its index
    // joins torn), or null when the record has no body here.
    RigidBody* Tear(int record, const RigidBody& car, const float prevVel[4], const float prevW[4]);
    // 0x4f98b0 state 3 for a part still on the car (no body yet): its body
    // made as for the hinge (0x4f8ef0, X: the request's matrix), free from
    // the start, no motion change. Returns it (kept in bodies, joins torn).
    RigidBody* Loose(const VehicleData& v, const VehicleData::Part& p, const RigidBody& car, const float carMatrix[16],
                     const float X[16]);

    Articulated::Group group;
    std::deque<RigidBody> bodies;           // the parts' bodies
    std::deque<std::vector<float>> recs;    // the group records (0x3c0 bytes each), [0] the car's
    std::vector<int> records;               // the damage record of each part body (bodies order)
    int hinged = 0;                         // the slot's +0x18c0: parts hanging from the car
    std::vector<int> torn;                  // bodies torn off (indices into bodies): free bodies now
};
