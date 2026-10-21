// The player's car simulation (BOOT_TO_MENU.md section 62, driv3r-re\PHYSICS.md).
// In Take A Ride the player's car is an object of class 0x7850d8; its tick
// (vtbl+0xb8 = 0x4b1bb0) runs every 1/120 s and, every 0.04 s, the car
// dynamics 0x4b6a00. That routine casts one ray per wheel, works out the
// suspension, tyre and drive forces and the gravity, and hands the sum to the
// body as ONE impulse (0x5b11c0); the rigid body (physics.h) then integrates
// at 120 Hz. Force and torque stay zero: everything is an impulse.
//
// The state keeps the game's layout (the object from +0x10, "S" in the
// comments: the game passes obj + 0x10 as the first argument), so the replay
// test can load traced objects directly; the offsets are the game's.
#pragma once

#include <cstdint>
#include <cstring>

#include "physics.h"

// One wheel ray of the batch the dynamics queues (0x4b5a30) and the world
// answers (0x5a0820): the game keeps them in two global arrays.
struct WheelRay
{
    float start[4];      // ray record +0x04: the wheel's side point + 0.5 up
    float dir[4];        // +0x14: -up
    float length;        // +0x24: |up.y| 2.4 + 0.6
    uint16_t filter;     // +0x28: S+0x66c
};
struct WheelHit
{
    int type = 0;        // result +0x00: 2 = hit
    float distance = 0;  // +0x04: along dir from start
    float f8 = -1;       // +0x08
    int surface = -1;    // +0x0c: index into the surface table 0x71c2e8
    float normal[4] = {};// +0x10
    RigidBody* body = nullptr;  // +0x20: the body hit, 0 for the world
};

// The world's answer to the wheel rays (0x5a0820 in the game: the collision
// world). The program gives the city ground. The rays are not const so that
// the replay test can put the traced records in their place.
class WheelRayCaster
{
public:
    virtual ~WheelRayCaster() = default;
    virtual void Cast(WheelRay* rays, WheelHit* hits, int count) = 0;
};

// The player's driving input, run every 0.04 s (the traced controls change
// on a 5-5-5-5-4 tick pattern): the input scheme 0x514480 binds the mapper's
// controls (steer 0x1e: 16 bits, accelerate 0x22 / reverse 0x25: 8 bits,
// brake 0x23: 3 bits, handbrake 0x24, 0x27: 1 bit) and the driving
// controller 0x50ce80 builds SetControls' struct from them. The keyboard
// steering is the digital pair source (vtbl 0x788970; keys 0x3c left / 0x3d
// right): target (right - left) x 2 clamped to +-1, shaped (0.8 x + 0.2) x,
// then stepped by 0.09 (0x7a1af8) while moving out from the previous value
// on its side, else 0.18 (0x8b4c34 = 2 x 0.09); a joystick axis is not
// handled (its source 0x514c40 adds nothing without one).
struct CarDriverKeys
{
    bool accelerate = false, reverse = false, left = false, right = false;
    bool brake = false, handbrake = false, control27 = false;
};
class CarDriver
{
public:
    // One 0.04 s update. wheelSpeed: the car's S+0x54c (read through the
    // vehicle, +4 vtbl+0x18). Out: SetControls' c[0..4] and handbrake.
    void Update(const CarDriverKeys& keys, int vehicleId, float wheelSpeed, float c[5], bool& handbrake);
    float steer = 0;   // the source's +0x5c (its output +0x58)
};

class CarSim
{
public:
    static const int kObjectSize = 0xc10;
    uint8_t obj[kObjectSize] = {};      // the game's object (class 0x7850d8)
    RigidBody* body = nullptr;          // obj+0x550 (S+0x540)

