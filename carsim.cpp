// The player's car dynamics (BOOT_TO_MENU.md section 62): a port of 0x4b6a00
// and its helpers, operation by operation (the game's FPU runs in single
// precision, so the float statements give the game's bits). Offsets are
// relative to S = object + 0x10, the game's first argument; T is the handling
// table (vehicle model +0x102c, keys as in section 61).
//
// The wheel records are 0xd0 bytes from S+0x60 (R below):
//   +0x00 compression        +0x04 flags (bit 0 locked, 2 on a body, 3 hit,
//   +0x08 surface                6 present, 7 off, 8 alternative point,
//   +0x10 slip ratio             9 driven)
//   +0x14 contact, +0x24 contact, +0x34 normal, +0x44 tyre direction
//   +0x60 side axis, +0x70 forward axis, +0x80 -normal, +0x90 contact (w 1)
//   +0xa0 contact velocity, +0xb0 skid offset, +0xc0 skid, +0xc4 slip speed
#include "carsim.h"

#include <cmath>

// On x86, MSVC returns a float in st0 and may go on computing with it on the
// x87 at 53-bit precision, which rounds twice where the game rounds once.
// __vectorcall returns floats in xmm0: every operation stays a single-precision
// SSE one (tests\carreplay found exp(-x) 1 ulp off without it).
#if defined(_M_IX86)
#define SSE_RET __vectorcall
#else
#define SSE_RET
#endif

