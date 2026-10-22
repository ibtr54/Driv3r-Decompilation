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
