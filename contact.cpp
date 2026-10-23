// Body contacts (contact.h).
#include "contact.h"

#include "articulated.h"

#include <cmath>
#include <cstring>

namespace {

// The surface table 0x71c2e8 (0x3a entries of 20 bytes; 29..57 repeat
// 0..28): four floats and a debug colour. 0x59b090 returns (t[1], t[0],
// t[2]); out of range (0.77, 0.5, 1.0).
const float kSurface[29][3] = {
    { 0.55f, 0.84f, 1.0f }, { 0.55f, 0.84f, 1.0f }, { 0.01f, 0.9f, 1.0f },  { 0.55f, 0.84f, 1.1f },
    { 0.4f, 0.2f, 1.0f },   { 0.2f, 0.9f, 1.0f },   { 0.55f, 0.84f, 1.0f }, { 0.25f, 0.7f, 1.0f },
    { 0.2f, 0.9f, 1.0f },   { 0.55f, 0.84f, 1.0f }, { 0.1f, 0.8f, 1.0f },   { 0.65f, 0.35f, 1.0f },
    { 0.8f, 0.9f, 1.0f },   { 0.2f, 0.8f, 1.0f },   { 0.55f, 0.84f, 1.0f }, { 0.2f, 0.9f, 1.1f },
    { 0.6f, 0.9f, 1.0f },   { 0.55f, 0.84f, 0.8f }, { 0.55f, 0.84f, 1.0f }, { 0.0f, 0.0f, 0.0f },
    { 0.4f, 0.3f, 1.0f },   { 0.95f, 0.3f, 0.1f },  { 0.2f, 0.7f, 1.0f },   { 0.55f, 0.84f, 1.0f },
    { 0.65f, 0.84f, 1.0f }, { 0.65f, 0.84f, 1.0f }, { 0.1f, 0.9f, 1.0f },   { 0.55f, 0.84f, 1.0f },
    { 0.01f, 0.35f, 1.0f },
};

// t[0], t[1], t[2] of a surface.
void SurfaceValues(int s, float t[3])
{
    if (s < 0 || s >= 0x3a) {
        t[0] = 0.5f; t[1] = 0.77f; t[2] = 1.0f;
        return;
    }
    for (int k = 0; k < 3; ++k) t[k] = kSurface[s % 29][k];
}

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void Cross(const float a[3], const float b[3], float out[3])
{
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    out[0] = x; out[1] = y; out[2] = z;
}

// Row-vector 4 x 4 product a b.
void Mul(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            out[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k] + a[r * 4 + 3] * b[12 + k];
}

// A box as 0x5b8df0 builds it (0x80 bytes): the three axis rows of its
// world matrix (0x4b5380), the centre (row 3), the axes scaled by the half
// extents and the half extents.
struct Box
{
    float axis[3][3], c[3], sa[3][3], half[3];
};

void MakeBox(const float size[3], const float* world, Box& b)
{
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) {
            b.axis[i][k] = world[i * 4 + k];
            b.sa[i][k] = size[i] * world[i * 4 + k];
        }
        b.c[i] = world[12 + i];
        b.half[i] = size[i];
    }
}

// The separating-axis record of 0x5b9340 (+0x00 point, +0x10 second point,
// +0x20 normal, +0x30..+0x6c the two edges, +0x70 best depth, +0x74 found,
// +0x78 edge contact).
struct Sat
{
    float p0[3] = {}, p1[3] = {}, n[3] = {};
    float a0[3] = {}, a1[3] = {}, b0[3] = {}, b1[3] = {};
    float best = 1000000.0f;
    bool found = false, edge = false;
};

