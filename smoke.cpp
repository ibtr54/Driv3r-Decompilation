#include "smoke.h"

#include <cmath>
#include <cstring>

namespace {

// The exhaust emitter's parameters (0x568e30 on the generic emitter of
// 0x56cca0; the fields it leaves are 0 from 0x56cca0).
const float kSpread = 0.01f;                     // +0x50 / +0x54 / +0x58: position, x r
const float kSize0 = 0.05f, kSize0R = 0.01f;     // +0xe4 / +0xe8, +0xec / +0xf0
const float kSize1 = 0.3f, kSize1R = 0.02f;      // +0xf8 / +0xfc, +0x100 / +0x104
const float kSize0C = 0.1f, kSize1C = 0.05f;     // +0xf4, +0x108: one r for x and y
const float kGrey0 = 0.9375f, kGrey1 = 0.234375f;  // +0x118.., +0x13c..
const float kGreyC = 0.0390625f;                 // +0x134, +0x158: one r for r, g, b
const float kAngle = 0.17453292f;                // +0xc4 / +0xcc: yaw / pitch, x r
const float kSpeed0 = 0.04f, kSpeed1 = 0.01f;    // +0xd0, +0xd8: per tick
const float kRise = 0.0005f;                     // +0xb8: added to the acceleration's y
const float kRate = 1.5f, kRateR = 0.25f;        // +0x170, +0x174: particles per tick
const float kLife = 20.0f, kLifeR = 10.0f;       // +0x178, +0x17c: ticks

uint32_t Pack(float a, float r, float g, float b)
{
    auto c = [](float v) {
        int i = int(v * 255.0f);
        return uint32_t(i < 0 ? 0 : i > 255 ? 255 : i);
    };
    return c(a) << 24 | c(r) << 16 | c(g) << 8 | c(b);
}

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
    tick_ = fraction_ = 0;
    spawned_ = 0;
    if (tips.empty() || !body) return;
    memcpy(particle_, particle, sizeof(particle_));
    for (size_t i = 0; i + 2 < tips.size(); i += 3) {
        Emitter e;
        for (int k = 0; k < 3; ++k) e.local[k] = tips[i + k];
        emitters_.push_back(e);
    }
    const float still[3] = { 0, 0, 0 };
    Update(body, still);
    // Every slot's six vertices; uv and corner signs of the traced records.
    vertices_.assign(emitters_.size() * kSlots * 6, SmokeVertex());
}

void ExhaustSmoke::Update(const float body[16], const float vel[3])
{
    if (!body) return;
    // 0x518006..: the squared speed x [0x78ee98] against [0x78e954] (5);
    // slow: the factor 1 - |v| (0x442ba0) x [0x78ee98] x [0x78ea54] (0.2)
    const float sq = ((vel[2] * vel[2] + vel[1] * vel[1]) + vel[0] * vel[0]) * 2.23704004f;
    const bool slow = !(sq > 5.0f);
    const float speed = std::sqrt((vel[0] * vel[0] + vel[1] * vel[1]) + vel[2] * vel[2]);
    const float factor = slow ? 1.0f - speed * 2.23704004f * 0.200000003f : 0.0f;
    // the angles from the body's row 2 (0x406570 = atan2, 0x6cda60 = asin)
    const float yaw = std::atan2(body[8], -body[10]);
    const float pitch = std::asin(-body[9]);
    for (Emitter& e : emitters_) {
        // 0x518a10 (the engine's state) and the tip's part not destroyed
        // are taken as true here
        e.on = slow;
        if (slow) e.alpha = factor * 60.0f * 0.00390625f;  // x the emitter's 60 x [0x78e874]
        for (int k = 0; k < 3; ++k)
            e.tip[k] = e.local[0] * body[k] + e.local[1] * body[4 + k] + e.local[2] * body[8 + k] + body[12 + k];
        e.yaw = yaw;
        e.pitch = pitch;
    }
}

void ExhaustSmoke::Advance(float seconds)
{
    if (emitters_.empty()) return;
    fraction_ += seconds * 60.0f;
    if (fraction_ > 120.0f) fraction_ = 120.0f;  // a long stall: two seconds at most
    while (fraction_ >= 1.0f) {
        for (size_t i = 0; i < emitters_.size(); ++i) Tick(emitters_[i], i);
        tick_ += 1.0f;
        fraction_ -= 1.0f;
    }
}

// One pass of 0x56d1a0's loop for the clock tick_ (+0x94).
void ExhaustSmoke::Tick(Emitter& e, size_t index)
{
    // The slots that die now are freed.
    if (e.live)
        for (float& d : e.death)
            if (d == tick_) {
                d = 0;
                --e.live;
            }
    // On (+0x9c): the rate, then int(rate) particles of 38 random numbers
    // each (taken even when the ring is full).
    if (!e.on) return;
    e.rate += Random() * kRateR + kRate;
    if (e.rate < 1.0f) return;
    int n = int(e.rate);
    for (int p = 0; p < n; ++p) {
        float r[38];
        for (float& v : r) v = Random();
        if (e.live >= uint32_t(kSlots)) continue;
        uint32_t slot;
        do {
            slot = e.next;
            if (++e.next >= uint32_t(kSlots)) e.next = 0;
        } while (tick_ < e.death[slot]);
        // Mode 0 (+0x1c): the point, spread by r0..r2.
        float pos[3] = { e.tip[0] + r[0] * kSpread, e.tip[1] + r[1] * kSpread, e.tip[2] + r[2] * kSpread };
        float sx0 = r[17] * kSize0R + kSize0 + r[16] * kSize0C, sy0 = r[18] * kSize0R + kSize0 + r[16] * kSize0C;
        float sx1 = r[20] * kSize1R + kSize1 + r[19] * kSize1C, sy1 = r[21] * kSize1R + kSize1 + r[19] * kSize1C;
        const float* m = particle_;  // colour x intensity (+0x160.. x +0x16c)
        float g0 = kGrey0 + r[22] * kGreyC, g1 = kGrey1 + r[27] * kGreyC;
        uint32_t c0 = Pack(e.alpha, g0 * m[0] * m[3], g0 * m[1] * m[3], g0 * m[2] * m[3]);
        uint32_t c1 = Pack(0.0f, g1 * m[0] * m[3], g1 * m[1] * m[3], g1 * m[2] * m[3]);
        float life = float(int(r[32] * kLifeR)) + kLife;
        float yaw = r[33] * kAngle + e.yaw, pitch = r[34] * kAngle + e.pitch;
        float dir[3] = { -std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
        SmokeVertex v = {};
        for (int k = 0; k < 3; ++k) {
            v.velocity[k] = kSpeed0 * dir[k];
            v.accel[k] = (kSpeed1 - kSpeed0) * dir[k] / life;
            v.position[k] = pos[k];
        }
        v.accel[1] += kRise;
        v.start = c0;
        v.end = c1;
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
        for (int q = 0; q < 6; ++q) {
            out[q] = v;
            float sx = float(kCorner[q][0]), sy = float(kCorner[q][1]);
            out[q].corner[0] = sx * sx0 * 0.5f;
            out[q].corner[1] = sy * sy0 * 0.5f;
            out[q].growth[0] = sx * (sx1 - sx0) * 0.5f / life;
            out[q].growth[1] = sy * (sy1 - sy0) * 0.5f / life;
            out[q].uv[0] = sx > 0 ? 1.0f : 0.0f;
            out[q].uv[1] = sy > 0 ? 0.0f : 1.0f;
        }
    }
    e.rate -= float(n);
}
