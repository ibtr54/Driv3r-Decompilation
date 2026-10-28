#include "smoke.h"

#include <cmath>
#include <cstring>

// 0x568e30: the exhaust emitter (spread 0.01, a slow puff up the exhaust's
// direction, grey 0.9375 -> 0.234, 20 + 10 r ticks).
const ParticleParams kExhaustParticles = {
    { 0.01f, 0.01f, 0.01f },       // +0x50..
    { 0.0f, 0.0005f, 0.0f },       // +0xb4.. (+0xb8: the rise)
    0.0f, 0.17453292f,             // yaw (set by the car), +0xc4
    0.0f, 0.17453292f,             // pitch (set by the car), +0xcc
    0.04f, 0.0f, 0.01f, 0.0f,      // +0xd0.., +0xd8..
    { 0.05f, 0.05f }, { 0.01f, 0.01f }, 0.1f,   // +0xe4.., +0xec.., +0xf4
    { 0.3f, 0.3f }, { 0.02f, 0.02f }, 0.05f,    // +0xf8.., +0x100.., +0x108
    { 0.9375f, 0.9375f, 0.9375f, 0.0f }, { 0, 0, 0 }, 0.0390625f, 0.0f,        // +0x118.., +0x134
    { 0.234375f, 0.234375f, 0.234375f, 0.0f }, { 0, 0, 0 }, 0.0390625f, 0.0f,  // +0x13c.., +0x158
    1.5f, 0.25f,                   // +0x170, +0x174
    20.0f, 10.0f,                  // +0x178, +0x17c
};

// 0x570ed0: the tyre smoke (BOOT_TO_MENU.md section 83): spread 0.2 on the
// ground, rising slowly in every direction (pitch pi / 2 +- pi / 6, any
// yaw), large puffs (0.8 -> 2.5), grey 0.9375 -> (0.703, 0.703, 0.898),
// 60 + 10 r ticks.
const ParticleParams kTyreParticles = {
    { 0.2f, 0.01f, 0.2f },
    { 0.0f, 0.0004f, 0.0f },
    0.0f, 6.28318548f,
    1.57079637f, 0.52359879f,
    0.01f, 0.01f, 0.01f, 0.01f,
    { 0.8f, 0.8f }, { 0.08f, 0.08f }, 0.4f,
    { 2.5f, 2.5f }, { 0.1f, 0.1f }, 0.5f,
    { 0.9375f, 0.9375f, 0.9375f, 0.0f }, { 0, 0, 0 }, 0.0390625f, 0.0f,
    { 0.703125f, 0.703125f, 0.8984375f, 0.0f }, { 0, 0, 0 }, 0.0390625f, 0.0f,
    1.0f, 0.5f,
    60.0f, 10.0f,
};

namespace {

uint32_t Pack(float a, float r, float g, float b)
{
    auto c = [](float v) {
        int i = int(v * 255.0f);
        return uint32_t(i < 0 ? 0 : i > 255 ? 255 : i);
    };
    return c(a) << 24 | c(r) << 16 | c(g) << 8 | c(b);
}

// The x87's fistp under the default rounding (to nearest, ties to even):
// Ghidra's ROUND in 0x56d1a0.
int Round(float v) { return int(std::nearbyint(v)); }

}  // namespace

// [0x8df128] = [0x8df128] x 0x19660d + 0x3c6ef35f; the mantissa as a float
// in [1, 2), minus 1.5, doubled: [-1, 1).
float ExhaustSmoke::Random()
{
    seed_ = seed_ * 0x19660du + 0x3c6ef35fu;
    uint32_t bits = (seed_ & 0x7fffff) | 0x3f800000u;
    float f;
    memcpy(&f, &bits, 4);
    return (f - 1.5f) * 2.0f;
}

