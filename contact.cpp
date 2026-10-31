// Body contacts (contact.h).
#include "contact.h"

#include "articulated.h"
#include "panels.h"
#include "props.h"

#include <cmath>
#include <cstring>
#include <utility>

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
    bool local = false;  // +0x7c: in the cylinder's frame (box / cylinder)
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

// The dot product as 0x5b8ab0 sums it: (z + y) + x.
float DotZYX(const float a[3], const float b[3]) { return (a[2] * b[2] + a[1] * b[1]) + a[0] * b[0]; }

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
    const float vv = DotZYX(v, v);
    if (!(vv >= 9.99999944e-11f)) return false;
    const float uu = DotZYX(u, u);
    if (!(uu >= 9.99999944e-11f)) return false;
    const float vw = DotZYX(v, w), uv = DotZYX(u, v);
    const float den = uu * vv - uv * uv;
    if (!(std::fabs(den) >= 9.99999975e-06f)) return false;
    const float s = (uv * vw - DotZYX(u, w) * vv) / den;
    const float tt = (s * uv + vw) / vv;
    if (!(s >= 0 && s <= 1 && tt >= 0 && tt <= 1)) return false;
    float d[3];
    for (int k = 0; k < 3; ++k) {
        out.p0[k] = t.a0[k] + u[k] * s;
        out.p1[k] = t.b0[k] + v[k] * tt;
        d[k] = out.p0[k] - out.p1[k];
    }
    out.best = std::sqrt(DotZYX(d, d));
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

// 0x41e990: v times 1 / sqrt((x x + y y) + z z).
void Normalize(const float v[3], float out[3])
{
    const float inv = 1.0f / std::sqrt((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]);
    const float x = inv * v[0], y = inv * v[1], z = inv * v[2];
    out[0] = x; out[1] = y; out[2] = z;
}

// The cylinder as 0x5b4380 builds it: its axis (matrix row 1), centre
// (row 3), the axis times the half height, radius +0x30, half height +0x34.
struct Cyl
{
    float axis[3], c[3], ah[3], r, h;
};

const float kCylEps = 9.99999975e-06f;   // [0x78e9b0]
const float kCrossEps = 9.99999944e-11f; // [0x78eb28]

// The separating-axis tests of 0x5b49b0 write R.local (+0x7c) when their
// points and normal are in the cylinder's frame (L: the box's matrix in that
// frame, 0x5b4120); the box face tests work in the world.

// 0x5b4b60: face i of box B. The face normal against the cylinder: its
// segment end and its rim point furthest along the normal.
bool CylFaceAxis(const Box& B, const Cyl& C, int i, Sat& r)
{
    float n[3] = { B.axis[i][0], B.axis[i][1], B.axis[i][2] };
    const float d[3] = { C.c[0] - B.c[0], C.c[1] - B.c[1], C.c[2] - B.c[2] };
    float s = (d[2] * n[2] + d[1] * n[1]) + d[0] * n[0];
    if (s < 0) {
        for (float& v : n) v = -v;
    } else {
        s = -s;
    }
    s += B.half[i];
    float q[3] = { C.c[0], C.c[1], C.c[2] };
    const float t = (n[2] * C.ah[2] + n[1] * C.ah[1]) + n[0] * C.ah[0];
    if (t < 0) {
        s = s - t;
        for (int k = 0; k < 3; ++k) q[k] = C.ah[k] + q[k];  // 0x41e4b0
    } else {
        s = s + t;
        for (int k = 0; k < 3; ++k) q[k] = q[k] - C.ah[k];
    }
    const float e = -((n[0] * C.axis[0] + n[2] * C.axis[2]) + n[1] * C.axis[1]);
    if (1.0f - std::fabs(e) > kCylEps) {
        // the normal without its part along the axis, to the rim
        float w[3];
        for (int k = 0; k < 3; ++k) w[k] = e * C.axis[k] + n[k];
        Normalize(w, w);
        for (float& v : w) v = v * C.r;
        const float f = (w[2] * n[2] + w[1] * n[1]) + w[0] * n[0];
        if (f < 0) {
            s = s - f;
            for (int k = 0; k < 3; ++k) q[k] = w[k] + q[k];
        } else {
            s = f + s;
            for (int k = 0; k < 3; ++k) q[k] = q[k] - w[k];
        }
    }
    if (s < 0) return false;
    if (s < r.best) {
        r.best = s;
        if (!(s < 0)) {
            memcpy(r.p0, q, sizeof(q));
            r.found = true;
            for (int k = 0; k < 3; ++k) r.n[k] = -n[k];
        }
    }
    return true;
}

// 0x5b4f90: the direction from the axis to the box's centre (in L). The
// box's corner deepest along it; a contact only when that corner is inside
// the cylinder. Nothing to test with the centre on the axis.
bool CylRadialAxis(const float* L, const Box& B, const Cyl& C, Sat& r)
{
    const float cx = L[12], cz = L[14];
    if (std::fabs(cx) < kCylEps && std::fabs(cz) < kCylEps) return true;
    const float flat[3] = { cx, 0.0f, cz };
    float u[3];
    Normalize(flat, u);
    float s = -std::sqrt(cz * cz + cx * cx) + C.r;
    float p[3] = { cx, L[13], cz };
    for (int j = 0; j < 3; ++j) {
        float e[3];
        for (int k = 0; k < 3; ++k) e[k] = B.half[j] * L[4 * j + k];
        const float d = (u[2] * e[2] + u[1] * e[1]) + e[0] * u[0];
        if (d < 0) {
            s = s - d;
            for (int k = 0; k < 3; ++k) p[k] = e[k] + p[k];
        } else {
            s = s + d;
            for (int k = 0; k < 3; ++k) p[k] = p[k] - e[k];
        }
    }
    if (s < 0) return false;
    const float q2 = p[2] * p[2] + p[0] * p[0];
    if (!(q2 < C.r * C.r)) return true;
    const float ay = p[1] > 0 ? p[1] : -p[1];
    if (!(ay < C.h)) return true;
    const float dist = std::sqrt(q2);
    const float depth = C.r - dist;
    if (!(depth < r.best)) return true;
    r.best = depth;
    memcpy(r.p0, p, sizeof(p));
    r.found = true;
    if (!((dist > 0 ? dist : -dist) > kCylEps)) {
        // on the axis: up, and the record keeps its frame flag
        r.n[0] = 0; r.n[1] = 1; r.n[2] = 0;
        return true;
    }
    const float radial[3] = { p[0], 0.0f, p[2] };
    Normalize(radial, r.n);
    r.local = true;
    return true;
}

