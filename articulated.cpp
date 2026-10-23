// The articulated groups (articulated.h). Every function follows one game
// routine in its operation order (worked out with
// driv3r-re\tools\physics\symx87.py and checked against the game's own code
// by tests\groupreplay.cpp); the comments give the address.
#include "articulated.h"

#include <cmath>
#include <cstring>

namespace Articulated
{
namespace
{
float& E(SMat& m, int k, int i)
{
    if (i < 3) return k < 3 ? m.A[k][i] : m.B[k - 3][i];
    return k < 3 ? m.C[k][i - 3] : m.D[k - 3][i - 3];
}
// a spatial vector's component i (the padding skipped)
float& P(SVec& v, int i) { return i < 3 ? v.t[i] : v.b[i - 3]; }

// 0x41e510 on a row of a block.
void RowTimes(const float v[4], const float m[3][4], float out[4])
{
    VecMat3(v, m, out);
    out[3] = 0;
}
}  // namespace

// 0x5bdef0: out_i = (m_i1 v1 + m_i2 v2) + m_i0 v0 (out[3] is stale stack in
// the game; every caller drops it).
void M3V(const float m[3][4], const float v[4], float out[4])
{
    for (int i = 0; i < 3; ++i) out[i] = (m[i][1] * v[1] + m[i][2] * v[2]) + m[i][0] * v[0];
    out[3] = 0;
}

// 0x5bf350: out_j = (m_1j v1 + m_2j v2) + m_0j v0.
void VecRot(const float m[3][4], const float v[4], float out[4])
{
    float r[3];
    for (int j = 0; j < 3; ++j) r[j] = (m[1][j] * v[1] + m[2][j] * v[2]) + m[0][j] * v[0];
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = 0;
}

// 0x5bdff0.
void Add(const SVec& a, const SVec& b, SVec& out)
{
    for (int i = 0; i < 3; ++i) { out.t[i] = a.t[i] + b.t[i]; out.b[i] = a.b[i] + b.b[i]; }
    out.t[3] = out.b[3] = 0;
}

// 0x5be0b0.
void Scale(const SVec& v, float s, SVec& out)
{
    for (int i = 0; i < 3; ++i) { out.t[i] = s * v.t[i]; out.b[i] = s * v.b[i]; }
    out.t[3] = out.b[3] = 0;
}

// 0x5be2b0: the row vector times the matrix, top = vt A + vb B, bottom =
// vt C + vb D (each part through 0x41e510).
void VecTimesMat(const SVec& v, const SMat& m, SVec& out)
{
    float a[4], b[4], c[4], d[4];
    RowTimes(v.t, m.A, a);
    RowTimes(v.b, m.B, b);
    RowTimes(v.t, m.C, c);
    RowTimes(v.b, m.D, d);
    for (int i = 0; i < 3; ++i) { out.t[i] = b[i] + a[i]; out.b[i] = d[i] + c[i]; }
    out.t[3] = out.b[3] = 0;
}

// 0x5be3f0: top = D vt + B vb, bottom = C vt + A vb (each block through
// 0x5bdef0).
void MatTimesVec(const SMat& m, const SVec& v, SVec& out)
{
    float a[4], b[4], c[4], d[4];
    M3V(m.A, v.b, a);
    M3V(m.C, v.t, c);
    M3V(m.B, v.b, b);
    M3V(m.D, v.t, d);
    for (int i = 0; i < 3; ++i) { out.b[i] = c[i] + a[i]; out.t[i] = d[i] + b[i]; }
    out.t[3] = out.b[3] = 0;
}

// 0x5be530 (0x41e4b0 per row).
void Add(const SMat& a, const SMat& b, SMat& out)
{
    const float* x = &a.A[0][0];
    const float* y = &b.A[0][0];
    float* o = &out.A[0][0];
    for (int r = 0; r < 12; ++r) {
        for (int k = 0; k < 3; ++k) o[r * 4 + k] = x[r * 4 + k] + y[r * 4 + k];
        o[r * 4 + 3] = 0;
    }
}

// 0x5c6170: a - b.
void Sub(const SMat& a, const SMat& b, SMat& out)
{
    const float* x = &a.A[0][0];
    const float* y = &b.A[0][0];
    float* o = &out.A[0][0];
    for (int r = 0; r < 12; ++r) {
        for (int k = 0; k < 3; ++k) o[r * 4 + k] = x[r * 4 + k] - y[r * 4 + k];
        o[r * 4 + 3] = 0;
    }
}

// 0x5c5f40: both parts' squared lengths below 1e-10.
bool IsZero(const SVec& v)
{
    if (!((v.t[0] * v.t[0] + v.t[1] * v.t[1]) + v.t[2] * v.t[2] < 9.99999944e-11f)) return false;
    return (v.b[0] * v.b[0] + v.b[1] * v.b[1]) + v.b[2] * v.b[2] < 9.99999944e-11f;
}

// 0x5be7c0 (0x41ea10 per row).
void Scale(float s, const SMat& m, SMat& out)
{
    const float* x = &m.A[0][0];
    float* o = &out.A[0][0];
    for (int r = 0; r < 12; ++r) {
        for (int k = 0; k < 3; ++k) o[r * 4 + k] = s * x[r * 4 + k];
        o[r * 4 + 3] = 0;
    }
}

// 0x5bea30: element (k, i) = b_i c'_k, c' = (c bottom, c top).
void Outer(const SVec& b, const SVec& c, SMat& out)
{
    SVec bb = b, cc = c;
    for (int i = 0; i < 6; ++i)
        for (int k = 0; k < 6; ++k) E(out, k, i) = P(bb, i) * P(cc, k < 3 ? k + 3 : k - 3);
}

// 0x5bead0: m x = rhs by Gaussian elimination with the pivot searched along
// k's elements (k, i >= k); m and rhs are overwritten. Returns false (the
// game's 1) when the pivot is below 1e-5.
bool Solve(SVec& x, SMat& m, SVec& rhs)
{
    const float eps = 9.99999975e-06f;  // 0x78e9b0
    for (int k = 0; k < 6; ++k) {
        float best = 0.0f;
        int p = 0;
        for (int i = k; i < 6; ++i) {
            float v = E(m, k, i);
            if (!(v > 0.0f)) v = -v;
            if (v > best) { best = v; p = i; }
        }
        if (best < eps) return false;
        if (p != k) {
            const float inv = 1.0f / E(m, k, p);
            E(m, k, p) = E(m, k, k);
            const float bp = P(rhs, p) * inv;
            P(rhs, p) = P(rhs, k);
            P(rhs, k) = bp;
            for (int j = k + 1; j < 6; ++j) {
                const float t = E(m, j, p) * inv;
                E(m, j, p) = E(m, j, k);
                E(m, j, k) = t;
            }
        } else {
            const float inv = 1.0f / E(m, k, k);
            for (int j = k + 1; j < 6; ++j) E(m, j, k) = inv * E(m, j, k);
            P(rhs, k) = inv * P(rhs, k);
        }
        for (int i = k + 1; i < 6; ++i) {
            P(rhs, i) = P(rhs, i) - E(m, k, i) * P(rhs, k);
            const float f = E(m, k, i);
            for (int j = k + 1; j < 6; ++j) E(m, j, i) = E(m, j, i) - E(m, j, k) * f;
        }
    }
    for (int i = 5; i >= 0; --i) {
        float s = P(rhs, i);
        for (int j = i + 1; j < 6; ++j) s = s - E(m, j, i) * P(x, j);
        P(x, i) = s;
    }
    return true;
}

// 0x5bef80: the spatial vector into the frame r (rows 0..2 the rotation,
// row 3 the offset p): top = vt r, bottom = (top x p) + vb r.
void Transform(const float r[4][4], const SVec& v, SVec& out)
{
    float t[3], b[3];
    for (int j = 0; j < 3; ++j) t[j] = (r[2][j] * v.t[2] + r[0][j] * v.t[0]) + r[1][j] * v.t[1];
    for (int j = 0; j < 3; ++j) b[j] = (r[0][j] * v.b[0] + r[1][j] * v.b[1]) + r[2][j] * v.b[2];
    const float* p = r[3];
    out.b[0] = (t[2] * p[1] - t[1] * p[2]) + b[0];
    out.b[1] = (t[0] * p[2] - t[2] * p[0]) + b[1];
    out.b[2] = (t[1] * p[0] - t[0] * p[1]) + b[2];
    out.t[0] = t[0]; out.t[1] = t[1]; out.t[2] = t[2];
    out.t[3] = out.b[3] = 0;
}

// 0x5bf640: the spatial inertia into the frame r. With R the rotation, S
// the skew matrix of the offset p (rows (0, pz, -py), (-pz, 0, px),
// (py, -px, 0)) and X' = R^T X R for each block (0x41e510 rows):
// A = A' - S B', B = B', C = C' + (G + G^T) - (S B') S with G = A' S, and
// D = A^T (the input's D is not read).
void Transform(const float r[4][4], const SMat& m, SMat& out)
{
    float R[3][4], Rt[3][4];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) { R[i][j] = r[i][j]; Rt[i][j] = r[j][i]; }
        R[i][3] = Rt[i][3] = 0;
    }
    auto rotate = [&](const float x[3][4], float o[3][4]) {
        float w[3][4];
        for (int i = 0; i < 3; ++i) RowTimes(x[i], R, w[i]);
        for (int i = 0; i < 3; ++i) RowTimes(Rt[i], w, o[i]);
    };
    float A[3][4], B[3][4], C[3][4];
    rotate(m.A, A);
    rotate(m.B, B);
    rotate(m.C, C);
    const float* p = r[3];
    const float S[3][4] = { { 0.0f, p[2], -p[1], 0 }, { -p[2], 0.0f, p[0], 0 }, { p[1], -p[0], 0.0f, 0 } };
    float SB[3][4], G[3][4], H[3][4];
    for (int i = 0; i < 3; ++i) RowTimes(S[i], B, SB[i]);
    for (int i = 0; i < 3; ++i) RowTimes(A[i], S, G[i]);
    for (int i = 0; i < 3; ++i) RowTimes(SB[i], S, H[i]);
    SMat o;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            o.A[i][j] = A[i][j] - SB[i][j];
            o.B[i][j] = B[i][j];
            o.C[i][j] = C[i][j] + ((G[i][j] + G[j][i]) - H[i][j]);
        }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o.D[i][j] = o.A[j][i];
    out = o;
}