// 0x5b9500: face i of box F against box O; sign +1 for A's faces (F = A),
// -1 for B's (F = B). False: separated on this axis.
bool FaceAxis(const Box& F, const Box& O, int i, int sign, Sat& r)
{
    float n[3] = { F.axis[i][0], F.axis[i][1], F.axis[i][2] };
    const float d[3] = { O.c[0] - F.c[0], O.c[1] - F.c[1], O.c[2] - F.c[2] };
    float s = Dot(d, n);
    if (s >= 0) {
        s = -s;
    } else {
        for (float& v : n) v = -v;
    }
    s += F.half[i];
    float p[3] = { O.c[0], O.c[1], O.c[2] };
    for (int j = 0; j < 3; ++j) {
        const float e = Dot(n, O.sa[j]);
        if (e >= 0) {
            s += e;
            for (int k = 0; k < 3; ++k) p[k] -= O.sa[j][k];
        } else {
            s -= e;
            for (int k = 0; k < 3; ++k) p[k] += O.sa[j][k];
        }
    }
    if (s < 0) return false;
    if (s < r.best) {
        r.best = s;
        memcpy(r.p0, p, sizeof(p));
        r.found = true;
        for (int k = 0; k < 3; ++k) r.n[k] = sign > 0 ? -n[k] : n[k];
    }
    return true;
}

// 0x5b9930: edge i of A against edge j of B (normal oriented from A to B).
bool EdgeAxis(const Box& A, const Box& B, int i, int j, Sat& r)
{
    float n[3];
    Cross(B.axis[j], A.axis[i], n);  // 0x407640 (b x a)
    const float l2 = Dot(n, n);
    if (!(l2 >= 9.99999944e-11f)) return true;
    const float l = std::sqrt(l2);
    for (float& v : n) v /= l;
    const float d[3] = { B.c[0] - A.c[0], B.c[1] - A.c[1], B.c[2] - A.c[2] };
    float s = Dot(d, n);
    if (s >= 0) {
        s = -s;
    } else {
        for (float& v : n) v = -v;
    }
    float pa[3] = { A.c[0], A.c[1], A.c[2] }, pb[3] = { B.c[0], B.c[1], B.c[2] };
    for (int k = 0; k < 3; ++k) {
        if (k != i) {
            const float e = Dot(n, A.sa[k]);
            if (e >= 0) {
                s += e;
                for (int m = 0; m < 3; ++m) pa[m] += A.sa[k][m];
            } else {
                s -= e;
                for (int m = 0; m < 3; ++m) pa[m] -= A.sa[k][m];
            }
        }
        if (k != j) {
            const float e = Dot(n, B.sa[k]);
            if (e >= 0) {
                s += e;
                for (int m = 0; m < 3; ++m) pb[m] -= B.sa[k][m];
            } else {
                s -= e;
                for (int m = 0; m < 3; ++m) pb[m] += B.sa[k][m];
            }
        }
    }
    if (s < 0) return false;
    if (s < r.best && s > 0) {
        r.best = s;
        r.found = true;
        for (int m = 0; m < 3; ++m) {
            r.a0[m] = pa[m] + A.sa[i][m];
            r.a1[m] = pa[m] - A.sa[i][m];
            r.b0[m] = pb[m] + B.sa[j][m];
            r.b1[m] = pb[m] - B.sa[j][m];
            r.p0[m] = pa[m];
            r.p1[m] = pb[m];
            r.n[m] = n[m];
        }
    }
    return true;
}

// 0x5b8ab0: the closest points of the two edges into out (and their
// distance as the depth); false when they are parallel or meet outside.
bool EdgeClosest(const Sat& t, Sat& out)
{
    float w[3], v[3], u[3];
    for (int k = 0; k < 3; ++k) {
        w[k] = t.a0[k] - t.b0[k];
        v[k] = t.b1[k] - t.b0[k];
        u[k] = t.a1[k] - t.a0[k];
    }
    const float vv = Dot(v, v);
    if (!(vv >= 9.99999944e-11f)) return false;
    const float uu = Dot(u, u);
    if (!(uu >= 9.99999944e-11f)) return false;
    const float vw = Dot(v, w), uv = Dot(u, v);
    const float den = uu * vv - uv * uv;
    if (!(std::fabs(den) >= 9.99999975e-06f)) return false;
    const float s = (uv * vw - Dot(u, w) * vv) / den;
    const float tt = (s * uv + vw) / vv;
    if (!(s >= 0 && s <= 1 && tt >= 0 && tt <= 1)) return false;
    float d[3];
    for (int k = 0; k < 3; ++k) {
        out.p0[k] = t.a0[k] + u[k] * s;
        out.p1[k] = t.b0[k] + v[k] * tt;
        d[k] = out.p0[k] - out.p1[k];
    }
    out.best = std::sqrt(Dot(d, d));
    return true;
}