void ExhaustSmoke::Setup(const std::vector<float>& tips, const float body[16], const float particle[4])
{
    emitters_.clear();
    vertices_.clear();
    exhausts_ = 0;
    tick_ = fraction_ = 0;
    spawned_ = 0;
    if (tips.empty() || !body) return;
    memcpy(particle_, particle, sizeof(particle_));
    for (size_t i = 0; i + 2 < tips.size(); i += 3) {
        Emitter e;
        e.p = &kExhaustParticles;
        for (int k = 0; k < 3; ++k) e.local[k] = tips[i + k];
        emitters_.push_back(e);
    }
    exhausts_ = emitters_.size();
    for (int k = 0; k < 2; ++k) {
        Emitter e;
        e.p = &kTyreParticles;
        e.yaw = kTyreParticles.yaw;
        e.pitch = kTyreParticles.pitch;
        emitters_.push_back(e);
    }
    // At rest at the body (what the car's first effects block decides for a
    // car standing still; the view without the car's physics keeps it).
    CarSmokeState rest;
    rest.exhaustSlow = true;
    rest.exhaustFactor = 1.0f;
    memcpy(rest.matrix, body, sizeof(rest.matrix));
    Apply(rest);
    // Every slot's six vertices; uv and corner signs of the traced records.
    vertices_.assign(emitters_.size() * kSlots * 6, SmokeVertex());
}

void ExhaustSmoke::Apply(const CarSmokeState& s)
{
    // the exhausts (0x4b3701..): taken while slow; off, then on with alpha
    // when slow (the engine's condition, car vtbl+0x1c above handling
    // +0x10cc or obj+0x66c, taken as true); always at the tip through the
    // body's matrix, pointed along its row 2 (0x406570 = atan2, 0x6cda60 =
    // asin)
    const float* m = s.matrix;
    const float yaw = std::atan2(m[8], -m[10]);
    const float pitch = std::asin(-m[9]);
    for (size_t i = 0; i < exhausts_; ++i) {
        Emitter& e = emitters_[i];
        if (s.exhaustSlow) e.taken = true;
        if (!e.taken) continue;
        e.on = s.exhaustSlow;
        if (e.on) e.alpha = s.exhaustFactor * 60.0f * 0.00390625f;  // x the emitter's 60 x [0x78e874]
        for (int k = 0; k < 3; ++k)
            e.tip[k] = e.local[0] * m[k] + e.local[1] * m[4 + k] + e.local[2] * m[8 + k] + m[12 + k];
        e.yaw = yaw;
        e.pitch = pitch;
    }
    // the rear tyres (0x4b2f14..): taken the first time they smoke
    for (int k = 0; k < 2 && exhausts_ + k < emitters_.size(); ++k) {
        Emitter& e = emitters_[exhausts_ + k];
        if (s.tyreOn[k]) e.taken = true;
        if (!e.taken) continue;
        e.on = s.tyreOn[k];
        if (e.on) e.alpha = s.tyreAlpha[k];
        for (int j = 0; j < 3; ++j) e.tip[j] = s.tyrePos[k][j];
    }
}

void ExhaustSmoke::Advance(float seconds)
{
    if (emitters_.empty()) return;
    fraction_ += seconds * 60.0f;
    if (fraction_ > 120.0f) fraction_ = 120.0f;  // a long stall: two seconds at most
    while (fraction_ >= 1.0f) {
        for (size_t i = 0; i < emitters_.size(); ++i)
            if (emitters_[i].taken) Tick(emitters_[i], i);
        tick_ += 1.0f;
        fraction_ -= 1.0f;
    }
}