// 0x58c1f0: v rotated by q, q (v, 0) q* through two 0x5209b0 products.
void QuatRotate(const float q[4], const float v[4], float out[4])
{
    const float vq[4] = { v[0], v[1], v[2], 0.0f };
    const float qc[4] = { -q[0], -q[1], -q[2], q[3] };
    float t[4], r[4];
    QuatMul(qc, vq, t);
    QuatMul(t, q, r);
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = q[3];
}

// 0x66fc51: rows 0 and 3 add their four products in pairs, rows 1 and 2
// one after the other.
void MatMul(const float a[16], const float b[16], float out[16])
{
    float o[16];
    for (int i = 0; i < 4; ++i) {
        const float* r = a + 4 * i;
        for (int j = 0; j < 4; ++j) {
            const float p0 = r[0] * b[j], p1 = r[1] * b[4 + j], p2 = r[2] * b[8 + j], p3 = r[3] * b[12 + j];
            o[4 * i + j] = (i == 0 || i == 3) ? (p0 + p1) + (p2 + p3) : ((p0 + p1) + p2) + p3;
        }
    }
    memcpy(out, o, sizeof(o));
}

// 0x5aa430: the rotation transposed (row 3 holds m's column 3 meanwhile),
// the offset -(p, 1) out through 0x66cad5: (x r0 + y r1) + (z r2 + w r3).
void RigidInverse(const float m[16], float out[16])
{
    float o[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) o[4 * i + j] = m[4 * j + i];
    o[3] = o[7] = o[11] = 0;
    const float v[4] = { m[12], m[13], m[14], 1.0f };
    float t[3];
    for (int j = 0; j < 3; ++j) t[j] = (v[0] * o[j] + v[1] * o[4 + j]) + (v[2] * o[8 + j] + v[3] * o[12 + j]);
    o[12] = t[0] * -1.0f;
    o[13] = t[1] * -1.0f;
    o[14] = t[2] * -1.0f;
    o[15] = 1.0f;
    memcpy(out, o, sizeof(o));
}

namespace
{
const float kPi = 3.14159274f;          // 0x71c144
const float kHalfPi = 1.57079637f;      // 0x78e9ec
const float kTwoPi = 6.28318548f;       // 0x78e8f4
const float kInvTwoPi = 0.159154937f;   // 0x78eb7c

// beyond pi: x - floor((x + pi) / 2pi) 2pi (the CRT's floor)
float Reduce(float x)
{
    const float f = static_cast<float>(std::floor(static_cast<double>((x + kPi) * kInvTwoPi)));
    return x - f * kTwoPi;
}
}  // namespace

// 0x401070: 1 + x^2/2 (-1 + x^2/12 (1 - ...)), reflected about +-pi/2.
float Cos(float x)
{
    float sign = 1.0f;
    float x2 = x * x;
    if (x2 > 9.86960506f) {  // 0x78eb80, pi^2
        x = Reduce(x);
        x2 = x * x;
    }
    if (x > kHalfPi) {
        const float y = kPi - x;
        sign = -1.0f;
        x2 = y * y;
    } else if (x < -kHalfPi) {
        const float y = x + kPi;
        sign = -1.0f;
        x2 = y * y;
    }
    float p = 0.00757575780f * x2 - 1.0f;
    p = (p * x2) * 0.0111111114f + 1.0f;
    p = (p * x2) * 0.0178571437f - 1.0f;
    p = (p * x2) * 0.0333333351f + 1.0f;
    p = (p * x2) * 0.0833333358f - 1.0f;
    return ((x2 * p) * 0.5f + 1.0f) * sign;
}

// 0x401160: x (1 - x^2/6 (1 - ...)), reflected about +-pi/2.
float Sin(float x)
{
    float sign = 1.0f;
    float x2 = x * x;
    if (x2 > 9.86960506f) {
        x = Reduce(x);
        x2 = x * x;
    }
    if (x > kHalfPi) {
        x = kPi - x;
        x2 = x * x;
    } else if (x < -kHalfPi) {
        sign = -1.0f;
        x = x + kPi;
        x2 = x * x;
    }
    float p = 0.00641025649f * x2 - 1.0f;
    p = (p * x2) * 0.00909090880f + 1.0f;
    p = (p * x2) * 0.0138888890f - 1.0f;
    p = (p * x2) * 0.0238095243f + 1.0f;
    p = (p * x2) * 0.0500000007f - 1.0f;
    p = (p * x2) * 0.166666672f + 1.0f;
    return x * (p * sign);
}