// 0x5b8df0 / 0x5b9340: box A against box B. The normal points from B to A.
bool BoxBox(const Box& A, const Box& B, BodyContact& c)
{
    Sat r;
    for (int i = 0; i < 3; ++i)
        if (!FaceAxis(A, B, i, 1, r)) return false;
    for (int i = 0; i < 3; ++i)
        if (!FaceAxis(B, A, i, -1, r)) return false;
    Sat t;
    t.best = r.found ? r.best : 1000000.0f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (!EdgeAxis(A, B, i, j, t)) return false;
    if (t.found && EdgeClosest(t, r)) {
        for (int k = 0; k < 3; ++k) r.n[k] = -t.n[k];
        r.edge = true;
        r.found = true;
    }
    if (!r.found) return false;
    for (int k = 0; k < 3; ++k) {
        c.normal[k] = r.n[k];
        c.pointA[k] = r.p0[k];
        c.pointB[k] = r.edge ? r.p1[k] : r.p0[k];
    }
    c.depth = r.best;
    return true;
}

// 0x5b71e0: box A against sphere B (radius r, centre ctr): the sphere's
// centre in the box's frame, then by region (only one axis outside, or
// none: that face, x when inside; two: the edge; three: the corner).
bool BoxSphere(const Box& A, float radius, const float ctr[3], BodyContact& c)
{
    float d[3] = { ctr[0] - A.c[0], ctr[1] - A.c[1], ctr[2] - A.c[2] };
    float p[3], sg[3], out[3];
    for (int i = 0; i < 3; ++i) {
        p[i] = Dot(d, A.axis[i]);
        sg[i] = p[i] < 0 ? -1.0f : 1.0f;
        out[i] = sg[i] * p[i] - A.half[i];
    }
    const bool ox = out[0] > 0, oy = out[1] > 0, oz = out[2] > 0;
    int count, axis = 0;  // the region and its axis (face: the face, edge: along)
    if (oy) {
        if (oz) {
            if (ox) count = 2;
            else count = 1, axis = 0;
        } else if (ox) {
            count = 1, axis = 2;
        } else {
            count = 0, axis = 1;
        }
    } else if (oz) {
        if (ox) count = 1, axis = 1;
        else count = 0, axis = 2;
    } else {
        count = 0, axis = 0;
    }
    float nl[3] = {};
    if (count == 0) {
        const float depth = (A.half[axis] + radius) - sg[axis] * p[axis];
        if (!(depth > 0)) return false;
        c.depth = depth;
        nl[axis] = -sg[axis];
    } else {
        float q[3];
        for (int i = 0; i < 3; ++i) q[i] = sg[i] * A.half[i];
        if (count == 1) q[axis] = p[axis];
        float e[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
        const float dist = std::sqrt(Dot(e, e));
        if (!(dist >= 9.99999975e-06f) || !(radius - dist > 0)) return false;
        c.depth = radius - dist;
        for (int i = 0; i < 3; ++i) nl[i] = e[i] / dist;
    }
    for (int k = 0; k < 3; ++k) {
        c.normal[k] = nl[0] * A.axis[0][k] + nl[1] * A.axis[1][k] + nl[2] * A.axis[2][k];
        c.pointA[k] = c.pointB[k] = ctr[k] + c.normal[k] * radius;
    }
    return true;
}

// 0x5a3ba0 for a box of the body against a static shape (a cylinder is
// taken as its bounding box: box / cylinder 0x5b4380 is not ported).
bool ShapePair(const float sizeA[3], const float* worldA, const CollisionWorld::Shape& B, BodyContact& c)
{
    Box a;
    MakeBox(sizeA, worldA, a);
    if (B.type == 2) return BoxSphere(a, B.size[0], B.world + 12, c);
    float size[3] = { B.size[0], B.size[1], B.size[2] };
    if (B.type == 1) size[2] = size[0];
    if (B.type > 2) return false;
    Box b;
    MakeBox(size, B.world, b);
    return BoxBox(a, b, c);
}

// 0x5c2a30: the contact frame, rows (u, n x u, n) with u perpendicular to
// n from its largest component.
void ContactFrame(const float n[3], float R[3][3])
{
    int i = 0;
    float m = std::fabs(n[0]);
    if (m < std::fabs(n[1])) i = 1, m = std::fabs(n[1]);
    if (m < std::fabs(n[2])) i = 2;
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    float u[3];
    u[j] = 1.0f;
    u[k] = 1.0f;
    u[i] = -((n[j] + n[k]) / n[i]);
    const float l = std::sqrt(Dot(u, u));
    for (float& v : u) v /= l;
    float t[3];
    Cross(n, u, t);
    for (int c = 0; c < 3; ++c) {
        R[0][c] = u[c];
        R[1][c] = t[c];
        R[2][c] = n[c];
    }
}

bool Invert3(const float m[3][3], float out[3][3])
{
    const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const float c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const float c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!(std::fabs(det) > 1e-20f)) return false;
    const float id = 1.0f / det;
    out[0][0] = c00 * id;
    out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    out[1][0] = c01 * id;
    out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    out[2][0] = c02 * id;
    out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    return true;
}

