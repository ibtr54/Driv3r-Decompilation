// Body contacts (BOOT_TO_MENU.md section 65, driv3r-re\PHYSICS.md): a body
// against the static collision world and the ground, as the physics step
// 0x5a0a50 does before it integrates:
//   - pairs (0x5a31f0): the body and every static object whose bounding box
//     overlaps the body's;
//   - narrow phase (0x5a3730): the bounding shapes first (shape 0 of each
//     side; no contact there: none at all), then every shape pair but the
//     bounds, keeping the deepest contact (0x5a3ba0: box / box 0x5b8df0 with
//     the separating axes of 0x5b9340, box / sphere 0x5b71e0);
//   - the ground (0x5a9b80 -> 0x5b36f0): the box's eight corners probed in
//     the GRTD heights, the deepest one below;
//   - response (0x5a9af0 -> 0x5a99d0 -> 0x5c5260): the surfaces' values
//     (0x59b090), an impulse in the contact frame (0x5c2bc0, 0x5c3110) with
//     restitution and the friction cone (0x5c49c0), then the depth pushed
//     out, at most 0.01 a step (0x5c5c50).
// Not ported: box / cylinder (0x5b4380 and its helpers; a cylinder is taken
// as its bounding box here), body / body contacts, and the constraint
// groups (body +0x1d8) the game makes of slow, resting contacts and solves
// in 0x5c2600 (here those get the same impulse as fast ones).
#pragma once

#include <vector>

#include "city.h"
#include "collision.h"
#include "physics.h"
#include "vehicle.h"

struct BodyContact
{
    float pointA[3] = {}, pointB[3] = {};  // world (0x5a3730 keeps them relative to the objects)
    float normal[3] = {};                  // from B to A
    float depth = 0;                       // +0x30
    int surfaceA = 0, surfaceB = 0;        // +0x40 / +0x44
};

class BodyContacts
{
public:
    // One physics sub-step's contacts of the body (shapes in its space)
    // against the world and the ground, each answered at once as the game
    // does. Returns the number of contacts answered.
    int Step(RigidBody& body, const std::vector<VehicleData::Shape>& shapes, const CollisionWorld& world,
             const CityGround* ground);

    // Diagnostics of the last step.
    int pairs = 0, contacts = 0, ground = 0;
    float deepest = 0;

private:
    std::vector<uint32_t> near_;
};