namespace
{
float& F(float* rec, int o) { return rec[o / 4]; }
int Int(const float* rec, int o) { return *reinterpret_cast<const int*>(rec + o / 4); }
SVec& V(float* rec, int o) { return *reinterpret_cast<SVec*>(rec + o / 4); }
SMat& Mat(float* rec, int o) { return *reinterpret_cast<SMat*>(rec + o / 4); }
const float (*Rows(const float* p))[4] { return reinterpret_cast<const float(*)[4]>(p); }

// The body's spatial inertia (0x5c1390, 0x5c7740): B the mass times the
// identity (0x41ea10 rows), C the inertia diagonal, A and D zero.
void RigidInertia(const RigidBody& b, SMat& out)
{
    out = SMat();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out.B[i][j] = b.mass * (i == j ? 1.0f : 0.0f);
        out.C[i][i] = b.inertia[i];
    }
}

// 0x407640 (a x b), w 0.
void Cross(const float a[4], const float b[4], float out[4])
{
    const float z = a[1] * b[0] - a[0] * b[1];
    const float y = a[0] * b[2] - a[2] * b[0];
    const float x = a[2] * b[1] - a[1] * b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = 0;
}
}  // namespace

// The quaternion of the rotation rows r (0x5c06d0, 0x5abd60): identity
// unless the rows are orthonormal (row 0 x row 1 = row 2 within 1e-4, unit
// columns within 1e-5), else from the trace, or the largest diagonal
// element when the trace is negative.
void QuatFromRows(const float r[][4], float quat[4], double crossEps, double colEps)
{
    quat[0] = quat[1] = quat[2] = 0.0f;
    quat[3] = 1.0f;
    float c[4];
    Cross(r[1], r[0], c);  // row 0 x row 1
    const float d0 = c[0] - r[2][0], d1 = c[1] - r[2][1], d2 = c[2] - r[2][2];
    bool ortho = (d0 * d0 + d2 * d2) + d1 * d1 < crossEps;  // 0x78eb58 (double)
    if (ortho) {
        for (int j = 0; j < 3 && ortho; ++j) {
            float e = (r[2][j] * r[2][j] + (r[1][j] * r[1][j] + r[0][j] * r[0][j])) - 1.0f;
            if (!(e > 0.0f)) e = -e;
            ortho = e < colEps;  // 0x78eb50 (double)
        }
    }
    if (ortho) {
        const float trace = (r[2][2] + r[1][1]) + r[0][0];
        if (!(trace < 0.0f)) {
            const float s = sqrtf(trace + 1.0f);
            const float h = 0.5f / s;
            quat[0] = (r[1][2] - r[2][1]) * h;
            quat[1] = (r[2][0] - r[0][2]) * h;
            quat[2] = h * (r[0][1] - r[1][0]);
            quat[3] = s * 0.5f;
        } else {
            int i = r[1][1] > r[0][0] ? 1 : 0;
            if (r[2][2] > r[i][i]) i = 2;
            const int j = (i + 1) % 3, l = (j + 1) % 3;
            const float s = sqrtf((r[i][i] - (r[j][j] + r[l][l])) + 1.0f);
            const float h = 0.5f / s;
            quat[i] = 0.5f * s;
            quat[j] = (r[i][j] + r[j][i]) * h;
            quat[l] = (r[i][l] + r[l][i]) * h;
            quat[3] = h * (r[j][l] - r[l][j]);
        }
    }
}

// The parent's spatial velocity (body +0x150 angular, +0x90 linear) moved
// into the member's joint frame rec +0xc0 (0x5bef80); a root parent's
// velocities are world ones, turned into its frame first (0x5bf350).
void Group::ParentVelocity(int k, SVec& out)
{
    const Member& m = members[k];
    if (m.parent < 0) { out = SVec(); return; }
    const Member& p = members[m.parent];
    SVec pv;
    if (!Int(p.rec, 0x330)) {
        VecRot(Rows(p.rec + 0xc0 / 4), p.body->vel, pv.b);
        VecRot(Rows(p.rec + 0xc0 / 4), p.body->w, pv.t);
    } else {
        memcpy(pv.t, p.body->w, 12);
        memcpy(pv.b, p.body->vel, 12);
    }
    Transform(Rows(m.rec + 0xc0 / 4), pv, out);
}

// 0x5c1040: the member's velocity (body +0x150 / +0x90, in its joint
// frame): the parent's plus the joint axis times the rate.
void Group::Velocities(int k)
{
    Member& m = members[k];
    const int type = Int(m.rec, 0x330);
    if (!type) return;
    SVec x;
    ParentVelocity(k, x);
    const float rate = qd[k];
    float a[3] = { 0.0f, 0.0f, 0.0f }, l[3];
    for (int i = 0; i < 3; ++i) {
        if (type == 1) {
            a[i] = rate * F(m.rec, 0xa0 + 4 * i);
            l[i] = rate * F(m.rec, 0x140 + 4 * i);
        } else {
            l[i] = rate * F(m.rec, 0xa0 + 4 * i);
        }
    }
    float* w = m.body->w;
    float* v = m.body->vel;
    for (int i = 0; i < 3; ++i) { w[i] = x.t[i] + a[i]; v[i] = x.b[i] + l[i]; }
    w[3] = v[3] = 0;
}

// 0x5c1390: the member's bias force rec +0x220 (in its frame: gravity and
// the applied force turned by q*, the torque by q, plus w x Iw), its
// spatial inertia rec +0x160 (mass and the inertia diagonal) and, for a
// jointed member, the joint's velocity product rec +0x240.
void Group::Bias(int k)
{
    Member& m = members[k];
    RigidBody* b = m.body;
    float* rec = m.rec;
    const float nm = -b->mass;
    const float f[4] = { nm * b->accel[0] - b->force[0], nm * b->accel[1] - b->force[1],
                         nm * b->accel[2] - b->force[2], 0.0f };
    const float qc[4] = { -b->q[0], -b->q[1], -b->q[2], b->q[3] };
    float zt[4], tq[4], w[4], iw[4], c[4];
    QuatRotate(qc, f, zt);
    const float I[3][4] = { { b->inertia[0], 0, 0, 0 }, { 0, b->inertia[1], 0, 0 }, { 0, 0, b->inertia[2], 0 } };
    if (!Int(rec, 0x330)) QuatRotate(qc, b->w, w);
    else memcpy(w, b->w, 12);
    VecMat3(w, I, iw);
    Cross(iw, w, c);
    QuatRotate(b->q, b->torque, tq);
    SVec& z = V(rec, 0x220);
    for (int i = 0; i < 3; ++i) { z.t[i] = zt[i]; z.b[i] = tq[i] + c[i]; }
    z.t[3] = z.b[3] = 0;

    RigidInertia(*b, Mat(rec, 0x160));

    if (m.parent < 0) {
        if (Int(rec, 0x330) == 2) V(rec, 0x240) = SVec();
        return;
    }
    const int type = Int(rec, 0x330);
    // the parent's angular velocity in the joint frame
    const Member& p = members[m.parent];
    float wp[4], wj[4];
    if (!Int(p.rec, 0x330)) VecRot(Rows(p.rec + 0xc0 / 4), p.body->w, wp);
    else memcpy(wp, p.body->w, 16);
    VecRot(Rows(rec + 0xc0 / 4), wp, wj);
    const float rate = qd[k];
    float a[4], np[4], u[4], uw[4];
    for (int i = 0; i < 3; ++i) { a[i] = F(rec, 0xa0 + 4 * i) * rate; np[i] = F(rec, 0xf0 + 4 * i) * -1.0f; }
    a[3] = np[3] = 0;
    Cross(np, wj, u);
    Cross(u, wj, uw);
    SVec& cv = V(rec, 0x240);
    if (type == 1) {
        float lw[4];
        Cross(a, wj, cv.t);
        Cross(rec + 0x140 / 4, wj, lw);
        const float rr = rate * rate, r2 = rate + rate;
        for (int i = 0; i < 3; ++i) cv.b[i] = rr * F(rec, 0x150 + 4 * i) + (r2 * lw[i] + uw[i]);
    } else if (type == 2) {
        float aw[4];
        Cross(a, wj, aw);
        cv.t[0] = cv.t[1] = cv.t[2] = 0;
        for (int i = 0; i < 3; ++i) cv.b[i] = 2.0f * aw[i] + uw[i];
    }
    cv.t[3] = cv.b[3] = 0;
}