// 0x5a99d0 -> 0x5c5260 for the body against a static object (other null:
// B does not move) or against a free body other (a loose panel,
// BOOT_TO_MENU.md section 76: the relative velocity A - B, K = K_A + K_B,
// -J on B, the push-out shared by the masses): true when an impulse was
// applied.
bool Respond(RigidBody& body, const BodyContact& c, float Jout[3] = nullptr, RigidBody* other = nullptr)
{
    float tA[3], tB[3];
    SurfaceValues(c.surfaceA, tA);
    SurfaceValues(c.surfaceB, tB);
    const float mu = tA[1] * tB[1];               // +0x48 x +0x4c
    const float e = tA[0] * tB[0];                // +0x38 x +0x3c
    const float grip = (tA[2] + tB[2]) * 0.5f;    // (+0x50 + +0x54) / 2
    // 0x5c2bc0: the relative velocity of the contact point in the frame;
    // separating (z >= 0): nothing (no push-out either).
    float R[3][3];
    ContactFrame(c.normal, R);
    const float r[3] = { c.pointA[0] - body.pos[0], c.pointA[1] - body.pos[1], c.pointA[2] - body.pos[2] };
    // A grouped body (0x5c5260's branches): a jointed member keeps its
    // velocities in its own frame (0x5c1040), turned into the world by q
    // (0x58c1f0); the root's are world ones.
    Articulated::Group* group = static_cast<Articulated::Group*>(body.group);
    float bv[4] = { body.vel[0], body.vel[1], body.vel[2], 0 }, bw[4] = { body.w[0], body.w[1], body.w[2], 0 };
    if (group && group->members[body.member].parent >= 0) {
        Articulated::QuatRotate(body.q, body.vel, bv);
        Articulated::QuatRotate(body.q, body.w, bw);
    }
    float wr[3];
    Cross(bw, r, wr);
    float vp[3] = { bv[0] + wr[0], bv[1] + wr[1], bv[2] + wr[2] };
    float rb[3] = {};
    if (other) {
        for (int k = 0; k < 3; ++k) rb[k] = c.pointB[k] - other->pos[k];
        float wrb[3];
        Cross(other->w, rb, wrb);
        for (int k = 0; k < 3; ++k) vp[k] = vp[k] - (other->vel[k] + wrb[k]);
    }
    float v[3];
    for (int i = 0; i < 3; ++i) v[i] = Dot(R[i], vp);
    if (v[2] >= 0) return false;
    // 0x5c3110: K (velocity change per impulse) in the frame.
    float Kw[3][3];
    for (int k = 0; k < 3; ++k) {
        float ek[3] = {}, a[3], wa[3], dv[3];
        ek[k] = 1.0f;
        Cross(r, ek, a);
        VecMat3(a, body.invI, wa);
        Cross(wa, r, dv);
        for (int m = 0; m < 3; ++m) Kw[m][k] = ek[m] / body.mass + dv[m];
    }
    float K[3][3], Ki[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float s = 0;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) s += R[i][a] * Kw[a][b] * R[j][b];
            K[i][j] = s;
        }
    // A grouped body's K through its group (0x5c3110 -> 0x5c67c0: test
    // impulses along the frame's axes), in the contact frame.
    Articulated::SMat x1;
    if (group) {
        // (BOOT_TO_MENU.md section 76) the contact record's +0x00 holds the
        // frame's axes as COLUMNS (R'; +0x30 is R) and its point +0x80 is
        // the offset in the frame (world offset = point x R): passing R and
        // the world offset, as stage 4 did, gave a K that does not match
        // the impulse's effect (tests\groupreplay kcheck) and the grouped
        // car gained speed into walls
        const float rows[3][4] = { { R[0][0], R[1][0], R[2][0], 0 }, { R[0][1], R[1][1], R[2][1], 0 },
                                   { R[0][2], R[1][2], R[2][2], 0 } };
        float pf[3];
        VecMat3(r, rows, pf);
        float Kg[3][4];
        group->ContactInertia(body.member, rows, pf, Kg, x1);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) K[i][j] = Kg[i][j];
    }
    if (other) {
        // B's share (free: 1/m and [rb]x invI [rb]x), in the frame
        float Kb[3][3];
        for (int k = 0; k < 3; ++k) {
            float ek[3] = {}, a[3], wa[3], dv[3];
            ek[k] = 1.0f;
            Cross(rb, ek, a);
            VecMat3(a, other->invI, wa);
            Cross(wa, rb, dv);
            for (int m = 0; m < 3; ++m) Kb[m][k] = ek[m] / other->mass + dv[m];
        }
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                float s = 0;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b) s += R[i][a] * Kb[a][b] * R[j][b];
                K[i][j] = K[i][j] + s;
            }
    }
    if (!Invert3(K, Ki)) return false;
    // 0x5c49c0: f stops the normal velocity alone, g all of it; P = f (1 +
    // e) + (g - f) grip, kept in the friction cone along the same line.
    const float f[3] = { 0, 0, -(v[2] / K[2][2]) };
    float g[3];
    for (int i = 0; i < 3; ++i) g[i] = -(Ki[i][0] * v[0] + Ki[i][1] * v[1] + Ki[i][2] * v[2]);
    const float e1 = e + 1.0f;
    float P[3];
    for (int i = 0; i < 3; ++i) P[i] = (g[i] - f[i]) * grip + f[i] * e1;
    if (P[2] * mu * P[2] * mu < P[0] * P[0] + P[1] * P[1]) {
        const float d = std::sqrt(g[0] * g[0] + g[1] * g[1]) - (g[2] - f[2]) * mu;
        if (std::fabs(d) <= 9.99999975e-06f) {
            P[0] = P[1] = P[2] = 0;
        } else {
            const float s = (e1 * f[2] * mu) / d;
            for (int i = 0; i < 3; ++i) P[i] = (g[i] - f[i]) * s + f[i] * e1;
        }
    }
    float J[3], ang[3];
    for (int k = 0; k < 3; ++k) J[k] = R[0][k] * P[0] + R[1][k] * P[1] + R[2][k] * P[2];
    Cross(r, J, ang);
    if (group) group->ContactImpulse(body.member, x1, P, false);  // 0x5a9da0, 0x5be2b0, 0x5c7390
    else body.ApplyImpulse(J, ang);
    if (other) {
        const float nj[3] = { -J[0], -J[1], -J[2] };
        float angB[3];
        Cross(rb, nj, angB);
        other->ApplyImpulse(nj, angB);
    }
    if (Jout)
        for (int k = 0; k < 3; ++k) Jout[k] = J[k];
    // 0x5c5c50: the depth along the normal, at most 0.01; a static B weighs
    // 1e16, so the body takes all of it.
    float corr[3] = { c.depth * c.normal[0], c.depth * c.normal[1], c.depth * c.normal[2] };
    const float len = std::sqrt(Dot(corr, corr));
    if (len > 0.00999999978f)
        for (float& x : corr) x *= 0.00999999978f / len;
    const float mB = other ? other->mass : 1e16f;
    const float share = mB / (body.mass + mB);
    for (int k = 0; k < 3; ++k) body.pos[k] += corr[k] * share;
    body.UpdateMatrix();
    if (other) {
        const float shareB = body.mass / (body.mass + mB);
        for (int k = 0; k < 3; ++k) other->pos[k] -= corr[k] * shareB;
        other->UpdateMatrix();
    }
    return true;
}

}  // namespace

