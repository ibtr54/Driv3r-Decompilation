// The game's rigid body step (BOOT_TO_MENU.md section 62). Every function
// follows one game routine instruction by instruction (lifted with
// driv3r-re\tools\physics\lift.py); the comments give the address.
#include "physics.h"

#include <cmath>
#include <cstring>

// 0x41e720 (after 0x41e7f0 sets the identity): t = 2 q.
void QuatToRows(const float q[4], float out[3][4])
{
    const float x2 = q[0] + q[0];
    const float xx = x2 * q[0], xy = x2 * q[1], xz = x2 * q[2];
    const float y2 = q[1] + q[1];
    const float yy = y2 * q[1], yz = y2 * q[2];
    const float z2 = q[2] + q[2];
    const float zz = z2 * q[2];
    const float w2 = q[3] + q[3];
    const float wx = w2 * q[0], wy = w2 * q[1], wz = w2 * q[2];
    out[0][0] = (1.0f - yy) - zz;
    out[1][0] = xy - wz;
    out[2][0] = wy + xz;
    out[0][1] = wz + xy;
    const float oneXx = 1.0f - xx;
    out[1][1] = oneXx - zz;
    out[2][1] = yz - wx;
    out[0][2] = xz - wy;
    out[1][2] = wx + yz;
    out[2][2] = oneXx - yy;
    out[0][3] = out[1][3] = out[2][3] = 0;
}

// 0x41e510.
void VecMat3(const float v[3], const float m[3][4], float out[3])
{
    const float z0 = v[2] * m[2][0], z1 = v[2] * m[2][1], z2 = v[2] * m[2][2];
    const float y0 = v[1] * m[1][0], y1 = v[1] * m[1][1], y2 = v[1] * m[1][2];
    const float x0 = v[0] * m[0][0], x1 = v[0] * m[0][1], x2 = v[0] * m[0][2];
    out[0] = z0 + (y0 + x0);
    out[1] = z1 + (y1 + x1);
    out[2] = z2 + (y2 + x2);
}

// 0x5209b0: the product with eight multiplications.
void QuatMul(const float a[4], const float b[4], float out[4])
{
    const float t3 = (a[0] + a[1]) * (b[2] + b[0]);
    const float t6 = (a[3] - a[2]) * (b[3] + b[1]);
    const float t9 = (b[3] - b[1]) * (a[3] + a[2]);
    const float t11 = (t9 + t6) + t3;
    const float t14 = (a[0] - a[1]) * (b[2] - b[0]);
    const float t16 = (t14 + t11) * 0.5f;
    out[0] = ((a[3] + a[0]) * (b[3] + b[0]) + t16) - t11;
    out[1] = ((b[3] - b[0]) * (a[2] + a[1]) + t16) - t9;
    out[2] = ((b[2] + b[1]) * (a[3] - a[0]) + t16) - t6;
    out[3] = ((b[2] - b[1]) * (a[1] - a[2]) + t16) - t3;
}

// 0x4b5170: the sum of squares stays on the FPU stack (rounded to single
// precision at each step by the control word).
void QuatNormalize(const float in[4], float out[4])
{
    const float s = ((in[0] * in[0] + in[1] * in[1]) + in[2] * in[2]) + in[3] * in[3];
    if (s < 1e-5f) {
        out[0] = out[1] = out[2] = 0;
        out[3] = 1;
        return;
    }
    const float r = 1.0f / sqrtf(s);
    const float x = r * in[0], y = r * in[1], z = r * in[2], w = r * in[3];
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}

