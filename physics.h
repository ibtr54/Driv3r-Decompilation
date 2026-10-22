// The game's rigid body (BOOT_TO_MENU.md section 62, driv3r-re\PHYSICS.md).
// Driv3r has its own physics (no middleware). A body is a 0x1e0-byte record;
// the fields below keep the game's offsets in their comments. The world
// steps every body once per 1/120 s tick (world +0xc = 1/120, sub-step
// context +0x1f4 = 1/120): 0x5a9880 integrates (0x5af710) and 0x5a9bf0
// rebuilds the matrix. Cars move only through impulses (0x5b11c0): force and
// torque are zero at every integration in the traces.
//
// The game's FPU runs in single precision (control word 0x007f under
// Direct3D 9), and the code stores almost every intermediate as a float, so
// the float statements here follow the game's operation order to give the
// same bits; tests\physreplay.cpp checks them against traced bodies.
#pragma once

struct RigidBody
{
    float matrix[4][4] = {};  // +0x10: rows x, y, z, position (row-vector, Direct3D)
    float pos[4] = {};        // +0x80
    float vel[4] = {};        // +0x90
    float q[4] = {};          // +0xa0: quaternion x, y, z, w
    float L[4] = {};          // +0xb0: angular momentum
    float force[4] = {};      // +0xc0
    float torque[4] = {};     // +0xd0
    float accel[4] = {};      // +0xe0: extra acceleration
    float R[3][4] = {};       // +0xf0: rotation from q (0x41e720)
    float invI[3][4] = {};    // +0x120: inverse world inertia R^T diag(1/I) R
    float w[4] = {};          // +0x150: angular velocity invI L
    float dq[4] = {};         // +0x160: 0.5 (w x q)
    float inertia[4] = {};    // +0x170: body-space inertia diagonal
    float sleep[6] = {};      // +0x1a0..+0x1b4: cleared when an impulse wakes the body
    float mass = 0;           // +0x1c0
    int state = 0x65;         // +0x1c4: 0x65 awake, 0x78 asleep (also 0x6e, 0x96, 0xa0)

    // 0x4b5170 + 0x5aeb20 + 0x5aee00 on the body: normalised q, R, invI, w, dq.
    void UpdateDerived();
    // 0x5aee00 alone: w and dq from L.
    void UpdateSpin();
    // 0x5af710: one sub-step of h (linear, then the rotation update chosen by
    // the spin: 0x5af9f0 first order, 0x5afb70 midpoint, 0x5afe70 four stages).
    void Integrate(float h);
    // 0x5a9bf0: the matrix from q and pos.
    void UpdateMatrix();
    // 0x5b11c0 (a free body, [+0] != 1 and no parent +0x1d8): v += J / m,
    // L += angular, then w and dq; a sleeping body (0x78) wakes (0x65).
    void ApplyImpulse(const float* linear, const float* angular);
};

// Helpers with the game's operation order.
// 0x41e720: rotation rows (stride 4 floats, column 3 zero) from a quaternion.
void QuatToRows(const float q[4], float out[3][4]);
// 0x41e510: out[j] = v[2] m[2][j] + (v[1] m[1][j] + v[0] m[0][j]), j = 0..2.
void VecMat3(const float v[3], const float m[3][4], float out[3]);
// 0x5209b0: the quaternion product a b.
void QuatMul(const float a[4], const float b[4], float out[4]);
// 0x4b5170: q / |q|, or (0, 0, 0, 1) when |q|^2 < 1e-5.
void QuatNormalize(const float in[4], float out[4]);