// One pass of 0x56d1a0's loop for the clock tick_ (+0x94).
void ExhaustSmoke::Tick(Emitter& e, size_t index)
{
    const ParticleParams& p = *e.p;
    // The slots that die now are freed.
    if (e.live)
        for (float& d : e.death)
            if (d == tick_) {
                d = 0;
                --e.live;
            }
    // On (+0x9c): the rate, then round(rate) particles of 38 random numbers
    // each (taken even when the ring is full); the rate keeps what the
    // rounding left (it can go below 0).
    if (!e.on) return;
    e.rate = (Random() * p.rateR + p.rate) + e.rate;
    if (!(1.0f <= e.rate)) return;
    const int n = Round(e.rate);
    const float* m = particle_;  // the mood's colour (+0x160..) and intensity (+0x16c)
    for (int q = 0; q < n; ++q) {
        float r[38];
        for (float& v : r) v = Random();
        if (e.live >= uint32_t(kSlots)) continue;
        uint32_t slot;
        do {
            slot = e.next;
            if (++e.next >= uint32_t(kSlots)) e.next = 0;
        } while (tick_ < e.death[slot]);
        // Mode 0 (+0x1c): the point, spread by r0..r2.
        float pos[3];
        for (int k = 0; k < 3; ++k) pos[k] = r[k] * p.spread[k] + e.tip[k];
        const float c0 = r[16] * p.size0C, c1 = r[19] * p.size1C;
        const float sx0 = r[17] * p.size0R[0] + p.size0[0] + c0, sy0 = r[18] * p.size0R[1] + p.size0[1] + c0;
        const float sx1 = r[20] * p.size1R[0] + p.size1[0] + c1, sy1 = r[21] * p.size1R[1] + p.size1[1] + c1;
        // the colours (+0x118.. / +0x13c..) x intensity x the mood's colour,
        // each channel in the game's order
        const float k0 = r[22] * p.colour0C, k1 = r[27] * p.colour1C;
        const float r0 = (r[23] * p.colour0R[0] + p.colour0[0] + k0) * m[3] * m[0];
        const float g0 = (r[24] * p.colour0R[1] + p.colour0[1] + k0) * m[1] * m[3];
        const float b0 = (r[25] * p.colour0R[2] + p.colour0[2] + k0) * m[2] * m[3];
        const float a0 = r[26] * p.alpha0R + e.alpha;
        const float r1 = (r[28] * p.colour1R[0] + p.colour1[0] + k1) * m[3] * m[0];
        const float g1 = (r[29] * p.colour1R[1] + p.colour1[1] + k1) * m[1] * m[3];
        const float b1 = (r[30] * p.colour1R[2] + p.colour1[2] + k1) * m[2] * m[3];
        const float a1 = r[31] * p.alpha1R + p.colour1[3];
        const float life = float(Round(r[32] * p.lifeR)) + p.life;
        const float yaw = r[33] * p.yawR + e.yaw, pitch = r[34] * p.pitchR + e.pitch;
        const float dir[3] = { -(std::sin(yaw) * std::cos(pitch)), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
        const float s0 = r[35] * p.speed0R + p.speed0, s1 = r[36] * p.speed1R + p.speed1;
        const float inv = 1.0f / life;
        SmokeVertex v = {};
        for (int k = 0; k < 3; ++k) {
            v.velocity[k] = s0 * dir[k];
            v.accel[k] = (s1 * dir[k] - s0 * dir[k]) * inv + p.accel[k];
            v.position[k] = pos[k];
        }
        v.start = Pack(a0, r0, g0, b0);
        v.end = Pack(a1, r1, g1, b1);
        v.life[0] = tick_;
        v.life[1] = tick_ + life;
        e.death[slot] = tick_ + life;
        ++e.live;
        ++spawned_;
        // The manager's six vertices (as traced): half the size as the
        // corner, the half growth per tick, corners (-, +), (-, -), (+, +),
        // (+, +), (-, -), (+, -) with uv (0, 0), (0, 1), (1, 0), (1, 0),
        // (0, 1), (1, 1).
        static const int kCorner[6][2] = { { -1, 1 }, { -1, -1 }, { 1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 } };
        SmokeVertex* out = &vertices_[(index * kSlots + slot) * 6];
        for (int q2 = 0; q2 < 6; ++q2) {
            out[q2] = v;
            const float sx = float(kCorner[q2][0]), sy = float(kCorner[q2][1]);
            out[q2].corner[0] = sx * sx0 * 0.5f;
            out[q2].corner[1] = sy * sy0 * 0.5f;
            out[q2].growth[0] = sx * (sx1 - sx0) * 0.5f / life;
            out[q2].growth[1] = sy * (sy1 - sy0) * 0.5f / life;
            out[q2].uv[0] = sx > 0 ? 1.0f : 0.0f;
            out[q2].uv[1] = sy > 0 ? 0.0f : 1.0f;
        }
    }
    e.rate = e.rate - float(Round(e.rate));
}