// 0x5c2100's backward loop: U = I^A S (rec +0x2a0), D^-1 (+0x36c), I^A c
// (+0x2c0), u = S . (I^A c + Z) (+0x370), the joint torque (+0x374): the
// spring and damper -(k q + c qd) (+0x350 / +0x354) and, with a stop gain
// (+0x364), a push against the parent's speed at the joint beyond 10.
void Group::Inward(int k)
{
    Member& m = members[k];
    float* rec = m.rec;
    SMat& ia = Mat(rec, 0x160);
    SVec& s = V(rec, 0x260);
    SVec& u = V(rec, 0x2a0);
    MatTimesVec(ia, s, u);
    const float d = ((u.b[2] * s.t[2] + u.b[1] * s.t[1]) + s.t[0] * u.b[0]) +
                    ((s.b[2] * u.t[2] + s.b[1] * u.t[1]) + s.b[0] * u.t[0]);
    F(rec, 0x36c) = 1.0f / d;
    VecTimesMat(V(rec, 0x240), ia, V(rec, 0x2c0));
    SVec t;
    Add(V(rec, 0x2c0), V(rec, 0x220), t);
    F(rec, 0x370) = ((t.t[2] * s.b[2] + t.t[1] * s.b[1]) + t.t[0] * s.b[0]) +
                    ((t.b[1] * s.t[1] + t.b[2] * s.t[2]) + t.b[0] * s.t[0]);
    float tau = qd[k] * F(rec, 0x354) + q[k] * F(rec, 0x350);
    if (F(rec, 0x364) > 0.0f) {
        SVec x;
        ParentVelocity(k, x);
        const float len = sqrtf((x.b[2] * x.b[2] + x.b[1] * x.b[1]) + x.b[0] * x.b[0]);
        if (len > 10.0f) {
            const float dot = (F(rec, 0x148) * x.b[2] + F(rec, 0x144) * x.b[1]) + F(rec, 0x140) * x.b[0];
            const float push = (dot * ((len - 10.0f) / len)) * F(rec, 0x364);
            float mag = push;
            if (!(mag > 0.0f)) mag = -mag;
            tau = mag * push + tau;
        }
    }
    F(rec, 0x374) = -tau;
    Propagate(k);
}

// 0x5c1da0: the member's articulated inertia and force handed to its
// parent: I^A_p += X^T (I^A - U D^-1 U'^T) X, Z_p += X^T (Z + I^A c + U'
// D^-1 (tau - u)), with U' = S I^A (0x5be2b0) and X the joint frame rec
// +0x100.
void Group::Propagate(int k)
{
    Member& m = members[k];
    if (m.parent < 0) return;
    float* rec = m.rec;
    float* prec = members[m.parent].rec;
    SVec su;
    SMat o, x;
    VecTimesMat(V(rec, 0x260), Mat(rec, 0x160), su);
    Outer(su, V(rec, 0x2a0), o);
    Scale(-F(rec, 0x36c), o, o);
    Add(o, Mat(rec, 0x160), o);
    Transform(Rows(rec + 0x100 / 4), o, x);
    Add(Mat(prec, 0x160), x, Mat(prec, 0x160));
    Scale(su, (F(rec, 0x374) - F(rec, 0x370)) * F(rec, 0x36c), su);
    Add(su, V(rec, 0x2c0), su);
    Add(su, V(rec, 0x220), su);
    SVec y;
    Transform(Rows(rec + 0x100 / 4), su, y);
    Add(V(prec, 0x220), y, V(prec, 0x220));
}

// 0x5c1ef0: the root's spatial acceleration (I^A a = -Z, 0x5bead0, which
// overwrites both), or the joint acceleration qdd = D^-1 (tau - U . a_p - u)
// (+0x378) and, for a member with children (+0x338), its own acceleration.
void Group::Accelerations(int k)
{
    Member& m = members[k];
    float* rec = m.rec;
    const int type = Int(rec, 0x330);
    SVec ap;
    if (m.parent < 0) {
        if (!type) {
            Scale(V(rec, 0x220), -1.0f, V(rec, 0x220));
            Solve(V(rec, 0x280), Mat(rec, 0x160), V(rec, 0x220));
            return;
        }
        F(rec, 0x378) = F(rec, 0x374);
        F(rec, 0x378) = F(rec, 0x378) - F(rec, 0x370);
        F(rec, 0x378) = F(rec, 0x36c) * F(rec, 0x378);
    } else {
        Transform(Rows(rec + 0xc0 / 4), V(members[m.parent].rec, 0x280), ap);
        const SVec& u = V(rec, 0x2a0);
        F(rec, 0x378) = F(rec, 0x374);
        const float dot = ((ap.t[1] * u.b[1] + ap.t[2] * u.b[2]) + ap.t[0] * u.b[0]) +
                          ((ap.b[1] * u.t[1] + ap.b[2] * u.t[2]) + ap.b[0] * u.t[0]);
        F(rec, 0x378) = F(rec, 0x378) - dot;
        F(rec, 0x378) = F(rec, 0x378) - F(rec, 0x370);
        F(rec, 0x378) = F(rec, 0x36c) * F(rec, 0x378);
    }
    if (Int(rec, 0x338) <= 0) return;
    SVec& a = V(rec, 0x280);
    Scale(V(rec, 0x260), F(rec, 0x378), a);
    Add(a, V(rec, 0x240), a);
    if (m.parent >= 0) Add(a, ap, a);
}

