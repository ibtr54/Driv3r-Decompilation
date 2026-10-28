// The car's smoke: its exhausts and its rear tyres (BOOT_TO_MENU.md
// sections 60, 82 and 83). The world object keeps pools of particle
// emitters (made by the world init 0x55bf00): six exhaust emitters (0x568e30
// at 0x55c271; free list +0x2200, in use +0x21cc) and the tyre smoke ones
// (0x570ed0; +0x2288 / +0x2254). The car takes them in its 60 Hz effects
// block (part of the car tick 0x4b1bb0, CarSim::Effects): one exhaust
// emitter per tip while it is slower than 5 mph (0x55f670 at 0x4b3701), one
// tyre emitter per rear wheel the first time that wheel smokes (0x55f8b0 at
// 0x4b2f14); it keeps them until it is removed. The block then switches
// each emitter on or off (+0x9c), sets its alpha (+0x124) and puts it at
// its place (+0x20): the exhaust at its tip through the body's matrix and
// pointed along -z of the body (yaw +0xc0, pitch +0xc8), the tyre smoke at
// the wheel's ground point. The world update adds 0.5 to every emitter's
// clock (+0x98, 0x55c984), so at the game's 120 updates a second the
// emitters tick 60 times a second.
// An emitter tick (0x56d1a0, the generic emitter of 0x56cca0) spawns
// round(rate) particles (rate += base + r x spread) into the free slots of
// its ring of 30 and writes each as one record of the particle manager
// (vtbl +0x40): six 80-byte vertices that the effect 0x2c animates on the
// GPU (position + v t + a t^2 / 2, a screen-aligned quad growing with t,
// the colour lerped from start to end, hidden outside birth..death). Every
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

// A generic emitter's settings (0x56cca0's record; the fields its makers
// set, the others 0): each value with the random spread the tick applies
// (r in [-1, 1), 38 per particle).
struct ParticleParams
{
    float spread[3];          // +0x50..+0x58: the position, x r0..r2
    float accel[3];           // +0xb4..+0xbc: added to the acceleration
    float yaw, yawR;          // +0xc0 (the emitter's, may be set later), +0xc4 x r33
    float pitch, pitchR;      // +0xc8, +0xcc x r34
    float speed0, speed0R;    // +0xd0, +0xd4 x r35: the start speed per tick
    float speed1, speed1R;    // +0xd8, +0xdc x r36: the end speed
    float size0[2], size0R[2], size0C;  // +0xe4.., +0xec.. x r17 / r18, +0xf4 x r16
    float size1[2], size1R[2], size1C;  // +0xf8.., +0x100.. x r20 / r21, +0x108 x r19
    float colour0[4];         // +0x118..+0x124: r, g, b, alpha (+0x124: the emitter's)
    float colour0R[3], colour0C, alpha0R;  // +0x128.. x r23..r25, +0x134 x r22, +0x138 x r26
    float colour1[4];         // +0x13c..+0x148
    float colour1R[3], colour1C, alpha1R;  // +0x14c.. x r28..r30, +0x158 x r27, +0x15c x r31
    float rate, rateR;        // +0x170, +0x174: particles per tick
    float life, lifeR;        // +0x178, +0x17c x r32: ticks
};
extern const ParticleParams kExhaustParticles;  // 0x568e30
extern const ParticleParams kTyreParticles;     // 0x570ed0
extern const ParticleParams kSprayParticles[3]; // 0x5706e0 (colours from 0x570c60)
// 0x406570: the game's approximate atan2(a, b) (the emitters' yaw).
float GameAtan2(float a, float b);