namespace {

// 0x5aeb20: R from q, then invI = R^T diag(1/I) R: row i of invI is row i
// of R^T times the matrix whose rows are R's rows over the inertia.
void UpdateInertia(const float q[4], const float inertia[4], float R[3][4], float invI[3][4])
{
    QuatToRows(q, R);
    float scaled[3][4] = {};
    for (int i = 0; i < 3; ++i) {
        const float inv = 1.0f / inertia[i];
        scaled[i][0] = inv * R[i][0];
        scaled[i][1] = inv * R[i][1];
        scaled[i][2] = inv * R[i][2];
    }
    for (int i = 0; i < 3; ++i) {
        const float col[3] = { R[0][i], R[1][i], R[2][i] };
        VecMat3(col, scaled, invI[i]);
        invI[i][3] = 0;
    }
}

// 0x5aee00: w = L invI, dq = 0.5 (q w) (0x5209b0 with a = q, b = (w, 0)).
void UpdateSpin(const float q[4], const float L[4], const float invI[3][4], float w[4], float dq[4])
{
    VecMat3(L, invI, w);
    w[3] = 0;
    const float wq[4] = { w[0], w[1], w[2], 0 };
    float p[4];
    QuatMul(q, wq, p);
    dq[0] = p[0] * 0.5f;
    dq[1] = p[1] * 0.5f;
    dq[2] = p[2] * 0.5f;
    dq[3] = p[3] * 0.5f;
}

// 0x5af0e0: the derivative at a stage state (q normalised, the inverse
// inertia and spin of that state), for the midpoint and Cash-Karp updates.
void StageDerivative(const float q[4], const float L[3], const float inertia[4], float out[4])
{
    float qn[4], R[3][4], invI[3][4], w[4];
    QuatNormalize(q, qn);
    UpdateInertia(qn, inertia, R, invI);
    const float l4[4] = { L[0], L[1], L[2], 0 };
    UpdateSpin(qn, l4, invI, w, out);
}

}  // namespace

void RigidBody::UpdateDerived()
{
    QuatNormalize(q, q);
    UpdateInertia(q, inertia, R, invI);
    ::UpdateSpin(q, L, invI, w, dq);
}

void RigidBody::UpdateSpin()
{
    ::UpdateSpin(q, L, invI, w, dq);
}

