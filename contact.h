// Body contacts (BOOT_TO_MENU.md section 65, driv3r-re\PHYSICS.md): a body
// against the static collision world and the ground, as the physics step
// 0x5a0a50 does before it integrates:
//   - pairs (0x5a31f0): the body and every static object whose bounding box
//     overlaps the body's;
//   - narrow phase (0x5a3730): the bounding shapes first (shape 0 of each
//     side; no contact there: none at all), then every shape pair but the
//     bounds, keeping the deepest contact (0x5a3ba0: box / box 0x5b8df0 with
//     the separating axes of 0x5b9340, box / cylinder 0x5b4380 with those of
//     0x5b49b0, box / sphere 0x5b71e0; the other order with the sides
//     swapped);
//   - the ground (0x5a9b80 -> 0x5b36f0): eight points of each shape probed
//     in the GRTD heights (a box's corners, a cylinder's rim points
//     0x5b3010), the deepest one below;
//   - response (0x5a9af0 -> 0x5a99d0 -> 0x5c5260): the surfaces' values
//     (0x59b090), an impulse in the contact frame (0x5c2bc0, 0x5c3110) with
//     restitution and the friction cone (0x5c49c0), then the depth pushed
//     out, at most 0.01 a step, shared by weights (0x5c5c50; BOOT_TO_MENU.md
//     section 78).
// A body in an articulated group (the car with hinged damage panels,
// articulated.h) answers through the group (0x5c3110 -> 0x5c67c0: the
// contact's axes as columns, the point in the frame; BOOT_TO_MENU.md
// section 76). Body / body: the car against a loose panel (Pair).
// Not ported: cylinder / cylinder (0x5b9f00) and cylinder / sphere (0x5b7cf0)
// (a cylinder is taken as its bounding box there), the ground's bounding
// pre-tests (0x5b3620, 0x5b28e0), body / body pairs other than the car's with
// its loose parts, the push-out of a jointed group member (0x5ae5b0's
// chain; hinged panels have no contacts), the smashable props: the contact
// callback drops a contact with a placed object whose GEBI record breaks
// under the hit's momentum (0x4e9160 -> 0x4e8f70; BOOT_TO_MENU.md section
// 80).
// Slow contacts feed the bodies' resting records (0x5af440, physics.h): a
// body resting on two supports sleeps or is damped (0x5af1d0; BOOT_TO_MENU.md
// section 79). The response is checked against runs 21..24
// (tests\respreplay.cpp; the grouped car with its whole group from run 24):
// every impulse decision equal but one smashed prop, every call within
// 7.7e-4 (state and resting record included).
#pragma once

#include <functional>
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
    int propInstance = -1;                 // B is this smashable prop (props.h)
};

class Props;

// Box A against box B (half sizes, world matrices; 0x5b8df0 / 0x5b9340).
bool BoxBoxContact(const float sizeA[3], const float* worldA, const float sizeB[3], const float* worldB, BodyContact& c);

// Box A against cylinder B (radius, half height; its axis is row 1 of its
// matrix; 0x5b4380). The normal points from the cylinder to the box.
bool BoxCylinderContact(const float sizeA[3], const float* worldA, float radius, float halfHeight,
                        const float* worldB, BodyContact& c);

// The points a cylinder (radius r, half height h, world matrix m: axis row
// 1, centre row 3) asks the ground at (0x5b3010), and the heights its
// depths are measured from: standing exactly up (|a.y - 1| < 1e-5,
// 0x5b2a20) the top rim at 0, 90, 45 and 135 degrees and their opposites,
// the depth from 2h lower; steep (|a.y| > 0.9, 0x5b1f00) the lower rim's
// eight points; else both rims at +-P and +-Q (P = a x (0, 1, 0), Q = a x
// P, times r).
void CylinderGroundPoints(float r, float h, const float* m, float probe[8][3], float low[8]);

// The vehicle's surface rename (0x4e9160, the world's contact callback
// +0x204 before the impulse; BOOT_TO_MENU.md section 77): a side of the
// contact with surface 0xb (a vehicle's body boxes) whose body is a
// vehicle becomes 0x18 when the contact point, in the vehicle's frame
// (its matrix's rigid inverse, D3DXVec3Transform), lies at or above
// bottom + 0.025 (bottom: the vehicle's vtbl+0xc4, slot +0x1894 = the
// bounding box's lowest y), else 0x1c when the other side's +4 is -1
// (the ground's pseudo-body of the probe 0x5a9b80; the static objects
// have 0 there) and the normal (from B to A) points up: n.y (side A) or
// -n.y (side B) above 0.924; else it stays 0xb. Nothing changes when both
// sides are 0xb. side: 0 for A, 1 for B. The response then takes the
// surfaces' values again (world +0x214).
// otherStatic: the other side is the ground.
int VehicleSurface(int surface, int side, const float point[3], const float vehicle[16], float bottom,
                   const float normal[3], bool otherStatic);
// The vehicle's bottom for VehicleSurface (the slot's set-up 0x502aa0
// writes slot +0x1894 at 0x503c38): the bounding shape's centre y plus
// minus its half extent along y (vehicle 6: -0.15897, bit-exact).
float VehicleBottom(const VehicleData::Shape& bounds);

// 0x5c5260 for a contact c of body A with the free body B, or with a static
// object when b is null (the responses BodyContacts::Pair / Step make, for
// the tests): J the impulse on A.
bool PairResponse(RigidBody& a, RigidBody* b, const BodyContact& c, float J[3]);

class BodyContacts
{
public:
    // One physics sub-step's contacts of the body (shapes in its space)
    // against the world and the ground, each answered at once as the game
    // does. Returns the number of contacts answered.
    int Step(RigidBody& body, const std::vector<VehicleData::Shape>& shapes, const CollisionWorld& world,
             const CityGround* ground);

    // A pair of bodies (0x5a31f0's body pairs, BOOT_TO_MENU.md section 76):
    // A (the car, possibly in its group) against the free body B (a loose
    // panel), the shapes as for Step; one contact answered (Respond with B
    // moving), onImpulse called with the impulse on A. 1 when answered.
    int Pair(RigidBody& a, const std::vector<VehicleData::Shape>& shapesA, RigidBody& b,
             const std::vector<VehicleData::Shape>& shapesB);

    // Body A of Step / Pair is a vehicle with this bottom (VehicleSurface):
    // its surface 0xb is renamed in its object and body pairs.
    bool vehicle = false;
    float vehicleBottom = 0;

    // The smashable props (0x4e8f70): a contact with a broken prop, or one
    // the hit breaks, is dropped. Null: props are solid.
    Props* props = nullptr;

    // Diagnostics of the last step.
    int pairs = 0, contacts = 0, ground = 0;
    float deepest = 0;

    // Called after each impulse against a static object with the contact
    // and the impulse on the body: the game's contact callback reaches the
    // vehicle slot's listener (0x503fb0, damage.h) so. (Run 15 shows no
    // listener call from the ground probe's contacts.)
    std::function<void(const BodyContact&, const float J[3])> onImpulse;

private:
    std::vector<uint32_t> near_;
};