// 0x5ae2b0: the root's world matrix from q and pos (0x5a9f50) and its
// inverse; a joint's frame rec +0x40 x the joint's motion x rec +0x80 (a
// turn about z by -q, 0x5aa200, or a slide along z by -q, 0x5aa070) and its
// inverse.
void Group::Frames(int k)
{
    Member& m = members[k];
    float* rec = m.rec;
    const int type = Int(rec, 0x330);
    if (m.parent < 0 && !type) {
        float* w = rec + 0x100 / 4;
        float R[3][4];
        QuatToRows(m.body->q, R);
        memcpy(w, R, sizeof(R));
        w[12] = m.body->pos[0]; w[13] = m.body->pos[1]; w[14] = m.body->pos[2]; w[15] = 1.0f;
        RigidInverse(w, rec + 0xc0 / 4);
        return;
    }
    float j[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    if (type == 1) {
        const float a = -q[k];
        j[0] = Cos(a); j[1] = Sin(a);
        j[4] = -Sin(a); j[5] = Cos(a);
    } else if (type == 2) {
        j[14] = -q[k];
    } else {
        return;
    }
    float t[16];
    MatMul(rec + 0x40 / 4, j, t);
    MatMul(t, rec + 0x80 / 4, rec + 0xc0 / 4);
    RigidInverse(rec + 0xc0 / 4, rec + 0x100 / 4);
}

// 0x5c06d0: the root takes its matrix from rec +0x100 and its rotation
// rows from q; a member its matrix from rec +0x100 x the parent's (0x66fc51),
// the position from it, and q back from the rotation (identity unless the
// rows are orthonormal: the trace, or the largest diagonal element),
// normalised (0x4b5170).
void Group::BodyMatrix(int k)
{
    Member& m = members[k];
    RigidBody* b = m.body;
    float* rec = m.rec;
    if (!Int(rec, 0x330)) {
        QuatToRows(b->q, b->R);
        memcpy(b->matrix, rec + 0x100 / 4, 64);
        return;
    }
    if (m.parent >= 0) MatMul(rec + 0x100 / 4, &members[m.parent].body->matrix[0][0], &b->matrix[0][0]);
    else memcpy(b->matrix, rec + 0x100 / 4, 64);
    b->pos[0] = b->matrix[3][0]; b->pos[1] = b->matrix[3][1]; b->pos[2] = b->matrix[3][2]; b->pos[3] = 0;
    if (b->state == 0x78) {
        b->state = 0x65;
        for (float& s : b->sleep) s = 0;
    }
    float quat[4];
    QuatFromRows(b->matrix, quat);
    QuatNormalize(quat, b->q);
    QuatToRows(b->q, b->R);
}

// The joint limits rec +0x358 / +0x35c after the step: a joint past one and
// still moving out bounces (qd = -e qd, e +0x360, nudged back by 0.001)
// when slower than 0.05, else takes the impulse -(1 + e) qd through the
// group (0x5c7280, 0x5c81b0).
void Group::Limits(int k)
{
    float* rec = members[k].rec;
    const float e = F(rec, 0x360);
    if (q[k] > F(rec, 0x35c) && qd[k] > 0.0f) {
        if (qd[k] > 0.05f) {
            LimitImpulse(k, -((e + 1.0f) * qd[k]));
        } else {
            qd[k] = -(e * qd[k]);
            q[k] = q[k] - 0.001f;
        }
    }
    if (q[k] < F(rec, 0x358) && qd[k] < 0.0f) {
        if (qd[k] < -0.05f) {
            LimitImpulse(k, -((e + 1.0f) * qd[k]));
        } else {
            qd[k] = -(e * qd[k]);
            q[k] = q[k] + 0.001f;
        }
    }
}

// The member's ancestors from its parent up to the root.
std::vector<int> Group::Ancestors(int k)
{
    std::vector<int> chain;
    for (int p = members[k].parent; p >= 0; p = members[p].parent) chain.push_back(p);
    return chain;
}

// 0x5c7930: rec +0x2e0 = S I^A, +0x300 = S / (S . S I^A) and, for a jointed
// member, I^A_p += X^T (I^A - (S I^A) (I^A +0x300)'^T) X.
void Group::ImpulseInertia(int k)
{
    Member& m = members[k];
    float* rec = m.rec;
    SMat& ia = Mat(rec, 0x160);
    const SVec& s = V(rec, 0x260);
    SVec& x = V(rec, 0x2e0);
    VecTimesMat(s, ia, x);
    const float d = ((x.b[2] * s.t[2] + x.b[1] * s.t[1]) + x.b[0] * s.t[0]) +
                    ((s.b[2] * x.t[2] + s.b[1] * x.t[1]) + s.b[0] * x.t[0]);
    Scale(s, 1.0f / d, V(rec, 0x300));
    if (m.parent < 0) return;
    SVec t;
    SMat o, y;
    MatTimesVec(ia, V(rec, 0x300), t);
    Outer(x, t, o);
    Sub(ia, o, o);
    Transform(Rows(rec + 0x100 / 4), o, y);
    SMat& pia = Mat(members[m.parent].rec, 0x160);
    Add(pia, y, pia);
}

// 0x5c7a70: the impulse rec +0x220 handed up to the parent's through
// (1 - (S I^A) (+0x300)'^T) and the joint frame; nothing (zero) when it is
// zero or the joint is flagged (+0x368, with flag).
void Group::ImpulseUp(int k, int flag)
{
    float* rec = members[k].rec;
    SVec& pp = V(members[members[k].parent].rec, 0x220);
    if ((!flag || !Int(rec, 0x368)) && !IsZero(V(rec, 0x220))) {
        SMat o, id;
        Outer(V(rec, 0x2e0), V(rec, 0x300), o);
        for (int i = 0; i < 3; ++i) id.A[i][i] = id.D[i][i] = 1.0f;
        Sub(id, o, o);
        SVec t;
        VecTimesMat(V(rec, 0x220), o, t);
        Transform(Rows(rec + 0x100 / 4), t, pp);
        return;
    }
    pp = SVec();
}

// 0x5c7b40: the member's response to the impulses: the root's acceleration
// change (I^A a = -p, on a copy of I^A; a singular one only prints
// "GAUSSIAN ERROR"), a joint's rate change rec +0x378 = -(+0x300 . (p +
// I^A a_p)) and, for a member with children (or flag), its acceleration.
void Group::Response(int k, int flag)
{
    Member& m = members[k];
    float* rec = m.rec;
    const SVec& v = V(rec, 0x300);
    const SVec& p = V(rec, 0x220);
    SVec& a = V(rec, 0x280);
    if (m.parent < 0) {
        if (!Int(rec, 0x330)) {
            SVec t;
            SMat c = Mat(rec, 0x160);
            Scale(p, -1.0f, t);
            Solve(a, c, t);
            return;
        }
        F(rec, 0x378) = -(((v.b[2] * p.t[2] + v.b[1] * p.t[1]) + p.t[0] * v.b[0]) +
                          ((p.b[2] * v.t[2] + p.b[1] * v.t[1]) + v.t[0] * p.b[0]));
        if (Int(rec, 0x338) > 0 || flag) Scale(V(rec, 0x260), F(rec, 0x378), a);
        return;
    }
    SVec ap, b;
    Transform(Rows(rec + 0xc0 / 4), V(members[m.parent].rec, 0x280), ap);
    VecTimesMat(ap, Mat(rec, 0x160), b);
    if (!IsZero(p)) Add(p, b, b);
    F(rec, 0x378) = -(((b.t[1] * v.b[1] + b.t[2] * v.b[2]) + b.t[0] * v.b[0]) +
                      ((b.b[1] * v.t[1] + b.b[2] * v.t[2]) + b.b[0] * v.t[0]));
    if (Int(rec, 0x338) > 0 || flag) {
        Scale(V(rec, 0x260), F(rec, 0x378), a);
        Add(a, ap, a);
    }
}

// 0x5c82f0: the same for the member the impulse f acts on: +0x378 = f D^-1
// minus the parent's share.
void Group::Response(int k, float f, int flag)
{
    Member& m = members[k];
    float* rec = m.rec;
    SVec& a = V(rec, 0x280);
    if (m.parent < 0) {
        F(rec, 0x378) = f * F(rec, 0x36c);
        if (Int(rec, 0x338) > 0 || flag) Scale(V(rec, 0x260), F(rec, 0x378), a);
        return;
    }
    const SVec& v = V(rec, 0x300);
    const SVec& p = V(rec, 0x220);
    SVec ap, b;
    Transform(Rows(rec + 0xc0 / 4), V(members[m.parent].rec, 0x280), ap);
    VecTimesMat(ap, Mat(rec, 0x160), b);
    if (!IsZero(p)) Add(p, b, b);
    F(rec, 0x378) = f * F(rec, 0x36c) - (((b.t[1] * v.b[1] + b.t[2] * v.b[2]) + b.t[0] * v.b[0]) +
                                         ((b.b[1] * v.t[1] + b.b[2] * v.t[2]) + b.b[0] * v.t[0]));
    if (Int(rec, 0x338) > 0 || flag) {
        Scale(V(rec, 0x260), F(rec, 0x378), a);
        Add(a, ap, a);
    }
}

// 0x5c7d80: the joint impulse s D^-1 (S I^A) handed up the chain, every
// member's response (the root's velocity and spin change turned into the
// world by q), the joint rates changed, the velocities again (0x5c1040).
void Group::ApplyImpulse(int k, float s)
{
    Member& m = members[k];
    std::vector<int> chain;
    int at = -1;
    if (m.parent >= 0) {
        V(m.rec, 0x220) = SVec();
        SVec t;
        Scale(V(m.rec, 0x2e0), s * F(m.rec, 0x36c), t);
        Transform(Rows(m.rec + 0x100 / 4), t, V(members[m.parent].rec, 0x220));
        chain = Ancestors(k);
        at = 0;
        for (size_t i = 0; i + 1 < chain.size(); ++i) {
            ImpulseUp(chain[i], 0);
            ++at;
        }
    }
    const int n = static_cast<int>(members.size());
    for (int j = 0; j < n; ++j) {
        if (at >= 0 && j == chain[at]) --at;
        else V(members[j].rec, 0x220) = SVec();
        if (j == k) Response(j, s, 0);
        else Response(j, 0);
        if (!Int(members[j].rec, 0x330)) {
            RigidBody* b = members[0].body;
            float* rec = members[0].rec;
            float v1[4], v2[4];
            QuatRotate(b->q, rec + 0x290 / 4, v1);
            QuatRotate(b->q, rec + 0x280 / 4, v2);
            for (int i = 0; i < 3; ++i) b->vel[i] = v1[i] + b->vel[i];
            for (int i = 0; i < 3; ++i) b->w[i] = v2[i] + b->w[i];
            b->vel[3] = b->w[3] = 0;
        } else {
            qd[j] = F(members[j].rec, 0x378) + qd[j];
        }
    }
    for (int j = 0; j < n; ++j) Velocities(j);
}

void Group::ImpulseInertias()
{
    const int n = static_cast<int>(members.size());
    for (int j = 0; j < n; ++j) RigidInertia(*members[j].body, Mat(members[j].rec, 0x160));
    for (int j = n - 1; j >= 0; --j) ImpulseInertia(j);
}

void Group::BodyImpulse(int k, const SVec& p)
{
    Scale(p, -1.0f, V(members[k].rec, 0x220));
    std::vector<int> chain{ k };
    int at = 0;
    if (members[k].parent >= 0) {
        int cur = k;
        do {
            ImpulseUp(cur, 1);
            cur = members[cur].parent;
            chain.push_back(cur);
            ++at;
        } while (members[cur].parent >= 0);
    }
    const int n = static_cast<int>(members.size());
    for (int j = 0; j < n; ++j) {
        if (at >= 0 && j == chain[at]) --at;
        else V(members[j].rec, 0x220) = SVec();  // 0x5bdf60
        Response(j, 0);
        if (!Int(members[j].rec, 0x330)) {
            RigidBody* b = members[0].body;
            float* rec = members[0].rec;
            float v1[4], v2[4];
            QuatRotate(b->q, rec + 0x290 / 4, v1);
            QuatRotate(b->q, rec + 0x280 / 4, v2);
            for (int i = 0; i < 3; ++i) b->vel[i] = v1[i] + b->vel[i];
            for (int i = 0; i < 3; ++i) b->w[i] = v2[i] + b->w[i];
            b->vel[3] = b->w[3] = 0;
        } else {
            qd[j] = F(members[j].rec, 0x378) + qd[j];
        }
    }
    for (int j = 0; j < n; ++j) Velocities(j);
}

void Group::TestImpulse(int k, const SVec& p, SVec& out)
{
    Scale(p, -1.0f, V(members[k].rec, 0x220));
    std::vector<int> chain{ k };
    if (members[k].parent >= 0) {
        int cur = k;
        do {
            ImpulseUp(cur, 0);
            cur = members[cur].parent;
            chain.push_back(cur);
        } while (members[cur].parent >= 0);
    }
    for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) Response(chain[i], 1);
    out = V(members[k].rec, 0x280);
}