// 0x5b54f0: the cap facing the box's centre (in L).
bool CylCapAxis(const float* L, const Box& B, const Cyl& C, Sat& r)
{
    const float cy = L[13];
    float sign, s;
    if (cy < 0) {
        sign = -1.0f;
        s = cy;
    } else {
        sign = 1.0f;
        s = cy * -1.0f;
    }
    s = s + C.h;
    float p[3] = { L[12], L[13], L[14] };
    for (int j = 0; j < 3; ++j) {
        float e[3];
        for (int k = 0; k < 3; ++k) e[k] = L[4 * j + k] * B.half[j];
        if (e[1] < 0) {
            s = s - e[1];
            for (int k = 0; k < 3; ++k) p[k] = e[k] + p[k];
        } else {
            s = s + e[1];
            for (int k = 0; k < 3; ++k) p[k] = p[k] - e[k];
        }
    }
    if (s < 0) return false;
    if (!(s < r.best)) return true;
    r.best = s;
    memcpy(r.p0, p, sizeof(p));
    r.found = true;
    r.n[0] = 0; r.n[1] = sign; r.n[2] = 0;
    r.local = true;
    return true;
}

// 0x5b6160: box edge i across the cylinder's axis (in L). The point is the
// rim point along the normal, then moved by that radius vector once more
// (the cap's half height was meant: the game adds the same vector again).
bool CylEdgeAxis(const float* L, const Box& B, const Cyl& C, int i, Sat& r)
{
    const float a[3] = { L[4 * i], L[4 * i + 1], L[4 * i + 2] };
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    float c1[3];
    Cross(up, a, c1);  // 0x407640 (b x a)
    if ((c1[2] * c1[2] + c1[1] * c1[1]) + c1[0] * c1[0] < kCrossEps) return true;
    float n[3];
    Cross(c1, a, n);
    Normalize(n, n);
    float d = (L[14] * n[2] + L[13] * n[1]) + L[12] * n[0];
    if (d < 0) {
        for (float& v : n) v = -v;
    } else {
        d = -d;
    }
    float w[3];
    Cross(up, c1, w);
    Normalize(w, w);
    for (float& v : w) v = v * C.r;
    const float f = (w[0] * n[0] + w[2] * n[2]) + w[1] * n[1];
    float q[3];
    if (f < 0) {
        d = d - f;
        for (int k = 0; k < 3; ++k) q[k] = -w[k];
    } else {
        d = f + d;
        for (int k = 0; k < 3; ++k) q[k] = w[k];
    }
    const float yh[3] = { 0.0f * C.h, 1.0f * C.h, 0.0f * C.h };
    const float g = (yh[0] * n[0] + yh[2] * n[2]) + yh[1] * n[1];
    if (g < 0) {
        d = d - g;
        for (int k = 0; k < 3; ++k) q[k] = q[k] - w[k];
    } else {
        d = d + g;
        for (int k = 0; k < 3; ++k) q[k] = w[k] + q[k];
    }
    for (int j = 0; j < 3; ++j) {
        if (j == i) continue;
        float e[3];
        for (int k = 0; k < 3; ++k) e[k] = B.half[j] * L[4 * j + k];
        const float fj = (e[0] * n[0] + e[2] * n[2]) + e[1] * n[1];
        d = fj < 0 ? d - fj : d + fj;
    }
    if (d < 0) return false;
    if (!(d < r.best)) return true;
    r.best = d;
    if (!(d > 0)) return true;
    r.found = true;
    memcpy(r.p0, q, sizeof(q));
    memcpy(r.p1, q, sizeof(q));
    memcpy(r.n, n, sizeof(n));
    r.local = true;
    return true;
}

// 0x5b5950: box edge i against the cylinder's side line facing it (in L):
// the two segments for the closest points 0x5b8ab0 (A: the side line at the
// rim, B: the box edge).
bool CylRimEdge(const float* L, const Box& B, const Cyl& C, int i, Sat& r)
{
    const float a[3] = { L[4 * i], L[4 * i + 1], L[4 * i + 2] };
    if (std::fabs(a[(i + 1) % 3]) < kCylEps && std::fabs(a[(i + 2) % 3]) < kCylEps) return true;
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    float n[3];
    Cross(up, a, n);
    if ((n[2] * n[2] + n[1] * n[1]) + n[0] * n[0] < kCrossEps) return true;
    Normalize(n, n);
    float e[3];
    for (int k = 0; k < 3; ++k) e[k] = a[k] * B.half[i];
    const float pc[3] = { L[12], L[13], L[14] };
    float d = (n[2] * pc[2] + n[1] * pc[1]) + n[0] * pc[0];
    if (d < 0) {
        for (float& v : n) v = -v;
    } else {
        d = -d;
    }
    d = d + C.r;
    float p[3] = { pc[0], pc[1], pc[2] };
    for (int j = 0; j < 3; ++j) {
        if (j == i) continue;
        float ej[3];
        for (int k = 0; k < 3; ++k) ej[k] = B.half[j] * L[4 * j + k];
        const float f = (ej[0] * n[0] + ej[2] * n[2]) + ej[1] * n[1];
        if (f < 0) {
            d = d - f;
            for (int k = 0; k < 3; ++k) p[k] = ej[k] + p[k];
        } else {
            d = d + f;
            for (int k = 0; k < 3; ++k) p[k] = p[k] - ej[k];
        }
    }
    if (d < 0) return false;
    if (!(d < r.best)) return true;
    r.best = d;
    if (!(d > 0)) return true;
    r.found = true;
    float rn[3];
    for (int k = 0; k < 3; ++k) rn[k] = n[k] * C.r;
    r.a0[0] = rn[0]; r.a0[1] = C.h; r.a0[2] = rn[2];
    r.a1[0] = rn[0]; r.a1[1] = -C.h; r.a1[2] = rn[2];
    for (int k = 0; k < 3; ++k) {
        r.b0[k] = e[k] + p[k];
        r.b1[k] = p[k] - e[k];
    }
    memcpy(r.p0, rn, sizeof(rn));
    memcpy(r.p1, p, sizeof(p));
    memcpy(r.n, n, sizeof(n));
    r.local = true;
    return true;
}