bool BoxBoxContact(const float sizeA[3], const float* worldA, const float sizeB[3], const float* worldB, BodyContact& c)
{
    Box a, b;
    MakeBox(sizeA, worldA, a);
    MakeBox(sizeB, worldB, b);
    return BoxBox(a, b, c);
}

int BodyContacts::Step(RigidBody& body, const std::vector<VehicleData::Shape>& shapes, const CollisionWorld& world,
                       const CityGround* groundData)
{
    pairs = contacts = ground = 0;
    deepest = 0;
    if (shapes.empty() || body.state == 0x78) return 0;
    const float* bodyWorld = &body.matrix[0][0];
    auto shapeWorld = [&](size_t s, float out[16]) { Mul(shapes[s].local, bodyWorld, out); };
    // The body's bounding box (0x5a95b0): its bounding shape's.
    float bw[16];
    shapeWorld(0, bw);
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        const float e = std::fabs(bw[k]) * shapes[0].size[0] + std::fabs(bw[4 + k]) * shapes[0].size[1] +
                        std::fabs(bw[8 + k]) * shapes[0].size[2];
        lo[k] = bw[12 + k] - e;
        hi[k] = bw[12 + k] + e;
    }
    const size_t first = shapes.size() > 1 ? 1 : 0;
    world.Overlapping(lo, hi, near_);
    for (uint32_t o : near_) {
        ++pairs;
        const CollisionWorld::Object& ob = world.ObjectAt(o);
        // 0x5a3730: the bounds first.
        BodyContact best;
        shapeWorld(0, bw);
        if (!ShapePair(shapes[0].size, bw, world.ShapeAt(ob.first), best)) continue;
        bool found = false;
        if (shapes.size() == 1 && ob.count == 1) {
            best.surfaceA = shapes[0].surface;
            best.surfaceB = world.ShapeAt(ob.first).surface;
            found = true;
        } else {
            const size_t firstB = ob.count > 1 ? 1 : 0;
            for (size_t i = first; i < shapes.size(); ++i) {
                float sw[16];
                shapeWorld(i, sw);
                for (size_t j = firstB; j < ob.count; ++j) {
                    const CollisionWorld::Shape& sb = world.ShapeAt(ob.first + j);
                    BodyContact c;
                    if (!ShapePair(shapes[i].size, sw, sb, c)) continue;
                    if (!found || best.depth < c.depth) {
                        best = c;
                        best.surfaceA = shapes[i].surface;
                        best.surfaceB = sb.surface;
                        found = true;
                    }
                }
            }
        }
        // 0x5a9af0: a real depth, neither surface 0x13.
        if (!found || !(best.depth > 9.99999975e-06f) || best.surfaceA == 0x13 || best.surfaceB == 0x13) continue;
        if (best.depth > deepest) deepest = best.depth;
        float J[3];
        if (Respond(body, best, J)) {
            ++contacts;
            if (onImpulse) onImpulse(best, J);
        }
        bodyWorld = &body.matrix[0][0];
    }
    // 0x5a9b80 -> 0x5b36f0: the ground under the boxes' corners.
    if (groundData) {
        BodyContact g;
        bool found = false;
        for (size_t s = first; s < shapes.size(); ++s) {
            float sw[16];
            shapeWorld(s, sw);
            Box b;
            MakeBox(shapes[s].size, sw, b);
            for (int k = 0; k < 8; ++k) {
                float p[3];
                for (int m = 0; m < 3; ++m)
                    p[m] = b.c[m] + ((k & 1) ? b.sa[0][m] : -b.sa[0][m]) + ((k & 2) ? b.sa[1][m] : -b.sa[1][m]) +
                           ((k & 4) ? b.sa[2][m] : -b.sa[2][m]);
                float h, n[3];
                int surf = 0;
                if (!groundData->Probe(p[0], p[1], p[2], h, n, &surf)) continue;
                const float depth = h - p[1];
                if (depth > (found ? g.depth : 0.0f)) {
                    const float l = std::sqrt(Dot(n, n));
                    if (!(l > 0)) continue;
                    found = true;
                    g.depth = depth;
                    for (int m = 0; m < 3; ++m) g.normal[m] = n[m] / l;
                    g.pointA[0] = p[0]; g.pointA[1] = p[1]; g.pointA[2] = p[2];
                    g.pointB[0] = p[0]; g.pointB[1] = h; g.pointB[2] = p[2];
                    g.surfaceA = shapes[s].surface;
                    g.surfaceB = surf;
                }
            }
        }
        if (found && g.depth > 9.99999975e-06f) {
            if (g.depth > deepest) deepest = g.depth;
            if (Respond(body, g)) ++ground;
        }
    }
    return contacts + ground;
}