namespace
{
// The cross-product rows of v (0x5c67c0): x [v]x = v x x... as the game
// lays them out: (0, z, -y), (-z, 0, x), (y, -x, 0).
void Skew(const float v[3], float s[3][4])
{
    const float r[3][4] = { { 0, v[2], -v[1], 0 }, { -v[2], 0, v[0], 0 }, { v[1], -v[0], 0, 0 } };
    memcpy(s, r, sizeof(r));
}
void Blocks(const float a[3][4], const float c[3][4], SMat& out)  // 0x5be170 with B zero, D = A
{
    out = SMat();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            out.A[i][j] = a[i][j];
            out.C[i][j] = c[i][j];
            out.D[i][j] = a[i][j];
        }
}
}  // namespace

void Group::ContactInertia(int k, const float C[3][4], const float point[3], float K[3][4], SMat& x1)
{
    ImpulseInertias();
    const RigidBody& b = *members[k].body;
    float T[3][4], TT[3][4] = {}, S[3][4], C1[3][4], C2[3][4];
    for (int i = 0; i < 3; ++i) {
        VecMat3(b.R[i], C, T[i]);
        T[i][3] = 0;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) TT[i][j] = T[j][i];
    float r[3];
    VecMat3(point, TT, r);
    Skew(r, S);
    for (int i = 0; i < 3; ++i) VecMat3(TT[i], S, C1[i]);
    SMat X1, X2;
    Blocks(TT, C1, X1);
    const float n[3] = { point[0] * -1.0f, point[1] * -1.0f, point[2] * -1.0f };
    Skew(n, S);
    for (int i = 0; i < 3; ++i) VecMat3(T[i], S, C2[i]);
    Blocks(T, C2, X2);
    for (int i = 0; i < 3; ++i) {
        SVec e, p, a, o;
        e.t[i] = 1.0f;
        VecTimesMat(e, X1, p);
        TestImpulse(k, p, a);
        a.t[3] = a.b[3] = 0;
        VecTimesMat(a, X2, o);
        K[i][0] = o.b[0]; K[i][1] = o.b[1]; K[i][2] = o.b[2]; K[i][3] = 0;
    }
    x1 = X1;
}