// 0x5b4120: the box's matrix in the cylinder's frame: the cylinder's
// rotation transposed (its column 3 kept), the offset -(r2 z + r1 y) + r0 x
// per column, then box x that (0x66fc51).
void CylFrame(const float* box, const float* cyl, float L[16])
{
    float inv[16];
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) inv[i * 4 + k] = cyl[k * 4 + i];
    for (int i = 0; i < 3; ++i) inv[i * 4 + 3] = cyl[i * 4 + 3];
    const float px = cyl[12], py = cyl[13], pz = cyl[14];
    for (int k = 0; k < 3; ++k) inv[12 + k] = -((inv[8 + k] * pz + inv[4 + k] * py) + inv[k] * px);
    inv[15] = 1.0f;
    Articulated::MatMul(box, inv, L);
}

// 0x605119 for (v, 1): (x m0 + y m1) + (z m2 + m3).
void CylToWorld(const float v[3], const float* m, float out[3])
{
    float o[3];
    for (int k = 0; k < 3; ++k) o[k] = (v[0] * m[k] + v[1] * m[4 + k]) + (v[2] * m[8 + k] + 1.0f * m[12 + k]);
    memcpy(out, o, sizeof(o));
}

// 0x5b4380 / 0x5b49b0: box against cylinder (axis row 1 of its matrix). The
// box's faces, the radial direction, the caps, the box's edges across the
// axis, then its edges against the side lines (their closest points when
// one was the least deep). Results in the cylinder's frame go back to the
// world; the normal (from the cylinder to the box) as a point, less the
// cylinder's centre.
bool BoxCylinder(const Box& B, const float* boxWorld, float radius, float halfHeight, const float* cylWorld,
                 BodyContact& c)
{
    Cyl C;
    for (int k = 0; k < 3; ++k) {
        C.axis[k] = cylWorld[4 + k];
        C.c[k] = cylWorld[12 + k];
        C.ah[k] = C.axis[k] * halfHeight;
    }
    C.r = radius;
    C.h = halfHeight;
    float L[16];
    CylFrame(boxWorld, cylWorld, L);
    Sat r;
    for (int i = 0; i < 3; ++i)
        if (!CylFaceAxis(B, C, i, r)) return false;
    if (!CylRadialAxis(L, B, C, r)) return false;
    if (!CylCapAxis(L, B, C, r)) return false;
    for (int i = 0; i < 3; ++i)
        if (!CylEdgeAxis(L, B, C, i, r)) return false;
    Sat t;
    t.best = r.found ? r.best : 1000000.0f;
    bool edges = false;
    for (int i = 0; i < 3; ++i) {
        if (!CylRimEdge(L, B, C, i, t)) return false;
        if (t.found) edges = true;
    }
    if (edges && EdgeClosest(t, r)) {
        memcpy(r.n, t.n, sizeof(r.n));
        r.edge = true;
        r.local = true;
    }
    if (!edges && !r.found) return false;
    c.depth = r.best;
    if (!r.local) {
        for (int k = 0; k < 3; ++k) {
            c.normal[k] = r.n[k];
            c.pointA[k] = c.pointB[k] = r.p0[k];
        }
        return true;
    }
    float n[3];
    CylToWorld(r.n, cylWorld, n);
    for (int k = 0; k < 3; ++k) c.normal[k] = n[k] - C.c[k];
    CylToWorld(r.p0, cylWorld, c.pointA);
    if (r.edge)
        CylToWorld(r.p1, cylWorld, c.pointB);
    else
        memcpy(c.pointB, c.pointA, sizeof(c.pointA));
    return true;
}

// Cylinder / cylinder (0x5b9f00 -> 0x5ba160), all in the world. The tests
// run on two records: R (the axes' cross direction, then the edge closest
// points) and T (the caps and the sides, each cylinder against the other).

// 0x5ba340: the direction across both axes (ahA x ahB). Parallel axes only
// separate when the axes lie further apart than the radii; otherwise the
// rims facing each other give R its two side lines (for 0x5b8ab0).
bool CylCrossAxis(const Cyl& A, const Cyl& B, Sat& r)
{
    float n[3];
    Cross(A.ah, B.ah, n);
    if (!((n[2] * n[2] + n[1] * n[1]) + n[0] * n[0] >= kCrossEps)) {
        const float ta = (A.c[1] * A.axis[1] + A.c[2] * A.axis[2]) + A.axis[0] * A.c[0];
        const float tb = (B.c[1] * B.axis[1] + B.c[2] * B.axis[2]) + B.axis[0] * B.c[0];
        float d[3];
        for (int k = 0; k < 3; ++k) d[k] = (A.c[k] - ta * A.axis[k]) - (B.c[k] - tb * B.axis[k]);
        const float dist = std::sqrt((d[0] * d[0] + d[2] * d[2]) + d[1] * d[1]);
        return A.r + B.r > dist;
    }
    Normalize(n, n);
    const float d[3] = { B.c[0] - A.c[0], B.c[1] - A.c[1], B.c[2] - A.c[2] };
    float s = (d[2] * n[2] + d[1] * n[1]) + d[0] * n[0];
    if (s >= 0) {
        s = -s;
    } else {
        for (float& v : n) v = -v;
    }
    s = (s + A.r) + B.r;
    if (s < 0) return false;
    if (!(s < r.best)) return true;
    r.best = s;
    r.found = r.edge = true;
    float pa[3], pb[3];
    for (int k = 0; k < 3; ++k) {
        pa[k] = n[k] * A.r + A.c[k];
        pb[k] = B.c[k] - n[k] * B.r;
    }
    for (int k = 0; k < 3; ++k) {
        r.a0[k] = A.ah[k] + pa[k];
        r.a1[k] = pa[k] - A.ah[k];
        r.b0[k] = B.ah[k] + pb[k];
        r.b1[k] = pb[k] - B.ah[k];
    }
    memcpy(r.p0, pa, sizeof(pa));
    memcpy(r.p1, pb, sizeof(pb));
    memcpy(r.n, n, sizeof(n));
    return true;
}