// What the car's effects block decided for its smoke (CarSim::Effects).
struct CarSmokeState
{
    // the exhausts: slow (|v| x 2.23704 <= 5) takes and switches them on,
    // alpha (1 - mph x 0.2) x 60 / 256; matrix: the body's (obj+0xac0)
    bool exhaustSlow = false;
    float exhaustFactor = 0;
    float matrix[16] = {};
    // the rear tyres (wheels 1 and 3): smoking, alpha, ground point
    bool tyreOn[2] = {};
    float tyreAlpha[2] = {};
    float tyrePos[2][3] = {};
    // the rear wheels' skid marks (0x4b3481, skid.h): skidding (smoking with
    // slip >= 0.5, flags without 4), the ribbon's intensity clamp(k x slip,
    // 0, 1) (k 0.9 when obj+0x578 > 0, else 0.75) and the tyre's width
    // (obj+0xc84 + 4 w, taken with the ribbon)
    // the rear wheels' spray on loose ground (0x4b2f98..0x4b3340, the record
    // of 0x5706e0 at obj+0xca4 + 16 w): emitter 0 (puffs), 1 (thrown back),
    // 2 (debris) on; sand (surface 2) or dirt colours (0x570c60); the rear
    // wheels' speed S+0x548 / 32 for 0x570b90; placed at tyrePos
    bool sprayOn[2][3] = {};
    bool spraySand[2] = {};
    float spraySpeed = 0;
    bool skidRan = false;  // the block ran (not under water)
    bool skidOn[2] = {};
    float skidIntensity[2] = {};
    float skidWidth[2] = {};
};

class ExhaustSmoke
{
public:
    static const int kSlots = 30;  // count 0x1e of both makers

    // The vehicle's tips (x, y, z each, in the vehicle's space) and its body
    // matrix (row vector): one exhaust emitter per tip and two tyre
    // emitters, not yet taken. particle: the mood's PARTICLE_COLOUR and
    // PARTICLE_INTENSITY (+0x210).
    void Setup(const std::vector<float>& tips, const float body[16], const float particle[4]);
    // The car's effects block's decision (60 Hz): emitters taken, switched
    // on or off, alpha and place.
    void Apply(const CarSmokeState& s);
    // Advances by seconds of game time (60 emitter ticks a second; the world
    // update adds 0.5 to the emitters' clock, 0x55c984).
    void Advance(float seconds);
    // The particles' clock for the draw (g_TimeAndConstants.x): the
    // emitters' tick plus the fraction of the next.
    float DrawTime() const { return tick_ + fraction_; }
    // Every emitter's ring as six vertices per slot, emitter after emitter.
    const std::vector<SmokeVertex>& Vertices() const { return vertices_; }
    int Emitters() const { return int(emitters_.size()); }
    int Spawned() const { return spawned_; }
    // Emitter i's ring: its first slot in Vertices() (six vertices a slot),
    // its slot count and its material (0: sfx 0x1f0000, 1: 0x1f0003, the
    // spray's debris).
    int First(int i) const { return int(emitters_[size_t(i)].first); }
    int Slots(int i) const { return int(emitters_[size_t(i)].death.size()); }
    int Material(int i) const { return emitters_[size_t(i)].material; }

private:
    struct Emitter
    {
        ParticleParams p = {};  // the maker's values (the spray's change at run time)
        int material = 0;
        size_t first = 0;     // the ring's first slot in vertices_
        bool taken = false;   // from the world's pool
        float local[3] = {};  // an exhaust's tip in the vehicle's space
        float tip[3] = {};    // +0x20: in the world
        float yaw = 0, pitch = 0;  // +0xc0, +0xc8
        bool on = false;      // +0x9c
        float alpha = 0;      // +0x124
        float rate = 0;       // +0xac
        uint32_t next = 0;    // +0x18: the ring's search start
        std::vector<float> death;  // +0x14: one per slot (the maker's count)
        uint32_t live = 0;    // +0xa8
    };
    float Random();
    void Tick(Emitter& e);
    void Add(const ParticleParams& p, int slots, int material);
    void SprayColours(size_t first, bool sand);  // 0x570c60

    std::vector<Emitter> emitters_;
    size_t exhausts_ = 0;     // emitters [0, exhausts_) are the exhausts, then the two tyres
    size_t spray_ = 0;        // then the two spray records' three emitters each (0x5706e0)
    bool sprayTaken_[2] = {};
    int spraySand_[2] = {};   // the record's +0xc (1 at the maker, then 0x570c60)
    std::vector<SmokeVertex> vertices_;
    float particle_[4] = { 1, 1, 1, 1 };
    float tick_ = 0, fraction_ = 0;
    uint32_t seed_ = 0;  // [0x8df128], shared by every emitter
    int spawned_ = 0;
};