void Group::ContactImpulse(int k, const SMat& x1, const float P[3], bool negate)
{
    SVec s, p;
    for (int i = 0; i < 3; ++i) s.t[i] = P[i];
    if (negate) Scale(s, -1.0f, s);
    VecTimesMat(s, x1, p);
    BodyImpulse(k, p);
}

namespace
{
// RigidBody::ApplyImpulse's route for a grouped body (0x5b11c0 reads both
// vectors in this branch; a missing one counts as zero here).
void GroupImpulse(void* group, int member, const float* linear, const float* angular)
{
    const float zero[3] = {};
    static_cast<Group*>(group)->Impulse(member, linear ? linear : zero, angular ? angular : zero);
}
struct InstallGroupImpulse
{
    InstallGroupImpulse() { g_groupImpulse = &GroupImpulse; }
} g_installGroupImpulse;
}  // namespace

void Group::Impulse(int k, const float J[3], const float A[3])
{
    for (Member& m : members) m.body->state = 0x65;
    const RigidBody& b = *members[k].body;
    float rt[3][4] = {};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) rt[r][c] = b.matrix[c][r];
    SVec p;
    VecMat3(J, rt, p.t);
    VecMat3(A, rt, p.b);
    p.t[3] = p.b[3] = 0;
    ImpulseInertias();
    BodyImpulse(k, p);
}

// 0x5c7280 + 0x5c81b0.
void Group::LimitImpulse(int k, float impulse)
{
    ImpulseInertias();

    Member& m = members[k];
    V(m.rec, 0x220) = SVec();
    if (m.parent >= 0) {
        SVec t;
        Scale(V(m.rec, 0x2e0), F(m.rec, 0x36c), t);
        Transform(Rows(m.rec + 0x100 / 4), t, V(members[m.parent].rec, 0x220));
        const std::vector<int> chain = Ancestors(k);
        for (size_t i = 0; i + 1 < chain.size(); ++i) ImpulseUp(chain[i], 0);
        for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) Response(chain[i], 0);
    }
    Response(k, 1.0f, 0);
    const float s = impulse / F(m.rec, 0x378);
    float mag = s;
    if (!(mag > 0.0f)) mag = -mag;
    if (mag <= 500000.0f) ApplyImpulse(k, s);  // 0x78edf4
}

// 0x5c2600: the dynamics every other call, the root moved by its spatial
// acceleration (turned into the world by q; position second order, q by
// 0.5 (q w), w first order), the joints by theirs, the limits, the
// matrices. A sleeping root (0x78) stops the group (0x5c2540).
void Group::Step(float h)
{
    if (toggle) {
        Dynamics();
        toggle = 0;
    } else {
        toggle = 1;
    }
    const int n = static_cast<int>(members.size());
    int first = 0;
    Member& root = members[0];
    if (!Int(root.rec, 0x330)) {
        RigidBody* b = root.body;
        if (b->state == 0x78) {
            for (int i = 0; i < 4; ++i) b->L[i] = b->vel[i] = b->w[i] = 0;
            for (int k = 1; k < n; ++k) {
                members[k].body->state = 0x78;
                qd[k] = 0;
            }
            return;
        }
        for (Member& m : members) m.body->state = 0x65;
        float a[4], al[4], t[3];
        QuatRotate(b->q, root.rec + 0x290 / 4, a);
        QuatRotate(b->q, root.rec + 0x280 / 4, al);
        for (int i = 0; i < 3; ++i) { t[i] = h * b->vel[i]; b->pos[i] = t[i] + b->pos[i]; }
        for (int i = 0; i < 3; ++i) { t[i] = a[i] * h; b->vel[i] = t[i] + b->vel[i]; }
        const float hh = h * 0.5f;
        for (int i = 0; i < 3; ++i) { t[i] = hh * t[i]; b->pos[i] = t[i] + b->pos[i]; }
        b->pos[3] = b->vel[3] = 0;
        const float wq[4] = { b->w[0], b->w[1], b->w[2], 0.0f };
        QuatMul(b->q, wq, b->dq);
        float d[4];
        for (int i = 0; i < 4; ++i) b->dq[i] = 0.5f * b->dq[i];
        for (int i = 0; i < 4; ++i) d[i] = h * b->dq[i];
        for (int i = 0; i < 4; ++i) b->q[i] = d[i] + b->q[i];
        QuatNormalize(b->q, b->q);
        for (int i = 0; i < 3; ++i) { t[i] = al[i] * h; b->w[i] = t[i] + b->w[i]; }
        b->w[3] = 0;
        Frames(0);
        first = 1;
    }
    for (int k = first; k < n; ++k) {
        const float qdd = F(members[k].rec, 0x378);
        q[k] = (h * qdd * 0.5f + qd[k]) * h + q[k];
        Frames(k);
        qd[k] = h * qdd + qd[k];
    }
    for (int k = first; k < n; ++k) Limits(k);
    for (int k = 0; k < n; ++k) BodyMatrix(k);
}

namespace
{
int& IntAt(float* rec, int o) { return *reinterpret_cast<int*>(rec + o / 4); }

// 0x41e990: v / |v| (the reciprocal times each component), w 0.
void Normalize(const float v[3], float out[4])
{
    const float s = 1.0f / sqrtf((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]);
    out[0] = s * v[0]; out[1] = s * v[1]; out[2] = s * v[2]; out[3] = 0;
}

// 0x5a9de0: a hinge frame: rows the reference axis made perpendicular to
// the axis (normalised), its cross product with the axis, the axis, the
// hinge point.
void HingeFrame(const float pos[3], const float axis[3], const float ref[3], float m[16])
{
    float a[4], r[4], u[4];
    Normalize(axis, a);
    const float d = (a[0] * ref[0] + a[2] * ref[2]) + a[1] * ref[1];
    for (int i = 0; i < 3; ++i) u[i] = ref[i] - a[i] * d;
    Normalize(u, r);
    float c[4];
    Cross(r, a, c);
    for (int j = 0; j < 4; ++j) { m[j] = r[j]; m[4 + j] = c[j]; m[8 + j] = a[j]; }
    m[3] = m[7] = m[11] = 0;
    m[12] = pos[0]; m[13] = pos[1]; m[14] = pos[2]; m[15] = 1.0f;
}
}  // namespace

// 0x5aada0 (the record's defaults: spring 10, damper 1, limits +-30) and
// 0x5abd60.
void Group::MakeRoot(RigidBody* body, float* rec)
{
    members.clear();
    q.assign(1, 0.0f);
    qd.assign(1, 0.0f);
    toggle = 1;
    IntAt(rec, 0x338) = 0;
    F(rec, 0x350) = 10.0f;
    F(rec, 0x354) = 1.0f;
    F(rec, 0x35c) = 30.0f;
    F(rec, 0x358) = -30.0f;
    F(rec, 0x360) = 0;
    F(rec, 0x364) = 0;
    IntAt(rec, 0x368) = 0;
    IntAt(rec, 0x330) = 0;
    IntAt(rec, 0x334) = -1;
    members.push_back({ body, rec, -1, {} });
    Frames(0);
    Link();
}

void Group::Link()
{
    for (size_t k = 0; k < members.size(); ++k) {
        members[k].body->group = this;
        members[k].body->member = static_cast<int>(k);
    }
}