    // The handling table (vehicle model +0x102c, 190 floats) and the model's
    // wheel data (slots +0x884, hardpoints +0x89c, offsets +0x92c, radius
    // +0x8fc, width +0x914, +0x98c) the dynamics reads.
    const float* table = nullptr;
    int vehicleId = 0;                  // model [+0xc]+0x14: the UPVH id (6 for the Take A Ride default)
    int slots[6] = {};                  // +0x884: nonzero for a wheel the model has
    float hardpoint[6][4] = {};         // +0x89c + 16 slot
    float wheelOffset[6][4] = {};       // +0x92c + 16 slot
    float radius[6] = {};               // +0x8fc
    float width[6] = {};                // +0x914
    float altDrop = 0;                  // +0x98c

    // World values the dynamics reads: G_8afa08 (the setup's value from
    // vtbl+0x74, set by the vehicle manager 0x50e5f0 every tick), G_8afa04.
    float setupValue = 0;
    uint8_t flag8afa04 = 0;
    uint8_t arg4 = 0;                   // the dynamics' fourth argument (0 in Take A Ride)

    // Globals the dynamics writes (kept for the replay).
    float g8afa00 = 0, g8af9f8 = 0, g8af9fc = 0, g8afa14 = 0;

    // The car's creation (0x4b1200 -> 0x4afd70 / 0x4afbe0, vtbl+0x88 =
    // 0x4b1470 -> 0x4b45a0 -> 0x4b6500): the object's state, the wheel
    // flags and rest compressions, the engine (0x5247d0) and the body (mass
    // T[0], inertia T[0xf..0x11] x T[0x1a], at rest at pos with the heading as
    // a turn about y). The model fields above must be set first.
    void Create(RigidBody* carBody, const float pos[3], float heading);

    // vtbl+0xac (0x4b1710, every 1/120 s from the vehicle slot 0x4fbaa0):
    // the driver's controls: c[0] accelerate (+1) / reverse (-1), c[1] steer
    // (right +, may pass +-1), c[2] brake, c[3] / c[4] (checked only),
    // handbrake. Sets the drive S+0x560, steer S+0x564 (x 352.71), handbrake
    // S+0x56c, throttle S+0x64c / +0x650, the brake obj+0xbd0 and wakes a
    // sleeping car.
    void SetControls(const float c[5], bool handbrake);

    // vtbl+0xf8 (0x4b0360): the automatic gearbox (S+0x65c; 1 in the traces).
    void SetAutomatic(bool on) { B(0x65c) = on ? 1 : 0; }

    // 0x4b1bb0 (vtbl+0xb8), every 1/120 s: the body's matrix to obj+0xac0,
    // then every 0.04 s (accumulator obj+0xb7c) the snapshot 0x4b0080, the
    // brake 0x4b4b30, the wheels' bits 8 / 7 from the game vehicle (vtbl+0x78
    // / +0x7c: wheelAlt / wheelOff) and the dynamics. True when they ran.
    // (The rest of the tick, sounds, skid marks and damage, is not here.)
    bool Tick(WheelRayCaster& world);
    uint8_t wheelAlt[6] = {}, wheelOff[6] = {};

    // 0x4b6a00 for the player's car (arguments 5..8 zero: rays against the
    // world). Water (0x524f00 with 0x526290) is not handled: the car is
    // always on land.
    void Dynamics(WheelRayCaster& world);

    // Object-relative accessors (the game's obj offsets, S + 0x10).
    float& OF(int o) { return *reinterpret_cast<float*>(obj + o); }
    uint32_t& OU(int o) { return *reinterpret_cast<uint32_t*>(obj + o); }
    float& F(int s) { return *reinterpret_cast<float*>(obj + 0x10 + s); }
    uint32_t& U(int s) { return *reinterpret_cast<uint32_t*>(obj + 0x10 + s); }
    uint8_t& B(int s) { return obj[0x10 + s]; }

private:
    void LiftOff();                     // 0x4ba260
    void Snapshot();                    // 0x4b0080
    void WakeBody();                    // 0x5a9230 (0x5af330)
    void SleepBody();                   // 0x5a96c0
};