// 0x5ba810: Y's cap facing X against X's point deepest along Y's axis (the
// end of X's axis, then its rim). The normal is Y's axis towards X, turned
// when X is B (flip) so that it points from B to A.
bool CylCapTest(const Cyl& X, const Cyl& Y, bool flip, Sat& r)
{
    float d[3] = { X.c[0] - Y.c[0], X.c[1] - Y.c[1], X.c[2] - Y.c[2] };
    float n[3] = { Y.axis[0], Y.axis[1], Y.axis[2] };
    float q[3] = { X.c[0], X.c[1], X.c[2] };
    float s = (d[2] * n[2] + d[1] * n[1]) + d[0] * n[0];
    if (s >= 0) {
        s = -s;
    } else {
        for (float& v : n) v = -v;
    }
    s = s + Y.h;
    const float e = (n[2] * X.ah[2] + n[1] * X.ah[1]) + n[0] * X.ah[0];
    if (e >= 0) {
        s = e + s;
        for (int k = 0; k < 3; ++k) q[k] = q[k] - X.ah[k];
    } else {
        s = s - e;
        for (int k = 0; k < 3; ++k) q[k] = q[k] + X.ah[k];
    }
    // X's rim vector furthest along n
    float w[3];
    const float f = -((n[0] * X.axis[0] + n[2] * X.axis[2]) + n[1] * X.axis[1]);
    if (1.0f - std::fabs(f) > kCylEps) {
        for (int k = 0; k < 3; ++k) w[k] = f * X.axis[k] + n[k];
        Normalize(w, w);
        for (float& v : w) v = v * X.r;
    } else {
        // n along X's axis: the rim towards Y's centre line
        const float t = (d[2] * X.axis[2] + d[1] * X.axis[1]) + d[0] * X.axis[0];
        for (int k = 0; k < 3; ++k) w[k] = d[k] - t * X.axis[k];
        if (!((w[2] * w[2] + w[1] * w[1]) + w[0] * w[0] >= kCrossEps)) {
            w[0] = w[1] = w[2] = 0.0f;
        } else {
            Normalize(w, w);
            for (float& v : w) v = v * X.r;
        }
    }
    const float g = (n[0] * w[0] + w[2] * n[2]) + w[1] * n[1];
    if (g < 0) {
        s = s - g;
        for (int k = 0; k < 3; ++k) q[k] = w[k] + q[k];
    } else {
        s = g + s;
        for (int k = 0; k < 3; ++k) q[k] = q[k] - w[k];
    }
    if (s < 0) return false;
    if (!(s < r.best)) return true;
    r.best = s;
    for (int k = 0; k < 3; ++k) r.n[k] = flip ? -n[k] : n[k];
    memcpy(r.p0, q, sizeof(q));
    r.edge = false;
    r.found = true;
    return true;
}

// 0x5bae10: Y's side facing X against X's point deepest towards Y's axis.
// Skipped when X's centre lies in Y's middle plane. The depth kept is that
// point's distance from Y's axis (taken only when inside Y's radius), not
// the separation.
bool CylSideTest(const Cyl& X, const Cyl& Y, bool flip, Sat& r)
{
    const float d[3] = { X.c[0] - Y.c[0], X.c[1] - Y.c[1], X.c[2] - Y.c[2] };
    float q[3] = { X.c[0], X.c[1], X.c[2] };
    const float t = (d[0] * Y.axis[0] + d[2] * Y.axis[2]) + d[1] * Y.axis[1];
    float pa[3], u[3];
    for (int k = 0; k < 3; ++k) {
        pa[k] = t * Y.axis[k];
        u[k] = d[k] - pa[k];
    }
    if (!((pa[2] * pa[2] + pa[1] * pa[1]) + pa[0] * pa[0] >= kCrossEps)) return true;
    if ((u[2] * u[2] + u[1] * u[1]) + u[0] * u[0] >= kCrossEps) Normalize(u, u);
    float s = (d[0] * u[0] + u[2] * d[2]) + u[1] * d[1];
    if (s >= 0) {
        s = -s;
    } else {
        for (float& v : u) v = -v;
    }
    s = s + Y.r;
    const float e = (u[2] * X.ah[2] + u[1] * X.ah[1]) + u[0] * X.ah[0];
    if (e >= 0) {
        s = e + s;
        for (int k = 0; k < 3; ++k) q[k] = q[k] - X.ah[k];
    } else {
        s = s - e;
        for (int k = 0; k < 3; ++k) q[k] = q[k] + X.ah[k];
    }
    // X's rim vector away from its axis, in the direction of d
    const float tx = (d[2] * X.axis[2] + d[1] * X.axis[1]) + d[0] * X.axis[0];
    float w[3];
    for (int k = 0; k < 3; ++k) w[k] = d[k] - tx * X.axis[k];
    if ((w[2] * w[2] + w[1] * w[1]) + w[0] * w[0] >= kCrossEps) {
        Normalize(w, w);
        for (float& v : w) v = v * X.r;
    } else {
        w[0] = w[1] = w[2] = 0.0f;
    }
    const float g = (w[2] * u[2] + w[1] * u[1]) + w[0] * u[0];
    if (g >= 0) {
        s = g + s;
        for (int k = 0; k < 3; ++k) q[k] = q[k] - w[k];
    } else {
        s = s - g;
        for (int k = 0; k < 3; ++k) q[k] = w[k] + q[k];
    }
    if (s < 0) return false;
    float v[3];
    for (int k = 0; k < 3; ++k) v[k] = q[k] - Y.c[k];
    const float ty = (v[0] * Y.axis[0] + v[2] * Y.axis[2]) + v[1] * Y.axis[1];
    for (int k = 0; k < 3; ++k) v[k] = v[k] - ty * Y.axis[k];
    const float dist = std::sqrt((v[2] * v[2] + v[1] * v[1]) + v[0] * v[0]);
    if (!(dist <= Y.r)) return true;
    if (!(dist < r.best)) return true;
    r.best = dist;
    for (int k = 0; k < 3; ++k) r.n[k] = flip ? -u[k] : u[k];
    memcpy(r.p0, q, sizeof(q));
    r.edge = false;
    r.found = true;
    return true;
}

// The cylinder record of 0x5b9f00: axis = matrix row 1, centre row 3.
void MakeCyl(const float size[3], const float* world, Cyl& C)
{
    for (int k = 0; k < 3; ++k) {
        C.axis[k] = world[4 + k];
        C.c[k] = world[12 + k];
        C.ah[k] = C.axis[k] * size[1];
    }
    C.r = size[0];
    C.h = size[1];
}

// 0x5b9f00 / 0x5ba160: cylinder A against cylinder B, the normal from B to
// A. R takes the cross direction; T (a copy started before it) the caps and
// sides; the smaller wins. When R stays an edge contact, its normal is turned
// and 0x5b8ab0 gives the closest points of the two side lines; when they
// meet outside, T's answer is taken if it has one, else there is no contact.
bool CylinderCylinder(const Cyl& A, const Cyl& B, BodyContact& c)
{
    Sat r, t;
    if (!CylCrossAxis(A, B, r)) return false;
    if (!CylCapTest(B, A, true, t)) return false;
    if (!CylCapTest(A, B, false, t)) return false;
    if (!CylSideTest(B, A, true, t)) return false;
    if (!CylSideTest(A, B, false, t)) return false;
    if (!(r.best <= t.best)) r = t;
    if (r.edge) {
        for (float& v : r.n) v = -v;
        if (!EdgeClosest(r, r)) {
            if (!t.found) return false;
            r = t;
        }
    } else if (!r.found) {
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        c.normal[k] = r.n[k];
        c.pointA[k] = r.p0[k];
        c.pointB[k] = r.edge ? r.p1[k] : r.p0[k];
    }
    c.depth = r.best;
    return true;
}