namespace {

struct V4 { float x, y, z, w; };

V4 Load4(const float* p) { return { p[0], p[1], p[2], p[3] }; }
void Store4(float* p, const V4& v) { p[0] = v.x; p[1] = v.y; p[2] = v.z; p[3] = v.w; }

// 0x407640 (eax = a, ecx = b).
V4 Cross(const V4& a, const V4& b)
{
    const float z = a.y * b.x - a.x * b.y;
    const float y = a.x * b.z - a.z * b.x;
    const float x = a.z * b.y - a.y * b.z;
    return { x, y, z, 0 };
}
// 0x41e460 / 0x41ea10 / 0x402760: s v (the game leaves a stale 4th component).
V4 Scale(const V4& v, float s) { return { s * v.x, s * v.y, s * v.z, 0 }; }
// 0x41e4b0 / 0x4076e0 (ecx = a, eax = b): a + b.
V4 Add(const V4& a, const V4& b) { return { a.x + b.x, a.y + b.y, a.z + b.z, 0 }; }
V4 Sub(const V4& a, const V4& b) { return { a.x - b.x, a.y - b.y, a.z - b.z, 0 }; }
V4 Neg(const V4& v) { return { -v.x, -v.y, -v.z, 0 }; }
// 0x417d10: -v over four components.
V4 Neg4(const V4& v) { return { -v.x, -v.y, -v.z, -v.w }; }
// The game's |x| (fcom 0, fchs unless x > 0): -0 for +0, unlike fabsf.
float SSE_RET Abs(float x)
{
    if (x > 0) return x;
    uint32_t u;
    memcpy(&u, &x, 4);
    u ^= 0x80000000u;
    memcpy(&x, &u, 4);
    return x;
}
// 0x41e990: v / |v|.
V4 Normalize3(const V4& v)
{
    const float s = (v.x * v.x + v.y * v.y) + v.z * v.z;
    const float r = 1.0f / sqrtf(s);
    return { r * v.x, r * v.y, r * v.z, 0 };
}
// 0x417ea0: v / |v| over four components.
V4 Normalize4(const V4& v)
{
    const float s = ((v.x * v.x + v.y * v.y) + v.z * v.z) + v.w * v.w;
    const float r = 1.0f / sqrtf(s);
    return { r * v.x, r * v.y, r * v.z, r * v.w };
}
// 0x4b57f0: Normalize3, or (1, 0, 0) for |v|^2 < 1e-10.
V4 NormalizeOrX(const V4& v)
{
    const float s = (v.x * v.x + v.y * v.y) + v.z * v.z;
    if (s < 9.99999944e-11f) return { 1, 0, 0, 0 };
    return Normalize3(v);
}
// 0x4b5860: Normalize3 of a possibly tiny vector (scaled up first).
V4 NormalizeTiny(const V4& v)
{
    const float s = (v.z * v.z + v.y * v.y) + v.x * v.x;
    if (s > 0.25) return Normalize3(v);
    if (s > 1e-30) return Normalize3({ v.x * 1.00000003e+16f, v.y * 1.00000003e+16f, v.z * 1.00000003e+16f, 0 });
    float m = v.x < v.y ? v.x : v.y;
    if (!(m < v.z)) m = v.z;
    if (!(m > 0)) return { 0, 0, 0, 0 };
    const float r = 1.0f / m;
    return Normalize3({ r * v.x, v.y * r, v.z * r, 0 });
}
// D3DXVec4Transform of (x, y, z, 1) (0x605119 jumps through [0x7961d8] to a
// CPU-specific version; the SSE one 0x66cad5 ran in the traces:
// (x m0 + y m1) + (z m2 + 1 m3)). The x87 version 0x605121 adds in another
// order: ((m3 + y m1) + z m2) + x m0 for x.
V4 Transform(const float p[3], const float* m)
{
    float o[3];
    for (int j = 0; j < 3; ++j) o[j] = (p[0] * m[j] + p[1] * m[4 + j]) + (p[2] * m[8 + j] + m[12 + j]);
    return { o[0], o[1], o[2], 0 };
}

// 0x401160: the game's sine (nested Taylor terms after reduction to +-pi/2).
float SSE_RET GameSin(float x)
{
    float sign = 1.0f;
    float x2 = x * x;
    if (x2 > 9.86960506f) {
        const float n = static_cast<float>(floor(static_cast<double>((x + 3.14159274f) * 0.159154937f)));
        x = x - n * 6.28318548f;
        x2 = x * x;
    }
    if (x > 1.57079637f) {
        x = 3.14159274f - x;
        x2 = x * x;
    } else if (x < -1.57079637f) {
        sign = -1.0f;
        x = x + 3.14159274f;
        x2 = x * x;
    }
    float p = 0.00641025649f * x2 - 1.0f;
    p = p * x2 * 0.0090909088f + 1.0f;
    p = p * x2 * 0.013888889f - 1.0f;
    p = p * x2 * 0.0238095243f + 1.0f;
    p = p * x2 * 0.0500000007f - 1.0f;
    p = p * x2 * 0.166666672f + 1.0f;
    return x * (p * sign);
}
// 0x401070: the game's cosine.
float SSE_RET GameCos(float x)
{
    float sign = 1.0f;
    float x2 = x * x;
    if (x2 > 9.86960506f) {
        const float n = static_cast<float>(floor(static_cast<double>((x + 3.14159274f) * 0.159154937f)));
        x = x - n * 6.28318548f;
        x2 = x * x;
    }
    if (x > 1.57079637f) {
        x = 3.14159274f - x;
        sign = -1.0f;
        x2 = x * x;
    } else if (x < -1.57079637f) {
        x = x + 3.14159274f;
        sign = -1.0f;
        x2 = x * x;
    }
    float p = 0.0075757578f * x2 - 1.0f;
    p = p * x2 * 0.0111111114f + 1.0f;
    p = p * x2 * 0.0178571437f - 1.0f;
    p = p * x2 * 0.0333333351f + 1.0f;
    p = p * x2 * 0.0833333358f - 1.0f;
    p = x2 * p;
    p = p * 0.5f + 1.0f;
    return p * sign;
}
// 0x581e00: the game's exp (Taylor below 1, then factors e, e^2, e^4). The
// dynamics only asks for exp(-x) with 0 <= x < 2.
float SSE_RET GameExp(float x)
{
    if (x < -0.75f) return 1.0f / GameExp(-x);
    if (x < 1.0f) {
        float p = x * 0.00833333284f + 0.0416666716f;
        p = p * x + 0.166666701f;
        p = p * x + 0.5f;
        p = p * x + 1.0f;
        return p * x + 1.0f;
    }
    if (x < 2.0f) return GameExp(x - 1.0f) * 2.71828175f;
    if (x < 4.0f) return GameExp(x - 2.0f) * 7.38905621f;
    return GameExp(x - 4.0f) * 54.5981483f;
}

// The surface table 0x71c2e8 (58 entries of 5 floats): the grip column +0xc.
const float kSurfaceGrip[0x3a] = {
    1.0f, 1.0f, 0.5f, 0.5f, 0.1f, 0.5f, 1.0f, 1.0f, 0.5f, 1.0f, 0.3f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 0.5f, 0.5f, 0.1f, 0.5f, 1.0f, 1.0f, 0.5f, 1.0f, 0.3f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
};

// 0x5141d0: vehicles whose wheel rays use the full width.
bool FullWidth(int id)
{
    switch (id) {
    case 0x10: case 0x13: case 0x23: case 0x24: case 0x32: case 0x3c: case 0x40: case 0x4c: case 0x4f:
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The engine (0xa4 bytes at S+0x578; 0x5247d0 builds it): torque curve
// {rpm, torque} pairs at +0 (count +0x30), rise +0x38 / +0x3c, fall +0x40 /
// +0x44, gear count +0x48, ratios +0x4c (gear 0 = reverse), +0x68 automatic,
// down-shift fractions +0x6c / +0x70 (full throttle), +0x74 blend, idle
// +0x78, max +0x7c, +0x81 shifting, rpm +0x84, gear +0x88, target +0x8c,
// throttle +0x90, reverse +0x94, clutch +0x95, smoothed throttle +0x98,
// limiter +0x9c, wheel speed +0xa0.
struct Engine
{
    uint8_t* e;
    float& F(int o) { return *reinterpret_cast<float*>(e + o); }
    int& I(int o) { return *reinterpret_cast<int*>(e + o); }
    uint8_t& B(int o) { return e[o]; }

    // 0x524bc0: torque at rpm (linear between the curve's points).
    float SSE_RET Torque(float rpm)
    {
        const int n = I(0x30);
        int i = 0;
        for (; i < n; ++i) {
            if (rpm == F(i * 8)) return F(i * 8 + 4);
            if (rpm < F(i * 8)) break;
        }
        const float r0 = F(i * 8 - 8), t0 = F(i * 8 - 4);
        const float u = (rpm - r0) / (F(i * 8) - r0);
        return u * (F(i * 8 + 4) - t0) + t0;
    }
    // 0x524b80: how far the rpm is above the gear's rpm, over the range.
    float SSE_RET Lag()
    {
        if (B(0x81)) return 0;
        float t = F(0x4c + I(0x88) * 4) * F(0xa0);
        if (t < F(0x78)) t = F(0x78);
        return (F(0x84) - t) / (F(0x7c) - F(0x78));
    }
    // 0x524c30: the rpm follows the target.
    void FollowTarget()
    {
        if (!B(0x95)) B(0x9c) = 0;
        if (B(0x9c)) {
            const float r = (F(0x84) - F(0x44) * F(0x84)) - F(0x40);
            F(0x84) = r;
            if (r < F(0x7c) * 0.949999988f) B(0x9c) = 0;
            return;
        }
        if (F(0x8c) > F(0x84)) {
            const float r = (F(0x3c) + 1.0f) * F(0x84) + F(0x38);
            F(0x84) = r;
            if (r < F(0x8c)) return;
        } else {
            const float r = (F(0x84) - F(0x44) * F(0x84)) - F(0x40);
            F(0x84) = r;
            if (r > F(0x8c)) return;
        }
        F(0x84) = F(0x8c);
        B(0x81) = 0;
    }
    // 0x5247a0: clamp to idle .. max.
    void Clamp(float& v)
    {
        if (v < F(0x78)) { v = F(0x78); return; }
        if (v > F(0x7c)) v = F(0x7c);
    }
    // The target clamped to idle .. max, then FollowTarget (0x524a25..0x524abd).
    void ClampTargetAndFollow()
    {
        if (F(0x8c) < F(0x78)) F(0x8c) = F(0x78);
        else if (F(0x8c) > F(0x7c)) F(0x8c) = F(0x7c);
        FollowTarget();
    }
    // 0x524d10: automatic shifts.
    void Shift(float speed)
    {
        const int gear = I(0x88);
        if (gear < I(0x48) - 1) {
            const float up = speed * F(0x50 + gear * 4);
            if (up < F(0x7c) && up > F(0x78)) {
                const float tUp = Torque(up);
                if (Torque(F(0x84)) * 1.10000002f < tUp) {
                    I(0x88) = gear + 1;
                    B(0x81) = 1;
                    F(0x8c) = up;
                    return;
                }
            }
        }
        float frac = F(0x6c);
        if (*reinterpret_cast<uint32_t*>(e + 0x90) == 0x3f800000) frac = F(0x70);
        if (gear > 1) {
            const float limit = frac * F(0x7c);
            for (;;) {
                float r = speed * F(0x48 + I(0x88) * 4);
                if (!(r < limit)) break;
                const int g = I(0x88) - 1;
                I(0x88) = g;
                if (r < F(0x78)) r = F(0x78);
                else if (r > F(0x7c)) r = F(0x7c);
                if (B(0x68)) F(0x8c) = (1.0f - F(0x74)) * r + F(0x7c) * F(0x74);
                else F(0x8c) = r;
                B(0x81) = 1;
                if (!(g > 1)) break;
            }
        }
        if (!B(0x68)) return;
        if (I(0x88) == 1) {
            if (speed < 0 && B(0x94)) {
                float r = -(speed * F(0x4c));
                Clamp(r);
                B(0x81) = 1;
                I(0x88) = 0;
                F(0x8c) = r;
            }
        } else if (I(0x88) == 0 && speed > 0 && B(0x94) == 0) {
            float r = speed * F(0x50);
            Clamp(r);
            I(0x88) = 1;
            F(0x8c) = r;
            B(0x81) = 1;
        }
    }
    // 0x5247d0: the engine from its 0x78-byte description, in the gear for
    // the wheel speed (the shift-down loop of 0x524d10 from the top gear).
    void Init(const uint8_t desc[0x78], float speed)
    {
        memcpy(e, desc, 0x78);
        const float idle = F(0);
        F(0x78) = idle;
        F(0x7c) = F(I(0x30) * 8 - 8);
        F(0x84) = idle;
        F(0x8c) = idle;
        B(0x80) = 1;
        B(0x81) = 0;
        F(0x90) = 0;
        B(0x94) = 0;
        B(0x95) = 0;
        F(0x98) = 0;
        B(0x9c) = 0;
        F(0xa0) = 0;
        I(0x88) = I(0x48);
        if (I(0x48) > 1) {
            const float limit = F(0x6c) * F(0x7c);
            for (;;) {
                float r = speed * F(0x48 + I(0x88) * 4);
                if (!(r < limit)) break;
                const int g = I(0x88) - 1;
                I(0x88) = g;
                if (r < idle) r = idle;
                else if (r > F(0x7c)) r = F(0x7c);
                F(0x8c) = r;
                if (!(g > 1)) break;
            }
        }
        F(0x84) = F(0x8c);
    }
    // 0x5248e0 (wheel speed, throttle, clutch, reset, auto shift).
    void Update(float speed, float throttle, bool clutch, bool reset, bool automatic)
    {
        if (reset) {
            B(0x81) = 1;
            F(0x90) = 0;
            B(0x94) = 0;
            F(0x8c) = F(0x78);
        }
        F(0xa0) = speed;
        B(0x95) = clutch;
        if (throttle < 0 && !B(0x68)) throttle = 0;
        else if (throttle > 1.0f) throttle = 1.0f;
        else if (throttle < -1.0f) throttle = -1.0f;
        F(0x90) = throttle;
        if (throttle < 0) {
            B(0x94) = 1;
            F(0x90) = -throttle;
        } else {
            B(0x94) = 0;
        }
        float d;
        if (B(0x81)) d = F(0x8c) > F(0x84) ? (1.0f - F(0x98)) * 0.400000006f : -F(0x98) * 0.400000006f;
        else d = (F(0x90) - F(0x98)) * 0.800000012f;
        F(0x98) = d + F(0x98);
        if (B(0x81)) {
            if (!B(0x68)) F(0x8c) = speed * F(0x4c + I(0x88) * 4);
            ClampTargetAndFollow();
            return;
        }
        if (clutch) {
            if (I(0x88) == 0 && B(0x94) == 0 && F(0x90) > 0) I(0x88) = 1;
            F(0x8c) = (F(0x7c) - F(0x78)) * throttle + F(0x78);
            ClampTargetAndFollow();
            return;
        }
        if (!automatic) {
            F(0x8c) = F(0x78);
            FollowTarget();
            return;
        }
        F(0x8c) = speed * F(0x4c + I(0x88) * 4);
        if (F(0x8c) < F(0x78)) F(0x8c) = F(0x78);
        else if (F(0x8c) > F(0x7c)) F(0x8c) = F(0x7c);
        FollowTarget();
        if (Lag() < 0.100000001f) Shift(speed);
    }
};
}  // namespace

// ---------------------------------------------------------------------------
// 0x4ba260: when the car has just left the ground, a roll impulse that keeps
// it level.
void CarSim::LiftOff()
{
    RigidBody& b = *body;
    const bool hold = U(0x674) != 0;
    float x = b.R[2][1];
    if (!hold && x < 0.300000012f) {
        x = x + 0.100000001f;
        if (0.300000012f < x) x = 0.300000012f;
    }
    float a = x * 0.882352948f;
    if (0 > a) a = 0;
    if (!hold) {
        const float d = (b.R[0][2] * b.w[2] + b.R[0][1] * b.w[1]) + b.R[0][0] * b.w[0];
        float v = -d * 1.5f;
        if (0 > v) v = 0;
        a = (v + 0.5f) * a;
    }
    const float ang[4] = { a * b.R[0][0] * b.inertia[0], a * b.R[0][1] * b.inertia[0], a * b.R[0][2] * b.inertia[0], 0 };
    const float lin[4] = { 0, 0, 0, 0 };
    b.ApplyImpulse(lin, ang);
}

// ---------------------------------------------------------------------------
// 0x4b0080: the previous 25 Hz state at obj+0x690 (steer, drive target,
// wheel speed, then per wheel 0xa8 bytes: compression, contact, contact,
// normal, tyre direction, the axes, contact velocity, skid, slip speed).
void CarSim::Snapshot()
{
    uint8_t* d = obj + 0x690;
    memcpy(d, &F(0x564), 4);
    memcpy(d + 4, &F(0x548), 4);
    memcpy(d + 8, &F(0x54c), 4);
    for (int w = 0; w < 6; ++w) {
        const uint8_t* R = obj + 0x10 + 0x60 + w * 0xd0;
        uint8_t* o = d + 0x10 + w * 0xa8;
        const uint32_t zero = 0;
        memcpy(o, R, 4);
        memcpy(o + 0x04, R + 0x14, 12); memcpy(o + 0x10, &zero, 4);
        memcpy(o + 0x14, R + 0x24, 12); memcpy(o + 0x20, &zero, 4);
        memcpy(o + 0x24, R + 0x34, 12); memcpy(o + 0x30, &zero, 4);
        memcpy(o + 0x34, R + 0x44, 12); memcpy(o + 0x40, &zero, 4);
        memcpy(o + 0x50, R + 0x60, 64);
        memcpy(o + 0x90, R + 0xa0, 16);
        memcpy(o + 0xa0, R + 0xc0, 8);
    }
}

void CarSim::Create(RigidBody* carBody, const float pos[3], float heading)
{
    const float* T = table;
    body = carBody;
    // 0x4afd70 (on S): the wheel records (0x4afbe0) and the state.
    for (int w = 0; w < 6; ++w) {
        uint8_t* R = obj + 0x10 + 0x60 + w * 0xd0;
        auto RF = [&](int o) -> float& { return *reinterpret_cast<float*>(R + o); };
        *reinterpret_cast<uint32_t*>(R + 4) &= 0xffffe030u;
        RF(0) = 0;
        *reinterpret_cast<int*>(R + 8) = -1;
        RF(0x10) = 0;
        Store4(&RF(0x14), { 0, 0, 0, 0 });
        Store4(&RF(0x24), { 0, 0, 0, 0 });
        Store4(&RF(0x34), { 0, 1, 0, 0 });
        Store4(&RF(0x44), { 1, 0, 0, 0 });
        RF(0xc0) = 0;
        RF(0xc4) = 0;
        for (int i = 0; i < 16; ++i) RF(0x60 + 4 * i) = (i % 5 == 0) ? 1.0f : 0.0f;
        Store4(&RF(0xb0), { 0, 0, 0, 0 });
    }
    for (int s = 0x540; s <= 0x550; s += 4) U(s) = 0;
    for (int s = 0x558; s <= 0x570; s += 4) U(s) = 0;
    B(0x574) = 0; B(0x5f8) = 0; B(0x61c) = 0;
    U(0x64c) = 0; U(0x650) = 0; B(0x654) = 0; U(0x658) = 0;
    B(0x65c) = 0; B(0x668) = 0; B(0x669) = 0; B(0x670) = 0; B(0x671) = 0;
    F(0x660) = 1.0f; F(0x664) = 1.0f;
    for (int i = 0; i < 16; ++i) F(4 * i) = (i % 5 == 0) ? 1.0f : 0.0f;
    B(0x66a) = 1;
    U(0x66c) = 1;
    U(0x674) = 4;

    // 0x4b45a0 (on the object) with 0x4b6500.
    OU(0xbd0) = 0;
    obj[0xbd4] = obj[0xbd5] = obj[0xbd7] = obj[0xbd8] = 0;
    OU(0xb98) = 0; OU(0xbdc) = 0; OU(0xbe0) = 0; OU(0xb84) = 0;
    {
        // 0x4b6500: the body (0x5a0b60 creates it from the model's shapes;
        // here: at rest, the heading as a turn about y), the rest
        // compression 0x5138c0 on every wheel, inertia and mass, the engine.
        RigidBody& b = *body;
        b = RigidBody();
        const float a = heading * 0.5f;
        const float c = GameCos(a), s = GameSin(a);
        b.q[0] = 0; b.q[1] = s; b.q[2] = 0; b.q[3] = c;
        b.pos[0] = pos[0]; b.pos[1] = pos[1]; b.pos[2] = pos[2]; b.pos[3] = 0;
        float rest = -T[0x8c] * T[0] * 0.01f / T[1];
        if (!(1.0f >= rest)) rest = 1.0f;
        else if (rest < 0.0f) rest = 0.0f;
        for (int w = 5; w >= 0; --w) F(0x60 + w * 0xd0) = rest;
        const V4 I = Scale({ T[0xf], T[0x10], T[0x11], 0 }, T[0x1a]);
        b.mass = T[0];
        b.inertia[0] = I.x; b.inertia[1] = I.y; b.inertia[2] = I.z; b.inertia[3] = 0;
        b.UpdateDerived();
        b.UpdateMatrix();
        b.accel[0] = b.accel[1] = b.accel[2] = b.accel[3] = 0;
        B(0x575) = 0;
        // The engine's description (0x4b676d..0x4b695c): curve (T[0x28], 0.9),
        // (T[0x29], 1), (T[0x2a], 0); T[0x2b]; rise T[0x2c] 0.04, T[0x2d]
        // 0.04; fall T[0x2e] 0.04, T[0x2f] 0.04; 7 ratios T[0x29] / (T[0x30
        // + i] x 0.277777791); automatic; T[0xb8..0xba].
        uint8_t desc[0x78] = {};
        auto DF = [&](int o, float v) { memcpy(desc + o, &v, 4); };
        auto DI = [&](int o, int v) { memcpy(desc + o, &v, 4); };
        DF(0x00, T[0x28]); DF(0x04, 0.899999976f);
        DF(0x08, T[0x29]); DF(0x0c, 1.0f);
        DF(0x10, T[0x2a]); DF(0x14, 0.0f);
        DI(0x30, 3);
        DF(0x34, T[0x2b]);
        DF(0x38, T[0x2c] * 0.0399999991f);
        DF(0x3c, T[0x2d] * 0.0399999991f);
        DF(0x40, T[0x2e] * 0.0399999991f);
        DF(0x44, T[0x2f] * 0.0399999991f);
        DI(0x48, 7);
        for (int i = 0; i < 7; ++i) DF(0x4c + 4 * i, T[0x29] / (T[0x30 + i] * 0.277777791f));
        desc[0x68] = 1;
        DF(0x6c, T[0xb8]); DF(0x70, T[0xb9]); DF(0x74, T[0xba]);
        const float speed = sqrtf((b.vel[0] * b.vel[0] + b.vel[2] * b.vel[2]) + b.vel[1] * b.vel[1]);
        Engine{ obj + 0x10 + 0x578 }.Init(desc, speed);
        // The water state 0x524f00 keeps.
        B(0x61c) = 1;
        U(0x648) = 0;
        for (int i = 0; i < 4; ++i) U(0x638 + 4 * i) = 0, F(0x628 + 4 * i) = 0.5f;
    }
    U(0x56c) = 1;                 // obj+0x57c: the handbrake on
    U(0x558) = 0;                 // obj+0x568: the wheel count
    OU(0xba0) = 0;
    for (int w = 0; w < 6; ++w) {
        uint32_t& flags = U(0x64 + w * 0xd0);
        if (slots[w] != 0) {
            flags |= 0x40;
            U(0x558) += 1;
        }
        OF(0xc84 + 4 * w) = width[w];
        flags &= 0xfffffe7fu;
        if (w < 5) OU(0xba4 + 4 * w) = 0;
    }
    OU(0xb9c) = (U(0x64 + 3 * 0xd0) & 0x40) ? 3 : 5;
    obj[0xbd6] = static_cast<uint8_t>((U(0x64 + OU(0xb9c) * 0xd0) >> 6) & 1);
    for (int w = 0; w < 4; ++w) U(0x64 + w * 0xd0) |= 0x200;
    if (!(U(0x64 + 2 * 0xd0) & 0x40) || !(U(0x64 + 3 * 0xd0) & 0x40)) U(0x64 + 5 * 0xd0) |= 0x200;
    if (!(U(0x64) & 0x40) || !(U(0x64 + 1 * 0xd0) & 0x40)) U(0x64 + 4 * 0xd0) |= 0x200;
    U(0x64 + 2 * 0xd0) |= 0x800;
    U(0x64) |= 0x800;
    U(0x64 + 1 * 0xd0) |= 0x1400;
    U(0x64 + 3 * 0xd0) |= 0x1400;
    OU(0xb7c) = 0; OU(0xb80) = 0;
    memcpy(obj + 0xac0, body->matrix, 64);
    OU(0xb00) = 0;
    Snapshot();
    OU(0xb90) = 0; OU(0xb8c) = 0; OU(0xb88) = 0; OF(0xb94) = 1.0f;
    B(0x65c) = 0; B(0x669) = 0;
    F(0x660) = 1.0f; F(0x664) = 1.0f;
    B(0x670) = 0; B(0x671) = 0;
    U(0x66c) = 1;
    obj[0xbd9] = 0;
}

// ---------------------------------------------------------------------------
// 0x5a9230 -> 0x5af330: a sleeping body (0x78) is awake again (0x65) with its
// sleep counters cleared.
void CarSim::WakeBody()
{
    RigidBody& b = *body;
    if (b.state != 0x78) return;
    b.state = 0x65;
    for (float& s : b.sleep) s = 0;
}

// 0x5a96c0 with the tick's 0x4b2860..: the body stops (L and v zero, then q
// normalised and R, w again) and sleeps (0x78).
void CarSim::SleepBody()
{
    RigidBody& b = *body;
    b.L[0] = b.L[1] = b.L[2] = b.L[3] = 0;
    b.UpdateDerived();
    WakeBody();
    b.vel[0] = b.vel[1] = b.vel[2] = b.vel[3] = 0;
    WakeBody();
    b.state = 0x6e;
    b.UpdateDerived();
    b.state = 0x78;
}

// 0x5e49a5 (_finite) on the controls, then 0x4b1710.
void CarSim::SetControls(const float c[5], bool handbrake)
{
    const float* T = table;
    static const int kOrder[5] = { 2, 3, 4, 1, 0 };
    for (int k : kOrder)
        if (!std::isfinite(static_cast<double>(c[k]))) return;
    if (B(0x654)) {   // under water
        OU(0x570) = 0; OU(0x574) = 0; OU(0x57c) = 0; OU(0xbd0) = 0;
        return;
    }
    float brake = c[2], drive = 0;
    bool stopped = false;
    // 0x514150: the vehicles whose brake drives (the cars among them).
    switch (vehicleId) {
    case 0xf: case 0x13: case 0x24: case 0x27: case 0x31: case 0x36: case 0x3a: case 0x3c: case 0x40: case 0x49:
    case 0x4c: case 0x56: case 0x57:
        drive = c[2];
        brake = 0;
    }
    if (c[0] == 0 && c[1] == 0 && !(c[2] > 0)) {
        // No input: a car standing on a wheel that is off counts as stopped.
        if (static_cast<int>(OU(0xbdc)) < 2) {
            for (int w = 0; w < 6; ++w) {
                const uint32_t f = U(0x64 + w * 0xd0);
                if (!(f & 0x40)) continue;
                if (!(f & 0x80)) continue;
                const float* v = body->vel;
                if (!((v[2] * v[2] + v[1] * v[1]) + v[0] * v[0] > 0.0225000009f)) stopped = true;
                break;
            }
        }
    } else {
        OU(0xbe0) = 0;
        if (OU(0xbdc) == 2) {
            WakeBody();
            OU(0xbdc) = 0;
        }
    }
    if (c[0] != 0) drive = c[0];
    F(0x650) = c[0];
    F(0x64c) = c[0];
    if (brake > 0) {
        const float t = 1.0f - brake;
        F(0x64c) = (1.0f - t * t * t) * 0.699999988f + 0.300000012f;
    }
    B(0x574) = (B(0x670) || -T[0xa6] > drive) ? 1 : 0;
    if (drive < 0) {
        // Reverse: T[0x13] / T[0x12] (negative), less above T[0x16] wheel
        // speed (the game reads it through the vehicle, [+0x43c] vtbl+0x10).
        const float x = T[0x16] - F(0x54c);
        const float ratio = T[0x13] / T[0x12];
        const float r = x <= 0 ? ratio : (1.0f - x / T[0x17]) * ratio;
        drive = -(r * drive);
    }
    F(0x560) = T[0x12] * drive;
    F(0x564) = c[1] * 352.709991f;
    U(0x56c) = (stopped || handbrake) ? 1 : 0;
    if (brake > 0) {
        const float k = drive < 0 ? Abs(T[0x13] * drive * 0.99000001f) : 0.0f;
        F(0x560) = T[0x12] - k;
    }
    float f;
    if (!(F(0x54c) < 26.8211555f)) f = 0;
    else if (F(0x54c) < 13.4105778f) f = 1.0f;
    else f = 1.0f - (F(0x54c) - 13.4105778f) * 0.0745680034f;
    F(0x55c) = F(0x564);
    OF(0xbd0) = f * brake;
    if (vehicleId == 0x14 || vehicleId == 0x58) F(0x564) = 0;   // 0x514410: no steering
}

// ---------------------------------------------------------------------------
bool CarSim::Tick(WheelRayCaster& world)
{
    memcpy(obj + 0xac0, body->matrix, 64);
    OU(0x684) = 0;   // the game vehicle's state (vtbl+8): 0 while driven
    const float acc = OF(0xb7c) + 0.00833333377f;
    OF(0xb7c) = acc;
    bool ran = false;
    if (acc > 0.0399999991f) {
        OF(0xb7c) = acc - 0.0399999991f;
        Snapshot();
        // 0x4b4b30: the brake obj+0xbd0 to S+0x568; the clutch while
        // braking slower than T[0x15].
        F(0x568) = OF(0xbd0);
        B(0x668) = (0 < OF(0xbd0) && F(0x550) < table[0x15]) ? 1 : 0;
        U(0x66c) = 1;    // the ray filter for states 0..4
        // A sleeping car (obj+0xbdc == 2) skips the dynamics.
        if (static_cast<int>(OU(0xbdc)) < 2) {
            for (int w = 0; w < 6; ++w) {
                uint32_t& flags = U(0x64 + w * 0xd0);
                flags = (flags & ~0x180u) | (wheelAlt[w] ? 0x100u : 0u) | (wheelOff[w] ? 0x80u : 0u);
            }
            Dynamics(world);
            ran = true;
        }
    }
    // Sleep (0x4b2290..0x4b2370): after 4 s (timer obj+0xbe0) with speed
    // <= 0.0625 and spin^2 <= 1/256, the body sleeps (obj+0xbdc = 2); any
    // motion wakes it.
    const float* v = body->vel;
    const float* w = body->w;
    float speed = sqrtf((v[1] * v[1] + v[2] * v[2]) + v[0] * v[0]);
    OF(0xbe0) = OF(0xbe0) + 0.00833333377f;
    float spin;
    if (vehicleId == 0x14 || vehicleId == 0x58) {
        speed = speed * 0.125f;
        const float wx = w[0] * 0.5f, wy = w[1] * 0.5f, wz = w[2] * 0.5f;
        spin = wx * wx + (wy * wy + wz * wz);
    } else {
        spin = w[0] * w[0] + (w[1] * w[1] + w[2] * w[2]);
    }
    if (speed > 0.0625f || spin > 0.00390625f || (OU(0xbdc) == 2 && body->state != 0x78)) {
        OF(0xbe0) = 0;
        if (OU(0xbdc) == 2) {
            WakeBody();
            OU(0xbdc) = 0;
        }
    }
    if (4.0f < OF(0xbe0) && OU(0xbdc) == 0) {
        SleepBody();
        OU(0xbdc) = 2;
    }
    return ran;
}

// ---------------------------------------------------------------------------
void CarSim::Dynamics(WheelRayCaster& world)
{
    const float* T = table;
    RigidBody& b = *body;
    auto Row = [&](int i) { return Load4(&F(i * 16)); };

    if (B(0x575) == 1) LiftOff();

    // The impulse accumulators: angular S+0x40, linear S+0x50.
    for (int i = 0; i < 4; ++i) F(0x50 + 4 * i) = 0, F(0x40 + 4 * i) = 0;
    // The body's matrix (S+0x00..0x3f: rows x, up, z, position).
    memcpy(&F(0), b.matrix, 64);
    const V4 vel = { b.vel[0], b.vel[1], b.vel[2], 0 };
    const V4 spin = { b.w[0], b.w[1], b.w[2], 0 };

    // Planar speed: max + min / 2 of |vx|, |vz| (S+0x550), and the factor
    // the tyre slip is scaled with.
    {
        const float ax = Abs(vel.x);
        const float az = Abs(vel.z);
        F(0x550) = ax >= az ? az * 0.5f + ax : ax * 0.5f + az;
    }
    float speedFactor = F(0x550) < T[6] ? 0.90234375f - F(0x550) / T[7] : 0.34765625f - F(0x550) / T[8];
    if (speedFactor > 0.8056640625f) speedFactor = 0.8056640625f;
    const float slipScale = T[10] * speedFactor;

    // The engine, fed with the wheel speed S+0x54c and the throttle S+0x64c.
    if (!B(0x66a)) {
        const bool reset = (U(0x2d4) & 1) && (U(0x134) & 1);
        const bool clutch = B(0x668) || (!(F(0x2d0) > 0) && !(F(0x130) > 0));
        Engine{ obj + 0x10 + 0x578 }.Update(F(0x54c), F(0x64c), clutch, reset, B(0x65c) != 0);
    }
    B(0x66a) = 0;

    // Grip split between the front (g) and the rear (h) from the drive input
    // S+0x560 (accelerate > 0, reverse < 0), the brake S+0x568, the handbrake
    // S+0x56c and the drift S+0x570.
    float g = F(0x560) < 0 ? 0.354736328f : (F(0x560) > 0 ? 0.2001953125f : 0.228759766f);
    g8afa00 = F(0x570);
    if (F(0x570) > 0 && F(0x548) > 0) g = g + (1.0f - F(0x568)) * F(0x570) * 0.145999998f;
    const bool reversing = F(0x560) < 0 && F(0x568) == 0 && F(0x548) > T[0x18] && B(0x574) != 0;
    for (int w = 0; w < 6; ++w) U(0x64 + w * 0xd0) = (U(0x64 + w * 0xd0) & ~1u) | (reversing ? 1u : 0u);
    if (U(0x56c) != 0) {
        if (!(F(0x560) < 0)) F(0x560) = 0;
        g = F(0x548) > 0 ? g + 0.16015625f : g - 0.091552734375f;
        for (int w = 5; w >= 1; --w)
            if (w != 2) U(0x64 + w * 0xd0) |= 1;  // the handbrake locks every wheel but 0 and 2
        F(0x568) = 0;
    } else if (F(0x568) > 0) {
        g = g + F(0x568) * 0.146484375f;
    }
    if (F(0x548) < 0 && !(F(0x560) < 0) && U(0x56c) == 0) g = g - 0.09765625f;
    float rearGrip = 0.46875f - g;
    float frontGrip;
    if (F(0x560) < 0 && F(0x548) > T[0x18] && B(0x574)) {
        frontGrip = g * 1.25f;
        rearGrip = rearGrip * 1.25f;
    } else {
        frontGrip = (U(0x64) & 2) ? g * 1.21875f : g * 1.09375f;
    }
    // The drive falls off as the wheel speed nears the top speed: 1 - x^64.
    float driveFall;
    if (B(0x670)) {
        driveFall = 1.0f;
    } else {
        const float top = F(0x560) > 0 ? T[0xa0] : -T[0xbb];
        float x = F(0x54c) / (top * 0.447019249f);
        if (1.0f < x) x = 1.0f;
        else if (0 > x) x = 0;
        const float x2 = x * x, x3 = x2 * x, x4 = x * x3;
        const float y2 = x4 * x4, y3 = y2 * x4, y4 = x4 * y3;
        const float z2 = y4 * y4, z3 = z2 * y4, z4 = z3 * y4;
        driveFall = 1.0f - z4;
    }
    // The drive target S+0x548 and the wheel spin S+0x54c.
    if (F(0x568) > 0) F(0x548) = T[0xb] * F(0x568);
    else if (F(0x560) > 0) F(0x548) = T[0xb];
    else if (F(0x560) < 0) F(0x548) = -T[0xc];
    else F(0x548) = F(0x548) * 0.850000024f;
    F(0x54c) = F(0x54c) * 0.850000024f;
    if (B(0x575) < 100) B(0x575) = static_cast<uint8_t>(B(0x575) + 1);
    unsigned driven = 0;
    for (int w = 5; w >= 0; --w) {
        const uint32_t f = U(0x64 + w * 0xd0);
        if (((f >> 9) & 1) && !(f & 0x80)) ++driven;
    }

    // ---- the wheel rays (0x4b6430, 0x4b5a30), cast together (0x5a0820)
    const V4 up = Row(1);
    const float side = FullWidth(vehicleId) ? 1.0f : 0.5f;
    WheelRay rays[6];
    int nrays = 0;
    for (int w = 5; w >= 0; --w) {
        uint32_t& flags = U(0x64 + w * 0xd0);
        flags &= ~0x30u;
        F(0x60 + w * 0xd0 + 0xc0) = 0;
        if (!((flags >> 6) & 1) || (flags & 0x80)) continue;
        // 0x4b63d0: hardpoint + offset, less the radius (+0x98c with flag 8).
        const float drop = (flags & 0x100) ? altDrop : radius[w];
        const float local[3] = { (wheelOffset[w][0] + hardpoint[w][0]) - 0.0f,
                                 (wheelOffset[w][1] + hardpoint[w][1]) - drop,
                                 (wheelOffset[w][2] + hardpoint[w][2]) - 0.0f };
        const V4 p = Transform(local, &F(0));
        const float s = (w == 3 || w == 5 || w == 2) ? -side : side;
        const V4 sx = Scale(Scale(Row(0), width[w]), s);
        const V4 point = Add(sx, p);
        WheelRay& r = rays[nrays++];
        Store4(r.start, Add(Scale(up, 0.5f), point));
        Store4(r.dir, Neg(up));
        r.length = Abs(up.y) * 2.4000001f + 0.600000024f;
        r.filter = *reinterpret_cast<uint16_t*>(&B(0x66c));
    }
    WheelHit hits[6];
    world.Cast(rays, hits, nrays);
    const V4 negUp = Neg(up);

    // ---- the forces, wheel by wheel
    int k = 0;                   // the ray of this wheel
    unsigned grounded = 0;       // B2c: wheels in contact while wheel 0 is driven
    bool rayFlag = false;        // B30
    bool rearSlides = false;     // B27
    for (int w = 5; w >= 0; --w) {
        uint8_t* R = obj + 0x10 + 0x60 + w * 0xd0;
        auto RF = [&](int o) -> float& { return *reinterpret_cast<float*>(R + o); };
        uint32_t& flags = *reinterpret_cast<uint32_t*>(R + 4);
        if (!((flags >> 6) & 1) || (flags & 0x80)) continue;

        // 0x4b5bc0: the ray's answer.
        const WheelRay& ray = rays[k];
        const WheelHit& hit = hits[k];
        ++k;
        const V4 start = Load4(ray.start);
        V4 base = Sub(start, Scale(up, 0.5f));
        float f8;
        V4 N, contact;
        RigidBody* other;
        if (hit.type == 2) {
            f8 = hit.f8;
            *reinterpret_cast<int*>(R + 8) = hit.surface;
            N = { hit.normal[0], hit.normal[1], hit.normal[2], 0 };
            contact = Add(Scale(up, -hit.distance), start);
            other = hit.body;
        } else {
            f8 = -1.0f;
            *reinterpret_cast<int*>(R + 8) = -1;
            N = { 0, 1, 0, 0 };
            contact = Add(Scale(up, -3.0f), start);
            other = nullptr;
        }
        if (!(f8 < 0)) {
            flags |= 8;
            if (f8 == 0) rayFlag = true;
        }
        // Back from the ray's side point to the wheel.
        const float s = (w == 3 || w == 5 || w == 2) ? side : -side;
        const V4 sx = Scale(Scale(Row(0), width[w]), s);
        base = Add(sx, base);
        contact = Add(sx, contact);
        flags = (flags & ~4u) | (other ? 4u : 0u);

        // The wheel's axes: side R+0x60 (from the steered heading A and the
        // normal), forward R+0x70, -normal R+0x80.
        const V4 back = Neg4(Row(0));   // B278
        const V4 negZ = Neg4(Row(2));   // B1b0
        V4 A = back;
        if (w == 0 || w == 2) {
            const float a = F(0x564) * 0.00153398083f;
            const float sn = GameSin(a), cs = GameCos(a);
            A = Add(Scale(negZ, sn), Scale(back, cs));
        }
        const V4 negN = Neg4(N);
        V4 C = Cross(negN, A);
        if (static_cast<double>((C.z * C.z + C.y * C.y) + C.x * C.x) < 0.01) {
            const float d = ((negN.x * negZ.x + negN.w * negZ.w) + negN.z * negZ.z) + negN.y * negZ.y;
            C = { negZ.x - negN.x * d, negZ.y - negN.y * d, negZ.z - negN.z * d, negZ.w - negN.w * d };
        }
        Store4(&RF(0x80), negN);
        const V4 sideAxis = Normalize4(C);
        Store4(&RF(0x60), sideAxis);
        Store4(&RF(0x70), Cross(sideAxis, negN));
        Store4(&RF(0x90), { contact.x, contact.y, contact.z, 1.0f });

        // Grip from the surface and the setup.
        const int surface = *reinterpret_cast<int*>(R + 8);
        const float sg = (surface >= 0 && surface < 0x3a) ? kSurfaceGrip[surface] : 1.0f;
        const float grip = (32400.0f - setupValue * 7000.0f) * sg * 3.0517578125e-05f + 0.1220703125f;  // B14
        float slope = 1.0f;          // B254: on steep non-road surfaces
        if (surface != 0 && surface != 6 && surface != 0x17) {
            const float ay = Abs(up.y);
            if (ay <= 0.865999997f) slope = ay * ay * ay;
        }
        const V4 lift = U(0x56c) ? V4{ 0, 1, 0, 0 } : up;   // B15c
        flags &= ~2u;

        // The suspension: compression along up, clamped to T[3].
        const V4 Dv = Sub(contact, base);
        float comp = ((Dv.z * up.z + Dv.y * up.y) + Dv.x * up.x) + T[4];
        if (comp <= 0) comp = comp <= -0.300000012f ? 0.0f : 1e-20f;
        if (comp > T[3]) comp = T[3];
        const float oldComp = RF(0);
        V4 tyreDir;   // Ba0
        if (comp <= 0 && oldComp <= 0) {
            RF(0) = 0;
            RF(0xc4) = 0;
            tyreDir = Row(0);
            N = up;
        } else {
            if ((U(0x64) >> 9) & 1) ++grounded;
            B(0x575) = 0;
            // Tyre axes in the ground plane: lat (sideways), fwd (forward),
            // and the steered direction for the front wheels.
            const V4 lat = NormalizeOrX(Cross({ F(0x20), F(0x24), F(0x28), 0 }, N));
            const V4 fwd = Cross(N, lat);
            const float a = F(0x564) * 0.00153398083f;
            const V4 fs = Scale(fwd, GameSin(a));
            const V4 lc = Scale(lat, GameCos(a));
            const V4 steer = Add(lc, fs);   // B210
            // Spring: (T1 + T2) comp - T2 old, times the normal's share of up.
            const float kspring = (T[1] + T[2]) * comp - T[2] * oldComp;
            const float ay = Abs(up.y);
            const float dn = (N.x * up.x + N.z * up.z) + N.y * up.y;
            float spring = (dn > ay ? dn : ay) * kspring;
            if (spring <= 0) spring = 0;
            if (spring > T[1]) spring = T[1];
            // The contact's velocity (less the other body's).
            const V4 r = Sub(base, { F(0x30), F(0x34), F(0x38), 0 });   // B8c
            const V4 cr = Cross(r, spin);
            V4 V = { vel.x + cr.x, vel.y + cr.y, cr.z + vel.z, 0 };
            if (other) {
                const V4 pos2 = Load4(other->pos);   // (a type-1 body would use +0x40)
                const V4 v2 = { other->vel[0], other->vel[1], other->vel[2], 0 };
                const V4 w2 = { other->w[0], other->w[1], other->w[2], 0 };
                V4 r2 = Sub(contact, pos2);
                if (!((w2.z * w2.z + w2.y * w2.y) + w2.x * w2.x < 9.99999944e-11f)) {
                    const V4 n = Normalize3(w2);
                    const float d = (n.z * r2.z + n.y * r2.y) + r2.x * n.x;
                    r2 = Sub(r2, Scale(n, d));
                }
                V = Sub(V, Add(Cross(r2, w2), v2));
            }
            Store4(&RF(0xa0), V);
            const bool locked = flags & 1;
            if (!locked) tyreDir = (w == 0 || w == 2) ? steer : lat;
            else tyreDir = NormalizeTiny(Cross(Cross(negN, V), negN));
            tyreDir.w = 0;
            // Tyre slip along the tyre direction.
            const float sv = (V.z * tyreDir.z + V.y * tyreDir.y) + tyreDir.x * V.x;
            RF(0xc4) = speedFactor * sv;
            float q = sv * slipScale;
            const float ratio = Abs(q) / T[9];
            RF(0xc0) = 0;
            RF(0x10) = ratio;
            if (ratio > 1.0f) {
                const float sk = (ratio - 1.0f) * 0.142857149f;
                RF(0xc0) = sk;
                if (sk > 1.0f) RF(0xc0) = 1.0f;
            }
            const float lim = T[9] * F(0x664);
            if (q > lim) q = lim;
            else if (q < -lim) q = -lim;
            // Drive.
            float drive = (1.0f - static_cast<float>(B(0x5f9))) * F(0x660) * F(0x560) * driveFall;
            if (F(0x560) > 0 && grip != 0) drive = drive / grip * 1.11083984f;
            V4 force = { 0, 0, 0, 0 };   // B78
            const float vf = (V.x * fwd.x + V.z * fwd.z) + V.y * fwd.y;
            if (w == 0 || w == 2) {
                q = q * frontGrip;
                if (locked) {
                    q = q * 0.75f;
                    const float d = (steer.z * tyreDir.z + steer.y * tyreDir.y) + tyreDir.x * steer.x;
                    float m = -(d * q);
                    m = m + m;
                    force = Add(Scale(steer, m), force);
                }
                F(0x54c) = vf;
            } else {
                if (!locked) {
                    q = q * rearGrip;
                    g8af9fc = F(0x564);
                    g8af9f8 = q;
                    if (q * F(0x55c) < 0) rearSlides = true;
                    force = Add(Scale(fwd, drive), force);
                } else {
                    q = q * frontGrip * 0.25f;
                }
                if (F(0x568) == 0) F(0x548) = vf;
            }
            force = Add(Scale(tyreDir, -q), force);
            const bool alt = (flags >> 8) & 1;
            force = Scale(force, alt ? 0.5f : 1.0f);
            const float damp = (-1.0f / static_cast<float>(driven)) * T[0xd];   // B28
            force = Add(Scale(Scale(V, alt ? 1.25f : 1.0f), damp), force);
            force = Scale(force, grip);
            if (F(0x560) > 0) {
                const float m = alt ? 1.25f : 1.0f;
                force = Add(Scale(fwd, vf * (1.11083984f - grip) * damp * m), force);
            }
            force = Scale(force, slope);
            force = Add(Scale(lift, spring), force);

            // The skid offset R+0xb0: the contact's sideways drift, decaying.
            V4 skid = Load4(&RF(0xb0));
            if (Abs(drive) > 0.00200000009f && w != 0 && w != 2 && !locked) {
                skid = { 0, 0, 0, 0 };
            } else {
                const V4 sa = Load4(&RF(0x60));
                float spd = Abs(vf) * 8.0f * 0.0399999991f;
                V4 Vs = V;
                if (!locked) {
                    const float d = (Vs.x * sa.x + Vs.z * sa.z) + Vs.y * sa.y;
                    Vs = Sub(Vs, Scale(sa, d));
                } else {
                    spd = sqrtf((V.z * V.z + V.y * V.y) + V.x * V.x) * 28.0f * 0.0399999991f;
                }
                const float L1 = sqrtf((Vs.x * Vs.x + Vs.z * Vs.z) + Vs.y * Vs.y);
                if (L1 > 0.0299999993f) Vs = Scale(Vs, 0.0299999993f / L1);
                skid = Add(Scale(Vs, 0.0399999991f), skid);
                if (spd < 2.0f) skid = Scale(skid, GameExp(-spd));
                else skid = { 0, 0, 0, 0 };
                const float L2 = sqrtf((skid.x * skid.x + skid.z * skid.z) + skid.y * skid.y);
                if (L2 > 0.0299999993f) skid = Scale(skid, 0.0299999993f / L2);
                if (!locked) {
                    const float d = (sa.x * skid.x + sa.z * skid.z) + sa.y * skid.y;
                    skid = Sub(skid, Scale(sa, d));
                }
                const float d = (skid.x * negN.x + skid.z * negN.z) + skid.y * negN.y;
                skid = Sub(skid, Scale(negN, d));
            }
            Store4(&RF(0xb0), { skid.x, skid.y, skid.z, 0 });
            force = Add(Scale(Scale(Scale(skid, 25.0f), T[0] * -0.0399999991f), grip), force);

            // Less force as the car tips over.
            const float upY = F(0x14);
            if (flag8afa04) {
                force = Scale(force, upY < 0 ? 0.0f : upY);
            } else if (upY < 0.5f) {
                const float t = 1.0f - upY;
                force = Scale(force, t > 1.0f ? 0.0f : 1.0f - t * t);
            }

            // The lever: the contact moved T[0x1e] along up from the centre's
            // level and T[0x20] back along z.
            V4 lever = r;
            const float d = ((negUp.z * lever.z) + (negUp.y * lever.y) + lever.x * negUp.x) - T[0x1e];
            lever = Sub(lever, Scale(negUp, d));
            lever = Sub(lever, Scale(Row(2), T[0x20]));
            if ((U(0x64) >> 9) & 1) {
                if (arg4) {
                    const float du = (force.z * up.z + force.y * up.y) + up.x * force.x;
                    force = Scale(up, du);
                }
                // 0x4b59c0: linear and angular impulse.
                Store4(&F(0x40), Add(Cross(force, lever), Load4(&F(0x40))));
                Store4(&F(0x50), Add(force, Load4(&F(0x50))));
            }
            RF(0) = comp;
            // (A dynamic body under the wheel gets the reaction through the
            // collision world, 0x4b9a54..0x4b9ee7: not ported.)
        }
        // The contact, normal and tyre direction records.
        Store4(&RF(0x14), { contact.x, contact.y, contact.z, 0 });
        Store4(&RF(0x24), { contact.x, contact.y, contact.z, 0 });
        Store4(&RF(0x34), { N.x, N.y, N.z, 0 });
        Store4(&RF(0x44), { tyreDir.x, tyreDir.y, tyreDir.z, 0 });
    }

    // ---- the drift S+0x570 builds up while the rear slides at speed
    {
        const float a = Abs(F(0x55c));
        if (a > 3.0f && !rearSlides && reinterpret_cast<const uint8_t*>(T)[0x74]) {
            float t = a * 0.00370370364f;
            if (t > 1.0f) t = 1.0f;
            const float v = 0.0769230798f * t + F(0x570);
            F(0x570) = v > t ? t : v;
        } else {
            F(0x570) = 0;
        }
    }
    // ---- angular damping about the body's z column, with the inertia
    {
        const V4 I = Scale(Load4(b.inertia), 1.0f / T[0x1a]);
        const float kd = grounded > 2 ? T[0xe] : 0.0f;
        const V4 u = { F(0x08), F(0x18), F(0x28), 0 };
        const float t1 = -(kd * I.y);
        const V4 ws = Scale(spin, t1);
        const float d = (spin.x * u.x + spin.z * u.z) + spin.y * u.y;
        const float c = -(kd * I.x) - t1;
        Store4(&F(0x40), Add(Add(Scale(u, d * c), ws), Load4(&F(0x40))));
    }
    // ---- gravity: on the ground a vertical impulse, in the air an acceleration
    if (grounded > 0) {
        b.accel[0] = b.accel[1] = b.accel[2] = b.accel[3] = 0;
        F(0x54) = T[0x8c] * b.mass * 0.0399999991f + F(0x54);
    } else {
        b.accel[0] = 0;
        b.accel[1] = T[0x8d];
        b.accel[2] = 0;
        b.accel[3] = 0;
    }
    B(0x671) = static_cast<uint8_t>(grounded);
    const V4 ang = Scale(Load4(&F(0x40)), T[0x1a]);

    // ---- 0x524f00 on land: the corners' water depths are cleared and their
    // buoyancy factors S+0x628 grow back towards 0.5.
    {
        int present = 0;
        for (int i = 0; i < 6; ++i) present += slots[i] != 0;
        const int corners = present == 2 ? 2 : 4;
        U(0x658) = 0;   // wheels in water
        F(0x648) = 0;   // the deepest corner
        for (int i = 0; i < corners; ++i) {
            F(0x638 + 4 * i) = 0;
            if (b.matrix[1][1] > 0) {
                F(0x628 + 4 * i) = F(0x628 + 4 * i) * 1.04999995f;
                if (F(0x628 + 4 * i) > 0.5f) F(0x628 + 4 * i) = 0.5f;
            }
        }
        (void)rayFlag;  // (0x526290 also casts down for water when it is set: not ported)
    }
    const float lin[4] = { F(0x50), F(0x54), F(0x58), 0 };
    const float angv[4] = { ang.x, ang.y, ang.z, 0 };
    b.ApplyImpulse(lin, angv);
    g8afa14 = F(0x570);
}

// ---------------------------------------------------------------------------
// The mapper's fixed-point storage of a control (0x4d9360 / 0x4d93f0): the
// value times 2^bits - 1 (signed: half that), rounded away from zero (ceil
// 0x5e3bc0 / floor 0x5e3230), read back divided by the same scale.
static float SSE_RET Quantize(float v, int bits, bool isSigned)
{
    const int full = (1 << bits) - 1, scale = isSigned ? full >> 1 : full;
    const float p = static_cast<float>(scale) * v;
    const double r = (isSigned && !(p >= 0)) ? floor(static_cast<double>(p)) : ceil(static_cast<double>(p));
    return static_cast<float>(static_cast<int>(r)) / static_cast<float>(scale);
}

void CarDriver::Update(const CarDriverKeys& keys, int vehicleId, float wheelSpeed, float c[5], bool& handbrake)
{
    // The steering source (0x514aa0): target from the key pair, rate-limited.
    const float raw = (keys.right ? 1.0f : 0.0f) - (keys.left ? 1.0f : 0.0f);
    float x = 2.0f * raw;
    if (x > 1.0f) x = 1.0f;
    else if (x < -1.0f) x = -1.0f;
    const bool neg = x < 0;
    if (neg) x = -x;
    x = (x * 0.800000012f + 0.200000003f) * x;
    if (neg) x = -x;
    const float delta = x - steer;
    const float rate = ((delta > 0) == (steer > 0)) ? 0.0900000036f : 0.180000007f;
    if (delta > rate) steer = steer + rate;
    else if (-rate > delta) steer = steer - rate;
    else steer = x;

    // 0x50ce80.
    float s = Quantize(steer, 16, true);
    if (wheelSpeed < 10.0f) {
        const float f = wheelSpeed < 5.0f ? 1.0f : (10.0f - wheelSpeed) * 0.200000003f;
        s = (f * 0.454545468f + 1.0f) * s;
    }
    float drive = keys.reverse ? -Quantize(1.0f, 8, false) : (keys.accelerate ? Quantize(1.0f, 8, false) : 0.0f);
    if (drive > 1.0f) drive = 1.0f;
    else if (drive < -1.0f) drive = -1.0f;
    float brake = 0;
    if (vehicleId == 0x13 || vehicleId == 0x36 || !keys.handbrake) brake = keys.brake ? Quantize(1.0f, 3, false) : 0.0f;
    c[0] = drive;
    c[1] = s;
    c[2] = brake;
    c[3] = keys.control27 ? 1.0f : 0.0f;
    c[4] = 0;   // control 0x1f (an axis, read only with +0x24 set)
    handbrake = keys.handbrake;
}