int BodyContacts::Pair(RigidBody& a, const std::vector<VehicleData::Shape>& shapesA, RigidBody& b,
                       const std::vector<VehicleData::Shape>& shapesB)
{
    if (shapesA.empty() || shapesB.empty()) return 0;
    if (a.state == 0x78 && b.state == 0x78) return 0;
    // members of one group never meet (0x5a31f0)
    if (a.group && a.group == b.group) return 0;
    auto world = [](const VehicleData::Shape& s, const RigidBody& body, float out[16]) {
        Mul(s.local, &body.matrix[0][0], out);
    };
    // 0x5a3730: the bounds first, then every shape pair but the bounds
    float wa[16], wb[16];
    world(shapesA[0], a, wa);
    world(shapesB[0], b, wb);
    BodyContact best;
    if (!BoxBoxContact(shapesA[0].size, wa, shapesB[0].size, wb, best)) return 0;
    bool found = false;
    if (shapesA.size() == 1 && shapesB.size() == 1) {
        best.surfaceA = shapesA[0].surface;
        best.surfaceB = shapesB[0].surface;
        found = true;
    } else {
        for (size_t i = shapesA.size() > 1 ? 1 : 0; i < shapesA.size(); ++i) {
            world(shapesA[i], a, wa);
            for (size_t j = shapesB.size() > 1 ? 1 : 0; j < shapesB.size(); ++j) {
                world(shapesB[j], b, wb);
                BodyContact c;
                if (!BoxBoxContact(shapesA[i].size, wa, shapesB[j].size, wb, c)) continue;
                if (!found || best.depth < c.depth) {
                    best = c;
                    best.surfaceA = shapesA[i].surface;
                    best.surfaceB = shapesB[j].surface;
                    found = true;
                }
            }
        }
    }
    if (!found || !(best.depth > 9.99999975e-06f) || best.surfaceA == 0x13 || best.surfaceB == 0x13) return 0;
    float J[3];
    if (!Respond(a, best, J, &b)) return 0;
    if (onImpulse) onImpulse(best, J);
    return 1;
}

bool PairResponse(RigidBody& a, RigidBody& b, const BodyContact& c, float J[3])
{
    return Respond(a, c, J, &b);
}