// 0x5a3ba0: shape A against shape B in the world (types 0 box, 1 cylinder
// {radius, half height}, 2 sphere {radius}). Box / box, box / cylinder,
// box / sphere and cylinder / cylinder are ported; cylinder / box and
// sphere / box run those with the sides swapped (the normal turned, the
// points exchanged). Cylinder / sphere (0x5b7cf0) is not: there the cylinder
// is taken as its bounding box (r, h, r).
bool ShapePair(int typeA, const float sizeA[3], const float* worldA, int typeB, const float sizeB[3],
               const float* worldB, BodyContact& c)
{
    if (typeA == 0) {
        Box a;
        MakeBox(sizeA, worldA, a);
        if (typeB == 0) {
            Box b;
            MakeBox(sizeB, worldB, b);
            return BoxBox(a, b, c);
        }
        if (typeB == 1) return BoxCylinder(a, worldA, sizeB[0], sizeB[1], worldB, c);
        if (typeB == 2) return BoxSphere(a, sizeB[0], worldB + 12, c);
        return false;
    }
    if (typeB == 0 && (typeA == 1 || typeA == 2)) {
        if (!ShapePair(typeB, sizeB, worldB, typeA, sizeA, worldA, c)) return false;
        for (int k = 0; k < 3; ++k) {
            c.normal[k] = -c.normal[k];
            std::swap(c.pointA[k], c.pointB[k]);
        }
        return true;
    }
    if (typeA == 1 && typeB == 1) {
        Cyl a, b;
        MakeCyl(sizeA, worldA, a);
        MakeCyl(sizeB, worldB, b);
        return CylinderCylinder(a, b, c);
    }
    if (typeA == 1) {
        const float box[3] = { sizeA[0], sizeA[1], sizeA[0] };
        return ShapePair(0, box, worldA, typeB, sizeB, worldB, c);
    }
    if (typeB == 1) {
        const float box[3] = { sizeB[0], sizeB[1], sizeB[0] };
        return ShapePair(typeA, sizeA, worldA, 0, box, worldB, c);
    }
    return false;  // sphere / sphere (0x5b6fb0): no moving spheres
}

// A body's shape against a static one.
bool ShapePair(const VehicleData::Shape& A, const float* worldA, const CollisionWorld::Shape& B, BodyContact& c)
{
    return ShapePair(A.type, A.size, worldA, B.type, B.size, B.world, c);
}

