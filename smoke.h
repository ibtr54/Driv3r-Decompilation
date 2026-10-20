// The vehicle's exhaust smoke (BOOT_TO_MENU.md section 60). The world
// object keeps a pool of six exhaust emitters (0x568e30, made by the world
// init 0x55bf00 at 0x55c271; free list +0x2200, in use +0x21cc); a vehicle
// takes one per exhaust tip (0x55f670 from the vehicle's effects update at
// 0x5180f4: four for the default car) while it is slower than 5 mph and
// gives them back when it is removed (0x515860). Per update the vehicle
// (0x518109..) switches each emitter on (+0x9c) with alpha (1 - mph x 0.2)
// x 60 / 256 (+0x124), puts it at its tip (the body matrix applied to the
// tip, +0x20) and points it along -z of the body (yaw atan2(row2.x,
// -row2.z) at +0xc0, pitch asin(-row2.y) at +0xc8); the world update adds
// 0.5 to the emitter's clock (+0x98, 0x55c984), so at the game's 120
// updates a second the emitters tick 60 times a second.
// An emitter tick (0x56d1a0, the generic emitter of 0x56cca0) spawns
// int(rate) particles (rate += 1.5 + r x 0.25) into the free slots of its
// ring of 30 and writes each as one record of the particle manager (vtbl
// +0x40): six 80-byte vertices that the effect 0x2c animates on the GPU
// (position + v t + a t^2 / 2, a screen-aligned quad growing with t, the
// colour lerped from start to end, hidden outside birth..death). Every
// emitter draws its whole ring each frame (substance type 31 of sfx.pmu
// material 0x1f0000, a 64 x 64 puff), after the shadow pass.
#pragma once

#include <cstdint>
#include <vector>

// The particle vertex (declaration of the traced draws, effect 0x2c's
// vs_1_1 inputs): v0 the quad corner, v1 the velocity, v2 the acceleration,
// v3 / v4 the start / end colour, v5 the corner's growth per tick, v6 birth
// and death (ticks), v7 the uv, v8 the spawn position.
struct SmokeVertex
{
    float corner[3];     // +0  POSITION1
    float velocity[3];   // +12 TEXCOORD1
    float accel[3];      // +24 TEXCOORD2
    uint32_t start, end; // +36 COLOR0, +40 COLOR1 (D3DCOLOR)
    float growth[2];     // +44 POSITION2
    float life[2];       // +52 TEXCOORD3: birth, death
    float uv[2];         // +60 TEXCOORD0
    float position[3];   // +68 POSITION0
};
static_assert(sizeof(SmokeVertex) == 80, "the traced stride");

class ExhaustSmoke
{
public:
    static const int kSlots = 30;  // 0x568e30: count 0x1e

    // The vehicle's tips (world positions) and its body matrix (row vector):
    // one emitter per tip, switched on, alpha 60 / 256 (standing still).
    // particle: the mood's PARTICLE_COLOUR and PARTICLE_INTENSITY (+0x210).
    void Setup(const std::vector<float>& tips, const float body[16], const float particle[4]);
    // Advances by seconds of game time (60 emitter ticks a second).
    void Advance(float seconds);
    // The particles' clock for the draw (g_TimeAndConstants.x): the
    // emitters' tick plus the fraction of the next.
    float DrawTime() const { return tick_ + fraction_; }
    // Every emitter's ring as six vertices per slot, emitter after emitter.
    const std::vector<SmokeVertex>& Vertices() const { return vertices_; }
    int Emitters() const { return int(emitters_.size()); }
    int Spawned() const { return spawned_; }

private:
    struct Emitter
    {
        float tip[3] = {};
        float yaw = 0, pitch = 0;
        float rate = 0;      // +0xac
        uint32_t next = 0;   // +0x18: the ring's search start
        float death[kSlots] = {};  // +0x14
        uint32_t live = 0;   // +0xa8
    };
    float Random();
    void Tick(Emitter& e, size_t index);

    std::vector<Emitter> emitters_;
    std::vector<SmokeVertex> vertices_;
    float particle_[4] = { 1, 1, 1, 1 };
    float alpha_ = 60.0f / 256.0f;
    float tick_ = 0, fraction_ = 0;
    uint32_t seed_ = 0;  // [0x8df128], shared by every emitter
    int spawned_ = 0;
};
