// The player's car as a physics system (BOOT_TO_MENU.md section 94): the
// car's simulation (carsim.h), its body and contacts with the city
// (contact.h), the vehicle slot's damage records and marks (damage.h,
// marks.h), the hinged and torn-off parts in the car's group (panels.h) and
// the wheels that came off. Step is one 1/120 s tick of the world (0x4a7410)
// for the car: the slot's pre-tick, the car's tick and controls, the
// physics step's contacts (and through them the slot's contact listener),
// the free bodies' contacts, then the integration or the group's solver.
//
// The program (main.cpp) and the end-to-end crash replay
// (tests\crashreplay.cpp) both run the car through this class, so the
// replay checks the program's own pipeline. Everything that only draws (the
// parts' draw matrices, the wheels' visuals beyond the slot's block, the
// smoke particles, the camera) stays with the caller, reached through the
// hooks below.
#pragma once

#include <deque>
#include <functional>
#include <vector>

#include "carsim.h"
#include "collision.h"
#include "contact.h"
#include "damage.h"
#include "marks.h"
#include "panels.h"
#include "physics.h"
#include "props.h"
#include "smoke.h"
#include "vehicle.h"

// The wheel rays (0x5a0820) against the city's static collision (the
// placed instances' PDL shapes) and its GRTD ground (collision.h). The
// dynamic props are not in the world yet (no bodies), so the car passes
// through them.
class WorldRays : public WheelRayCaster
{
public:
    const CollisionWorld* world = nullptr;
    const CityGround* ground = nullptr;
    void Cast(WheelRay* rays, WheelHit* hits, int count) override;
};

class CarWorld
{
public:
    // The world the car lives in (set before Init; props may be null).
    const CollisionWorld* collision = nullptr;
    const CityGround* ground = nullptr;
    Props* props = nullptr;
    // printf-style log (main.cpp: its Log); null: nothing logged.
    void (*log)(const char* fmt, ...) = nullptr;

    // Hooks for the caller's draw (optional):
    // a damage record's part got a body of its own (hinged, or torn off the
    // car directly): q is the damage's request (its matrix: the part's pose).
    std::function<void(const DamageRequest& q)> onPartBody;
    // the tick's effects block ran (or not): the smoke, spray and skid
    // decision (CarSim::Effects) at its place in the car's tick.
    std::function<void(bool ran, const CarSmokeState& smoke)> onEffects;

    // The wheels that came off (0x506cd0): free bodies, their shapes about
    // their centre; drawRel is the caller's (the wheel's draw matrix
    // relative to the body, set in onWheelOff).
    struct LooseWheel
    {
        RigidBody body;
        std::vector<VehicleData::Shape> shapes;
        float drawRel[16] = {};
        int wheel = -1;  // index into the vehicle's wheels
        int ticks = 0;   // steps since it came off (the log)
    };
    std::function<void(LooseWheel& lw)> onWheelOff;

    // The car's physics set up for vehicle v (id, handling table T, the
    // creation pose pos / heading) as main.cpp InitCar did: the model's
    // wheel data, Create, the automatic gearbox, the rays, the shapes and
    // the surface rename, the slot's damage records, marks and dent, an
    // empty group, the wheel block. The table must outlive the car.
    void Init(const VehicleData& v, int id, const float* T, float wheelOffset, const float pos[3], float heading);

    // One 1/120 s tick with the driver's latest controls (SetControls'
    // c[0..4] and handbrake). Returns the number of the car body's contacts
    // with the world this tick.
    int Step(const float controls[5], bool handbrake);

    // The slot's contact listener (0x503fb0, damage.h) for one of the car
    // body's contacts (BodyContacts::onImpulse).
    void Impact(const BodyContact& c, const float J[3]);
    // The hinged parts' matrices and joint values for the damage's tests.
    void SyncPanels();
    // 0x4f98b0 for a state the damage asks for (timer: the bonnet timer's).
    void DamageState(const DamageRequest& q, bool timer = false);
    // 0x506cd0: the wheel of slot `slot` comes off as a free body.
    void SpawnLooseWheel(int slot);

    CarSim car;
    RigidBody body;
    WorldRays rays;
    BodyContacts contacts;               // the car body's
    BodyContacts debrisContacts;         // the loose parts' with the world
    std::vector<VehicleData::Shape> shapes;
    const VehicleData* vehicle = nullptr;
    VehicleDamage damage;                // the slot's damage records
    CarMarks marks;                      // the slot's damage marks
    MarkPool markPool;                   // the world's mark buffers
    CarGroup group;                      // the car and its hinged parts
    std::deque<LooseWheel> looseWheels;
    WheelVisuals wheelVis, wheelPrev;    // the slot's wheel block (slot+0x554) and the previous (+0x578)
    WheelTyres tyres;                    // slot+0x1800: burst / gone (gone from the wheel hits)
    float wheelDamageSeen[6] = {};       // the last logged wheel damage

    // log counters (as main.cpp had them)
    long contactCount = 0;
    int contactLog = 0, damageLog = 0, debrisLog = 0, wheelLog = 0, panelLog = 0, panelTicks = 0;
};