// A shape's bounding box half sizes (a cylinder (r, h, r), a sphere r).
void BoundingHalf(const VehicleData::Shape& s, float h[3])
{
    h[0] = s.size[0];
    h[1] = s.type == 2 ? s.size[0] : s.size[1];
    h[2] = s.type == 0 ? s.size[2] : s.size[0];
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

// 0x5c5ed0: a body moved by m (0x41e4b0; the matrix is not rebuilt: the
// step's 0x5a9bf0 does that after the integration). Moved up, it loses the
// upward speed the climb costs (vy^2 - 20 m.y, g = 10): at or below 0 its
// vertical speed is 0 and its angular momentum is damped by 0.99.
void Shift(RigidBody& b, const float m[3])
{
    for (int k = 0; k < 3; ++k) b.pos[k] = m[k] + b.pos[k];
    if (!(m[1] > 0.0f)) return;
    const float v2 = b.vel[1] * b.vel[1] - m[1] * 20.0f;
    if (v2 > 0.0f) {
        const float s = std::sqrt(v2);
        b.vel[1] = b.vel[1] > 0.0f ? s : -s;
        return;
    }
    b.vel[1] = 0.0f;
    for (int k = 0; k < 3; ++k) b.L[k] = 0.99000001f * b.L[k];
    b.UpdateSpin();
}

// 0x5c5c50 (traced in run 21, BOOT_TO_MENU.md section 78): the depth along
// the normal, at most 0.01 long, shared by weights: an awake free body
// (+0 == 0, state 0x65) weighs its mass, anything else (a static object,
// other null) 1e16; of two awake bodies, A is held (1e16) when its +0x70 is
// 1, else B when B's is (the car against its loose panels: the panel takes
// it all). Nothing when the weights reach 2e16; a side moves only by a
// share above 1e-5.
void PushOut(RigidBody& a, RigidBody* b, const BodyContact& c)
{
    float corr[3] = { c.depth * c.normal[0], c.depth * c.normal[1], c.depth * c.normal[2] };
    const float len = std::sqrt((corr[2] * corr[2] + corr[1] * corr[1]) + corr[0] * corr[0]);
    if (len > 0.00999999978f) {
        const float s = 0.00999999978f / len;
        for (float& x : corr) x = s * x;
    }
    const bool freeA = a.state == 0x65;
    bool freeB = b && b->state == 0x65;
    float wA = 1e16f;
    if (freeA && !(freeB && a.pairClass == 1)) {
        if (freeB && b->pairClass == 1) freeB = false;
        wA = a.mass;
    }
    const float wB = freeB ? b->mass : 1e16f;
    const float sum = wB + wA;
    if (!(static_cast<double>(sum) < 2e16)) return;
    const float inv = 1.0f / sum;
    const float shareA = inv * wB, shareB = inv * wA;
    // a grouped body goes through 0x5ae5b0 instead of 0x5c5ed0: the group's
    // root (joint type 0) only moves (0x41e4b0, no vertical speed rule); a
    // jointed member is moved along its chain (not ported: hinged panels
    // have no contacts, BOOT_TO_MENU.md section 75)
    auto move = [](RigidBody& body, const float m[3]) {
        if (!body.group) {
            Shift(body, m);
            return;
        }
        for (int k = 0; k < 3; ++k) body.pos[k] = m[k] + body.pos[k];
    };
    if (shareA > 9.99999975e-06f) {
        const float m[3] = { corr[0] * shareA, corr[1] * shareA, corr[2] * shareA };
        move(a, m);
    }
    if (b && shareB > 9.99999975e-06f) {
        const float m[3] = { corr[0] * -shareB, corr[1] * -shareB, corr[2] * -shareB };
        move(*b, m);
    }
}

// The resting threshold, argument 8 of 0x5c5260 (0x5a99d0): sqrt(sqrt(|g|)
// x 0.02) x 4 for the world's gravity |g| = 10 (1.78885).
const float kRestSpeed = std::sqrt(std::sqrt(100.0f) * 0.0199999996f) * 4.0f;

float Speed(const float v[3]) { return std::sqrt((v[2] * v[2] + v[1] * v[1]) + v[0] * v[0]); }

// 0x5a9230 -> 0x5af330: a sleeping body wakes with its resting record
// cleared.
void Wake(RigidBody& b)
{
    if (b.state != 0x78) return;
    b.state = 0x65;
    b.ClearRest();
}

// 0x5af370
void ClearRestUnlessAsleep(RigidBody& b)
{
    if (b.state != 0x78) b.ClearRest();
}

// 0x5af1d0: a body resting on two supports either sleeps (|v| <= 0.5 and
// |w| <= 1: v and L zero, state 0x78) or loses half its velocity and
// angular momentum (and its record unless asleep).
void SettleOrDamp(RigidBody& b)
{
    const float sv = std::sqrt((b.vel[0] * b.vel[0] + b.vel[1] * b.vel[1]) + b.vel[2] * b.vel[2]);
    const float sw = std::sqrt((b.w[0] * b.w[0] + b.w[1] * b.w[1]) + b.w[2] * b.w[2]);
    if (!(sv > 0.5f) && !(sw > 1.0f)) {
        for (float& x : b.vel) x = 0.0f;
        Wake(b);
        for (float& x : b.L) x = 0.0f;
        b.UpdateSpin();
        Wake(b);
        b.state = 0x78;
        return;
    }
    for (int k = 0; k < 3; ++k) b.vel[k] = 0.5f * b.vel[k];
    b.vel[3] = 0.0f;
    for (int k = 0; k < 3; ++k) b.L[k] = 0.5f * b.L[k];
    b.L[3] = 0.0f;
    b.UpdateSpin();
    ClearRestUnlessAsleep(b);
}

// 0x5af3a0: a sleeping body the resting one lies on joins its first free
// neighbour slot (of three).
void NoteNeighbour(RigidBody& b, RigidBody* n)
{
    if (!n || n->state != 0x78) return;
    if (!b.restNear[0]) { b.restNear[0] = n; return; }
    if (b.restNear[0] == n) return;
    if (!b.restNear[1]) { b.restNear[1] = n; return; }
    if (b.restNear[1] == n) return;
    if (!b.restNear[2]) b.restNear[2] = n;
}

// 0x5af440 (BOOT_TO_MENU.md section 79): a slow contact at p (the contact
// point relative to the body) adds to the body's resting record unless the
// body is held (0x96), the other body moves (awake, 0x65), or p lies within
// 0.1 of the body's vertical axis. The first point is kept; the second, on
// the other side (p . p1 horizontally < 0.01), turns both into the
// horizontal directions across them (each pointing toward the other's
// point); a later point between them (p.x d.x + p.z d.z < 0.01 for both
// directions d) settles or damps the body (SettleOrDamp), after
// restHold - which counts up from below 0 - reaches 0. Any other point
// counts a miss; after 51 misses the record is cleared.
void RestingContact(RigidBody& b, const float p[3], RigidBody* other)
{
    if (b.state == 0x96) return;
    if (other && other->state != 0x6e && other->state != 0x78 && other->state != 0x8c) return;
    if (p[0] * p[0] + p[2] * p[2] < 0.00999999978f) return;
    auto miss = [&] {
        if (b.restMiss++ > 0x32) ClearRestUnlessAsleep(b);
    };
    if (b.restCount == 0) {
        b.rest[0][0] = p[0]; b.rest[0][1] = p[1]; b.rest[0][2] = p[2]; b.rest[0][3] = 0.0f;
    } else if (b.restCount == 1) {
        const float* n = b.rest[0];
        if (!(p[2] * n[2] + n[0] * p[0] < 0.00999999978f)) { miss(); return; }
        float t1[3] = { n[2], 0.0f, -n[0] }, t2[3] = { p[2], 0.0f, -p[0] };
        if ((t1[1] * p[1] + t1[2] * p[2]) + t1[0] * p[0] < 0.0f)
            for (float& x : t1) x = -x;
        if ((t2[2] * n[2] + t2[1] * n[1]) + t2[0] * n[0] < 0.0f)
            for (float& x : t2) x = -x;
        for (int k = 0; k < 3; ++k) { b.rest[0][k] = t1[k]; b.rest[1][k] = t2[k]; }
        b.rest[0][3] = b.rest[1][3] = 0.0f;
    } else if (b.restCount == 2) {
        for (int i = 0; i < 2; ++i) {
            const float* d = b.rest[i];
            if (!(static_cast<double>(p[2] * d[2] + p[0] * d[0]) < 0.01)) { miss(); return; }
        }
        if (b.restHold < 0) {
            b.restCount = 0;
            ++b.restHold;
            return;
        }
        SettleOrDamp(b);
        if (b.restHold == 0) NoteNeighbour(b, other);
        return;
    } else {
        miss();
        return;
    }
    ++b.restCount;
    if (b.restHold == 0) NoteNeighbour(b, other);
}

// 0x5a99d0 -> 0x5c5260 for the body against a static object (other null:
// B does not move) or against a free body other (a loose panel,
// BOOT_TO_MENU.md section 76: the relative velocity A - B, K = K_A + K_B,
// -J on B, the push-out shared by the masses): true when an impulse was
// applied.
bool Respond(RigidBody& body, const BodyContact& c, float Jout[3] = nullptr, RigidBody* other = nullptr,
             Props* props = nullptr)
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
    // 0x4e9160 -> 0x4e8f70 (BOOT_TO_MENU.md section 81): a smashable prop
    // broken before, or broken by this hit (the momentum of the body, a
    // grouped one's root), drops the contact: no impulse, no push-out
    if (c.propInstance >= 0 && props) {
        const RigidBody& root = group ? *group->members[0].body : body;
        if (props->Hit(c.propInstance, Props::Momentum(root.vel, root.mass))) return false;
    }
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
    // 0x5c3110's start: a resting contact (normal speed above -kRestSpeed)
    // between light bodies (mass <= 1000, [0x78e8cc]) wakes nobody (else
    // 0x5a9230); a body still asleep then gets no impulse.
    if (!(v[2] > -kRestSpeed) || body.mass > 1000.0f || (other && other->mass > 1000.0f)) {
        Wake(body);
        if (other) Wake(*other);
    }
    // 0x5c5260 after the impulse is found (BOOT_TO_MENU.md section 79): a
    // slow contact (|relative velocity| below kRestSpeed) feeds the resting
    // record of a side that is awake, whose horizontal motion the impulse
    // opposes, that moves slower than kRestSpeed and is pushed up (A: J.y >
    // 0, B: J.y < 0); a fast one clears both records (0x5af370).
    const float vv = (v[2] * v[2] + v[1] * v[1]) + v[0] * v[0];
    if (vv < kRestSpeed * kRestSpeed) {
        const bool jointed = group && group->members[body.member].parent >= 0;
        if (!jointed && body.state == 0x65) {
            const float inv = 1.0f / body.mass;
            // a grouped root takes -1 ([0x78e904]): always opposing
            const float s = group ? -1.0f : (J[0] * inv) * body.vel[0] + (J[2] * inv) * body.vel[2];
            if (s < 0.0f && Speed(body.vel) < kRestSpeed && J[1] > 0.0f) RestingContact(body, r, other);
        }
        if (other && other->state == 0x65) {
            const float inv = -1.0f / other->mass;
            const float s = (J[0] * inv) * other->vel[0] + (J[2] * inv) * other->vel[2];
            if (s < 0.0f && Speed(other->vel) < kRestSpeed && J[1] < 0.0f) RestingContact(*other, rb, &body);
        }
    } else {
        ClearRestUnlessAsleep(body);
        if (other) ClearRestUnlessAsleep(*other);
    }
    // the impulse reaches awake bodies only (0x5c3110 gives a sleeping one
    // nothing)
    if (group) group->ContactImpulse(body.member, x1, P, false);  // 0x5a9da0, 0x5be2b0, 0x5c7390
    else if (body.state == 0x65) body.ApplyImpulse(J, ang);
    if (other && other->state == 0x65) {
        const float nj[3] = { -J[0], -J[1], -J[2] };
        float angB[3];
        Cross(rb, nj, angB);
        other->ApplyImpulse(nj, angB);
    }
    if (Jout)
        for (int k = 0; k < 3; ++k) Jout[k] = J[k];
    PushOut(body, other, c);
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

bool BoxCylinderContact(const float sizeA[3], const float* worldA, float radius, float halfHeight,
                        const float* worldB, BodyContact& c)
{
    Box a;
    MakeBox(sizeA, worldA, a);
    return BoxCylinder(a, worldA, radius, halfHeight, worldB, c);
}

bool CylinderCylinderContact(const float sizeA[3], const float* worldA, const float sizeB[3], const float* worldB,
                             BodyContact& c)
{
    Cyl a, b;
    MakeCyl(sizeA, worldA, a);
    MakeCyl(sizeB, worldB, b);
    return CylinderCylinder(a, b, c);
}

// 0x407640 (eax e, ecx c): c x e.
static void CrossCE(const float e[3], const float c[3], float out[3])
{
    const float x = e[2] * c[1] - e[1] * c[2];
    const float y = e[0] * c[2] - e[2] * c[0];
    const float z = e[1] * c[0] - e[0] * c[1];
    out[0] = x; out[1] = y; out[2] = z;
}

void CylinderGroundPoints(float r, float h, const float* m, float probe[8][3], float low[8])
{
    const float* r0 = m;
    const float* a = m + 4;
    const float* r2 = m + 8;
    const float* pos = m + 12;
    float up = a[1] - 1.0f;
    if (!(up > 0.0f)) up = -up;
    if (up < 9.99999975e-06f) {
        // 0x5b2a20: standing up: the top rim's eight points, the depth from
        // the bottom rim's (y - 2h)
        float X[3], Z[3], s[3], D1[3], D2[3];
        for (int k = 0; k < 3; ++k) X[k] = r * r0[k], Z[k] = r * r2[k];
        for (int k = 0; k < 3; ++k) s[k] = Z[k] + X[k];
        Normalize(s, D1);
        for (float& v : D1) v = r * v;
        for (int k = 0; k < 3; ++k) s[k] = Z[k] - X[k];
        Normalize(s, D2);
        for (float& v : D2) v = r * v;
        const float top = pos[1] + h;
        const float* d[4] = { X, Z, D1, D2 };
        for (int i = 0; i < 4; ++i) {
            float* p = probe[2 * i];
            float* q = probe[2 * i + 1];
            p[0] = d[i][0] + pos[0]; p[1] = d[i][1] + top; p[2] = pos[2] + d[i][2];
            q[0] = pos[0] - d[i][0]; q[1] = top - d[i][1]; q[2] = pos[2] - d[i][2];
        }
        const float twoH = h + h;
        for (int i = 0; i < 8; ++i) low[i] = probe[i][1] - twoH;
        return;
    }
    // 0x5b1f00: L the lower end's centre, U the upper one's
    float A[3], L[3], U[3];
    for (int k = 0; k < 3; ++k) A[k] = h * a[k];
    for (int k = 0; k < 3; ++k) {
        const float top = A[k] + pos[k], bot = pos[k] - A[k];
        L[k] = a[1] > 0.0f ? bot : top;
        U[k] = a[1] > 0.0f ? top : bot;
    }
    float ay = a[1];
    if (!(ay > 0.0f)) ay = -ay;
    if (ay > 0.899999976f) {
        // steep: the lower rim's eight points (0, 90, 45 and 135 degrees)
        float X[3], Z[3], s[3], D1[3], D2[3];
        for (int k = 0; k < 3; ++k) X[k] = r * r0[k], Z[k] = r * r2[k];
        for (int k = 0; k < 3; ++k) s[k] = Z[k] + X[k];
        Normalize(s, D1);
        for (float& v : D1) v = r * v;
        for (int k = 0; k < 3; ++k) s[k] = Z[k] - X[k];
        Normalize(s, D2);
        for (float& v : D2) v = r * v;
        for (int k = 0; k < 3; ++k) {
            probe[0][k] = L[k] + X[k];
            probe[1][k] = L[k] - X[k];
            probe[2][k] = L[k] + Z[k];
            probe[3][k] = L[k] - Z[k];
            probe[4][k] = D1[k] + L[k];
            probe[5][k] = L[k] - D1[k];
            probe[6][k] = D2[k] + L[k];
            probe[7][k] = L[k] - D2[k];
        }
    } else {
        // lying: both rims at +-P (level, across the axis) and +-Q (P x
        // the axis, the side facing up and down)
        const float Y[3] = { 0.0f, 1.0f, 0.0f };
        float P[3], Q[3];
        CrossCE(Y, a, P);
        CrossCE(P, a, Q);
        for (float& v : P) v = r * v;
        for (float& v : Q) v = r * v;
        for (int k = 0; k < 3; ++k) {
            probe[0][k] = L[k] + P[k];
            probe[1][k] = L[k] - P[k];
            probe[2][k] = L[k] + Q[k];
            probe[3][k] = L[k] - Q[k];
            probe[4][k] = U[k] + P[k];
            probe[5][k] = U[k] - P[k];
            probe[6][k] = U[k] + Q[k];
            probe[7][k] = U[k] - Q[k];
        }
    }
    for (int i = 0; i < 8; ++i) low[i] = probe[i][1];
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
    float bw[16], bh[3];
    shapeWorld(0, bw);
    BoundingHalf(shapes[0], bh);
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        const float e = std::fabs(bw[k]) * bh[0] + std::fabs(bw[4 + k]) * bh[1] + std::fabs(bw[8 + k]) * bh[2];
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
        if (!ShapePair(shapes[0], bw, world.ShapeAt(ob.first), best)) continue;
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
                    if (!ShapePair(shapes[i], sw, sb, c)) continue;
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
        // 0x4e9160: the vehicle's surface against a static object (its +4
        // is 0 in the traces, not -1: never 0x1c)
        if (vehicle && !(best.surfaceA == 0xb && best.surfaceB == 0xb))
            best.surfaceA = VehicleSurface(best.surfaceA, 0, best.pointA, &body.matrix[0][0], vehicleBottom,
                                           best.normal, false);
        best.propInstance = ob.prop ? ob.instance : -1;
        float J[3];
        if (Respond(body, best, J, nullptr, props)) {
            ++contacts;
            if (onImpulse) onImpulse(best, J);
        }
        bodyWorld = &body.matrix[0][0];
    }
    // 0x5a9b80 -> 0x5b36f0 -> 0x5b3570: the ground under eight points of
    // each shape (a box's corners 0x5b2960; a cylinder's rim points 0x5b3010,
    // CylinderGroundPoints), the deepest one below. The contact takes the
    // surface of the last shape tested once one was found (0x5b3570 stamps
    // the best record with every shape that returns 5).
    if (groundData) {
        BodyContact g;
        bool found = false;
        for (size_t s = first; s < shapes.size(); ++s) {
            float sw[16];
            shapeWorld(s, sw);
            float probe[8][3], low[8];
            if (shapes[s].type == 1) {
                CylinderGroundPoints(shapes[s].size[0], shapes[s].size[1], sw, probe, low);
            } else {
                float sh[3];
                BoundingHalf(shapes[s], sh);
                Box b;
                MakeBox(sh, sw, b);
                for (int k = 0; k < 8; ++k) {
                    for (int m = 0; m < 3; ++m)
                        probe[k][m] = b.c[m] + ((k & 1) ? b.sa[0][m] : -b.sa[0][m]) +
                                      ((k & 2) ? b.sa[1][m] : -b.sa[1][m]) + ((k & 4) ? b.sa[2][m] : -b.sa[2][m]);
                    low[k] = probe[k][1];
                }
            }
            for (int k = 0; k < 8; ++k) {
                const float* p = probe[k];
                float h, n[3];
                int surf = 0;
                if (!groundData->Probe(p[0], p[1], p[2], h, n, &surf)) continue;
                const float depth = h - low[k];
                if (depth > (found ? g.depth : 0.0f)) {
                    const float l = std::sqrt(Dot(n, n));
                    if (!(l > 0)) continue;
                    found = true;
                    g.depth = depth;
                    for (int m = 0; m < 3; ++m) g.normal[m] = n[m] / l;
                    g.pointA[0] = p[0]; g.pointA[1] = low[k]; g.pointA[2] = p[2];
                    g.pointB[0] = p[0]; g.pointB[1] = h; g.pointB[2] = p[2];
                    g.surfaceB = surf;
                }
            }
            if (found) g.surfaceA = shapes[s].surface;
        }
        if (found && g.depth > 9.99999975e-06f) {
            if (g.depth > deepest) deepest = g.depth;
            // the ground is a body too (+4 = -1, identity matrix, no shapes):
            // the vehicle's surface is renamed against it (0x1c on its
            // bottom) and the listener hears the impulse (runs 16..20)
            if (vehicle && !(g.surfaceA == 0xb && g.surfaceB == 0xb))
                g.surfaceA = VehicleSurface(g.surfaceA, 0, g.pointA, &body.matrix[0][0], vehicleBottom, g.normal, true);
            float J[3];
            if (Respond(body, g, J)) {
                ++ground;
                if (onImpulse) onImpulse(g, J);
            }
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
    if (!ShapePair(shapesA[0].type, shapesA[0].size, wa, shapesB[0].type, shapesB[0].size, wb, best)) return 0;
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
                if (!ShapePair(shapesA[i].type, shapesA[i].size, wa, shapesB[j].type, shapesB[j].size, wb, c)) continue;
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
    // 0x4e9160: the vehicle's surface against a body (B is not static)
    if (vehicle && !(best.surfaceA == 0xb && best.surfaceB == 0xb))
        best.surfaceA = VehicleSurface(best.surfaceA, 0, best.pointA, &a.matrix[0][0], vehicleBottom, best.normal,
                                       false);
    float J[3];
    if (!Respond(a, best, J, &b)) return 0;
    if (onImpulse) onImpulse(best, J);
    return 1;
}

int VehicleSurface(int surface, int side, const float point[3], const float vehicle[16], float bottom,
                   const float normal[3], bool otherStatic)
{
    if (surface != 0xb) return surface;
    const float h = bottom + 0.0250000004f;  // [0x78ea80]
    float inv[16], local[4];
    InverseRows(vehicle, false, inv);
    const float p[4] = { point[0], point[1], point[2], 1.0f };
    Vec4Transform(p, inv, local);
    if (!(local[1] < h)) return 0x18;
    const float f = side == 0 ? 1.0f : -1.0f;  // [0x78e860] / [0x78e904]
    if (otherStatic && 0.924000025f < normal[1] * f) return 0x1c;  // [0x78f244]
    return surface;
}

float VehicleBottom(const VehicleData::Shape& b)
{
    const float e = std::fabs(b.local[1]) * b.size[0] + std::fabs(b.local[5]) * b.size[1] +
                    std::fabs(b.local[9]) * b.size[2];
    return b.local[13] + -e;
}

bool PairResponse(RigidBody& a, RigidBody* b, const BodyContact& c, float J[3])
{
    return Respond(a, c, J, b);
}