int Group::AddMember(int parent, RigidBody* body, float* rec, const float cp[3], const float cax[3],
                     const float cref[3], const float pp[3], const float pax[3], const float pref[3], int type)
{
    IntAt(rec, 0x338) = 0;
    F(rec, 0x350) = 10.0f;
    F(rec, 0x354) = 1.0f;
    F(rec, 0x35c) = 30.0f;
    F(rec, 0x358) = -30.0f;
    F(rec, 0x360) = 0;
    F(rec, 0x364) = 0;
    IntAt(rec, 0x368) = 0;
    IntAt(rec, 0x330) = type;

    // the parent's side: the hinge point, the unit axis, the reference axis
    // made perpendicular to it
    Member::Child c;
    c.pos[0] = pp[0]; c.pos[1] = pp[1]; c.pos[2] = pp[2]; c.pos[3] = 0;
    Normalize(pax, c.axis);
    float d = (c.axis[2] * pref[2] + c.axis[1] * pref[1]) + c.axis[0] * pref[0];
    float u[4];
    for (int i = 0; i < 3; ++i) u[i] = pref[i] - d * c.axis[i];
    Normalize(u, c.ref);
    // the body's side (rec +0x0 point, +0x10 axis, +0x20 reference)
    float* cpos = rec;
    float* caxis = rec + 0x10 / 4;
    float* cr = rec + 0x20 / 4;
    cpos[0] = cp[0]; cpos[1] = cp[1]; cpos[2] = cp[2]; cpos[3] = 0;
    Normalize(cax, caxis);
    d = (caxis[1] * cref[1] + caxis[2] * cref[2]) + cref[0] * caxis[0];
    for (int i = 0; i < 3; ++i) u[i] = cref[i] - d * caxis[i];
    Normalize(u, cr);
    SVec& s = V(rec, 0x260);
    if (type == 2) {
        s.t[0] = s.t[1] = s.t[2] = 0;
        memcpy(s.b, caxis, 12);
    } else if (type == 1) {
        memcpy(s.t, caxis, 12);
        Cross(caxis, cp, s.b);
    }
    s.t[3] = s.b[3] = 0;
    // the rotation between the hinge frames: M = Bp^T Bc over the bases
    // (axis, axis x ref, ref) of both sides (0x41e510 rows)
    float bc[3][4], bp[3][4], m[3][4];
    memcpy(bc[0], caxis, 16);
    Cross(caxis, cr, bc[1]);
    memcpy(bc[2], cr, 16);
    memcpy(bp[0], c.axis, 16);
    Cross(c.axis, c.ref, bp[1]);
    memcpy(bp[2], c.ref, 16);
    for (int i = 0; i < 3; ++i) {
        const float col[3] = { bp[0][i], bp[1][i], bp[2][i] };
        VecMat3(col, bc, m[i]);
        m[i][3] = 0;
    }
    QuatFromRows(m, c.quat);
    float* cq = rec + 0x30 / 4;
    cq[0] = -c.quat[0]; cq[1] = -c.quat[1]; cq[2] = -c.quat[2]; cq[3] = c.quat[3];

    // the member list: right after the parent
    float* prec = members[parent].rec;
    IntAt(rec, 0x334) = IntAt(prec, 0x338);
    IntAt(prec, 0x338) = IntAt(prec, 0x338) + 1;
    const int at = parent + 1;
    for (Member& o : members) {
        if (o.parent >= at) ++o.parent;
        for (Member::Child& oc : o.children)
            if (oc.member >= at) ++oc.member;
    }
    c.member = at;
    members[parent].children.push_back(c);
    Member nm;
    nm.body = body;
    nm.rec = rec;
    nm.parent = parent;
    members.insert(members.begin() + at, nm);
    q.insert(q.begin() + at, 0.0f);
    qd.insert(qd.begin() + at, 0.0f);
    for (int k = 0; k < static_cast<int>(members.size()); ++k) IntAt(members[k].rec, 0x328) = k;
    F(rec, 0x378) = 0;

    // the frames: +0x80 the body's hinge frame, +0x40 the inverse of the
    // parent's
    float f[16];
    HingeFrame(cp, cax, cref, rec + 0x80 / 4);
    HingeFrame(pp, pax, pref, f);
    RigidInverse(f, rec + 0x40 / 4);
    q[at] = 0;
    Frames(at);
    qd[at] = 0;
    if (type == 1) {
        float a[4];
        Normalize(cax, a);
        Cross(a, cp, rec + 0x140 / 4);
        Cross(rec + 0x140 / 4, a, rec + 0x150 / 4);
    }
    BodyMatrix(at);
    Velocities(at);
    return at;
}

void Group::Detach(int k)
{
    const int n = static_cast<int>(members.size());
    for (int i = 0; i < n; ++i) Velocities(i);
    for (int i = 0; i < n; ++i) BodyMatrix(i);
    Leave(k);
}

void Group::Leave(int k)
{
    RigidBody* b = members[k].body;
    if (Int(members[k].rec, 0x330)) {
        float w[4], v[4];
        QuatRotate(b->q, b->w, w);
        QuatRotate(b->q, b->vel, v);
        memcpy(b->w, w, 12);
        b->w[3] = 0;
        memcpy(b->vel, v, 12);
        b->vel[3] = 0;
    }
    QuatToRows(b->q, b->R);
    // R' (the rows' columns), D, M = D R, N = R' M, L = w N
    float rt[3][4] = {}, d[3][4] = {}, m[3][4] = {}, n[3][4] = {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) rt[i][j] = b->R[j][i];
        d[i][i] = b->inertia[i];
    }
    for (int i = 0; i < 3; ++i) VecMat3(d[i], b->R, m[i]);
    for (int i = 0; i < 3; ++i) VecMat3(rt[i], m, n[i]);
    VecMat3(b->w, n, b->L);
    b->L[3] = 0;
    b->UpdateDerived();
    // out of the parent's joints and the member list
    const int parent = members[k].parent;
    if (parent >= 0) {
        std::vector<Member::Child>& ch = members[parent].children;
        for (size_t i = 0; i < ch.size(); ++i)
            if (ch[i].member == k) {
                ch.erase(ch.begin() + static_cast<long>(i));
                IntAt(members[parent].rec, 0x338) = IntAt(members[parent].rec, 0x338) - 1;
                break;
            }
    }
    members.erase(members.begin() + k);
    q.erase(q.begin() + k);
    qd.erase(qd.begin() + k);
    for (Member& o : members) {
        if (o.parent > k) --o.parent;
        for (Member::Child& oc : o.children)
            if (oc.member > k) --oc.member;
    }
    for (int i = 0; i < static_cast<int>(members.size()); ++i) IntAt(members[i].rec, 0x328) = i;
    b->group = nullptr;
    b->member = -1;
    Link();
}

void Group::End()
{
    while (!members.empty()) Leave(static_cast<int>(members.size()) - 1);
}

// 0x5c2100.
void Group::Dynamics()
{
    const int n = static_cast<int>(members.size());
    for (int k = 0; k < n; ++k) Velocities(k);
    for (int k = 0; k < n; ++k) Bias(k);
    for (int k = n - 1; k >= 0; --k) Inward(k);
    for (int k = 0; k < n; ++k) Accelerations(k);
}
}  // namespace Articulated