void RigidBody::Integrate(float h)
{
    // 0x5af710: linear part.
    const float inv = 1.0f / mass;
    const float a0 = inv * force[0] + accel[0];
    const float a1 = inv * force[1] + accel[1];
    const float a2 = inv * force[2] + accel[2];
    const float hh = h * 0.5f;
    pos[0] = (a0 * hh + vel[0]) * h + pos[0];
    pos[1] = (a1 * hh + vel[1]) * h + pos[1];
    pos[2] = (a2 * hh + vel[2]) * h + pos[2];
    pos[3] = 0;
    vel[0] = a0 * h + vel[0];
    vel[1] = a1 * h + vel[1];
    vel[2] = a2 * h + vel[2];
    vel[3] = 0;

    // The rotation update by the spin: q = 0.0004 |w|^2, t = 1.6667e-5 / q.
    const float spin = ((w[2] * w[2] + w[1] * w[1]) + w[0] * w[0]) * 0.0004f;
    const float t = 1.66666669e-05f / spin;
    const float h2 = h * h;
    if (h2 < t) {
        // 0x5af9f0: first order.
        for (int i = 0; i < 4; ++i) q[i] = h * dq[i] + q[i];
        for (int i = 0; i < 3; ++i) L[i] = h * torque[i] + L[i];
    } else if (t / sqrtf(spin) > h2 * h) {
        // 0x5afb70: midpoint.
        const float hm = h * 0.5f;
        float qs[4], ls[3], k[4];
        for (int i = 0; i < 4; ++i) qs[i] = hm * dq[i] + q[i];
        for (int i = 0; i < 3; ++i) ls[i] = hm * torque[i] + L[i];
        StageDerivative(qs, ls, inertia, k);
        for (int i = 0; i < 4; ++i) q[i] = k[i] * h + q[i];
        for (int i = 0; i < 3; ++i) L[i] = h * torque[i] + L[i];
    } else {
        // 0x5afe70: Cash-Karp, fifth order (weights on k1, k3, k4, k6).
        float k2[4], k3[4], k4[4], k5[4], k6[4], qs[4], ls[3];
        const float c21 = h * 0.2f;
        for (int i = 0; i < 4; ++i) qs[i] = c21 * dq[i] + q[i];
        for (int i = 0; i < 3; ++i) ls[i] = c21 * torque[i] + L[i];
        StageDerivative(qs, ls, inertia, k2);

        const float c31 = h * 0.075f, c32 = h * 0.225f;
        for (int i = 0; i < 4; ++i) qs[i] = (c31 * dq[i] + q[i]) + k2[i] * c32;
        const float s3 = c31 + c32;
        for (int i = 0; i < 3; ++i) ls[i] = s3 * torque[i] + L[i];
        StageDerivative(qs, ls, inertia, k3);

        const float c41 = h * 0.3f, c42 = h * -0.9f, c43 = h * 1.2f;
        for (int i = 0; i < 4; ++i) qs[i] = ((c41 * dq[i] + q[i]) + k2[i] * c42) + k3[i] * c43;
        const float s4 = c41 + (c43 + c42);
        for (int i = 0; i < 3; ++i) ls[i] = s4 * torque[i] + L[i];
        StageDerivative(qs, ls, inertia, k4);

        const float c51 = h * -0.203703701f, c52 = h * 2.5f, c53 = h * -2.59259248f, c54 = h * 1.29629624f;
        for (int i = 0; i < 4; ++i) qs[i] = (((c51 * dq[i] + q[i]) + k2[i] * c52) + k3[i] * c53) + k4[i] * c54;
        const float s5 = c51 + ((c54 + c53) + c52);
        for (int i = 0; i < 3; ++i) ls[i] = s5 * torque[i] + L[i];
        StageDerivative(qs, ls, inertia, k5);

        const float c61 = h * 0.0294958036f, c62 = h * 0.341796875f, c63 = h * 0.0415943302f,
                    c64 = h * 0.400345415f, c65 = h * 0.0617675781f;
        for (int i = 0; i < 4; ++i)
            qs[i] = ((((c61 * dq[i] + q[i]) + k2[i] * c62) + k3[i] * c63) + k4[i] * c64) + k5[i] * c65;
        const float s6 = ((((c65 + c64) + c63) + c62) + c61);
        for (int i = 0; i < 3; ++i) ls[i] = s6 * torque[i] + L[i];
        StageDerivative(qs, ls, inertia, k6);

        const float w1 = h * 0.097883597f, w3 = h * 0.402576476f, w4 = h * 0.210437715f, w6 = h * 0.289102197f;
        for (int i = 0; i < 4; ++i) q[i] = w1 * dq[i] + q[i];
        for (int i = 0; i < 4; ++i) q[i] = k3[i] * w3 + q[i];
        for (int i = 0; i < 4; ++i) q[i] = k4[i] * w4 + q[i];
        for (int i = 0; i < 4; ++i) q[i] = k6[i] * w6 + q[i];
        const float sw = ((w6 + w4) + w3) + w1;
        for (int i = 0; i < 3; ++i) L[i] = sw * torque[i] + L[i];
    }
    L[3] = 0;
    UpdateDerived();
}

void RigidBody::UpdateMatrix()
{
    // 0x5a9bf0: the same terms as 0x41e720, into the world matrix's rows.
    float r[3][4];
    QuatToRows(q, r);
    for (int i = 0; i < 3; ++i) {
        matrix[i][0] = r[i][0];
        matrix[i][1] = r[i][1];
        matrix[i][2] = r[i][2];
        matrix[i][3] = 0;
    }
    matrix[3][0] = pos[0];
    matrix[3][1] = pos[1];
    matrix[3][2] = pos[2];
    matrix[3][3] = 1;
}

void RigidBody::ApplyImpulse(const float* linear, const float* angular)
{
    if (linear) {
        const float inv = 1.0f / mass;
        vel[0] = inv * linear[0] + vel[0];
        vel[1] = inv * linear[1] + vel[1];
        vel[2] = inv * linear[2] + vel[2];
        vel[3] = 0;
        if (state == 0x78) {
            state = 0x65;
            memset(sleep, 0, sizeof(sleep));
        }
    }
    if (angular) {
        L[0] = L[0] + angular[0];
        L[1] = L[1] + angular[1];
        L[2] = L[2] + angular[2];
        L[3] = 0;
        ::UpdateSpin(q, L, invI, w, dq);
        if (state == 0x78) {
            state = 0x65;
            memset(sleep, 0, sizeof(sleep));
        }
    }
}
