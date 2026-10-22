// The damage panels (panels.h). Every function follows the game's routine
// in its operation order (worked out with driv3r-re\tools\physics\symx87.py,
// checked by tests\groupreplay.cpp against run 12 / 14's dumps of the
// creation, D3PROXY_PARTS); the comments give the address.
#include "panels.h"

#include <cmath>
#include <cstring>

namespace
{
// 0x407640 (a x b in the game's naming), w 0.
void Cross(const float a[], const float b[], float out[4])
{
    const float z = a[1] * b[0] - a[0] * b[1];
    const float y = a[0] * b[2] - a[2] * b[0];
    const float x = a[2] * b[1] - a[1] * b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = 0;
}
}  // namespace

void Vec4Transform(const float v[4], const float m[16], float out[4])
{
    float o[4];
    for (int j = 0; j < 4; ++j) o[j] = (v[0] * m[j] + v[1] * m[4 + j]) + (v[2] * m[8 + j] + v[3] * m[12 + j]);
    memcpy(out, o, sizeof(o));
}

void InverseRows(const float m[16], bool thirdXzy, float out[16])
{
    const float* p = m + 12;
    float t[3];
    t[0] = -1.0f * ((m[2] * p[2] + m[1] * p[1]) + m[0] * p[0]);
    t[1] = -1.0f * ((m[6] * p[2] + m[5] * p[1]) + m[4] * p[0]);
    t[2] = thirdXzy ? -1.0f * ((m[8] * p[0] + m[10] * p[2]) + m[9] * p[1])
                    : -1.0f * ((m[10] * p[2] + m[9] * p[1]) + m[8] * p[0]);
    const float o[16] = { m[0], m[4], m[8], m[3], m[1], m[5], m[9], m[7], m[2], m[6], m[10], m[11], t[0], t[1], t[2], 1.0f };
    memcpy(out, o, sizeof(o));
}

namespace
{
// 0x41e990: v / |v|.
void Normalize(const float v[3], float out[4])
{
    const float s = 1.0f / sqrtf((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]);
    out[0] = s * v[0]; out[1] = s * v[1]; out[2] = s * v[2]; out[3] = 0;
}
}  // namespace

void PanelBody(const VehicleData::DamageEntry& e, const float X[16], const float car[16], const RigidBody& carBody,
               RigidBody& out)
{
    // the body's matrix: inverse(part) x X x car (0x66fc51)
    float t[16], m1[16], b[16];
    InverseRows(e.matrix, true, t);
    Articulated::MatMul(t, X, m1);
    Articulated::MatMul(m1, car, b);
    float q[4];
    Articulated::QuatFromRows(reinterpret_cast<const float(*)[4]>(b), q, 9.99999944e-11f, 9.99999975e-06f);  // 0x43e1a0
    out = RigidBody();
    for (int i = 0; i < 4; ++i) out.q[i] = q[i];
    out.pos[0] = b[12]; out.pos[1] = b[13]; out.pos[2] = b[14]; out.pos[3] = 0;
    out.accel[1] = -10.0f;
    // the body's allocation (0x5a0b60) normalises q and builds the matrix;
    // 0x5a1440: mass and inertia, then q normalised again, R, invI, w, dq
    QuatNormalize(out.q, out.q);
    out.UpdateMatrix();
    out.mass = e.mass;
    for (int i = 0; i < 3; ++i) out.inertia[i] = e.inertia[i];
    out.inertia[3] = 0;
    out.UpdateDerived();
    // the car's velocity at the part (0x5a1700 / 0x5a1790): r is the
    // inverse's offset turned by the car (0x66cad5 on the car's rows), only
    // its part across the spin counts
    const float* w = carBody.w;
    float v[4] = { carBody.vel[0], carBody.vel[1], carBody.vel[2], 0.0f };
    if (!((w[2] * w[2] + w[1] * w[1]) + w[0] * w[0] < 9.99999944e-11f)) {  // 0x43e890
        const float rot[16] = { car[0], car[1], car[2], car[3], car[4], car[5], car[6], car[7],
                                car[8], car[9], car[10], car[11], 0, 0, 0, 1 };
        const float off[4] = { t[12], t[13], t[14], 1.0f };
        float r[4], n[4], rp[4], c[4];
        Vec4Transform(off, rot, r);
        Normalize(w, n);
        const float d = (r[0] * n[0] + r[2] * n[2]) + r[1] * n[1];
        for (int i = 0; i < 3; ++i) rp[i] = r[i] - d * n[i];
        Cross(rp, w, c);
        for (int i = 0; i < 3; ++i) v[i] = c[i] + v[i];
    }
    for (int i = 0; i < 3; ++i) out.vel[i] = v[i];  // 0x5a0f40
    out.vel[3] = 0;
    // 0x5a1000: L = w (invI)^-1 (cofactors over the determinant), then w
    // and dq again (0x5aee00)
    const float (*I)[4] = out.invI;
    const float c01 = I[1][0] * I[0][2] - I[1][2] * I[0][0];
    const float c22 = I[1][1] * I[0][0] - I[1][0] * I[0][1];
    const float c02 = I[1][2] * I[0][1] - I[1][1] * I[0][2];
    const float s = 1.0f / ((c01 * I[2][1] + c22 * I[2][2]) + c02 * I[2][0]);
    const float C[3][3] = {
        { (I[2][2] * I[1][1] - I[2][1] * I[1][2]) * s, (I[0][2] * I[2][1] - I[0][1] * I[2][2]) * s, c02 * s },
        { (I[2][0] * I[1][2] - I[2][2] * I[1][0]) * s, (I[0][0] * I[2][2] - I[0][2] * I[2][0]) * s, c01 * s },
        { (I[2][1] * I[1][0] - I[2][0] * I[1][1]) * s, (I[0][1] * I[2][0] - I[0][0] * I[2][1]) * s, c22 * s },
    };
    for (int j = 0; j < 3; ++j) out.L[j] = w[2] * C[2][j] + (w[1] * C[1][j] + w[0] * C[0][j]);
    out.L[3] = 0;
    out.UpdateSpin();
}

bool PanelHingeInputs(const VehicleData& v, const VehicleData::Part& p, const float X[16], float damage4,
                      float damage8, float width, PanelHinge& out)
{
    if (p.hardpoint < 0 || p.hardpoint >= int(v.hardpoints.size()) || p.record < 0 ||
        p.record >= int(v.damage.size()))
        return false;
    // the direction the reference axis comes from (0x4f46f0)
    float d[3] = { 0.0f, 0.0f, 0.0f };
    switch (p.id) {
    case 0x04: case 0x23: case 0x24: case 0x3b: d[1] = 1.0f; break;
    case 0x05: d[1] = -1.0f; break;
    case 0x1f: case 0x34: case 0x36: case 0x38: d[0] = 1.0f; break;
    case 0x20: case 0x35: case 0x37: case 0x39: case 0x3a: d[0] = -1.0f; break;
    default: return false;
    }
    const VehicleData::Hardpoint& hp = v.hardpoints[p.hardpoint];
    float P[3] = { hp.pos[0], hp.pos[1], hp.pos[2] };
    float A[3] = { hp.axis[0], hp.axis[1], hp.axis[2] };
    if (p.type == 1 && (p.id == 0x23 || p.id == 0x24)) {  // 0x4f48f0: the bumpers
        const float sign = damage4 > damage8 ? 1.0f : -1.0f;
        for (float& a : A) a = sign * a;
        P[0] = (sign * width) * 0.4375f + P[0];  // 0x78f110
    }
    for (float& c : P) c = 0.0f + c;
    // the part's frame: inverse(X) x the part's matrix (0x66fc51); axes
    // through it without its offset
    float tx[16], m[16];
    InverseRows(X, false, tx);
    Articulated::MatMul(tx, v.damage[p.record].matrix, m);
    float mr[16];
    memcpy(mr, m, sizeof(mr));
    mr[12] = mr[13] = mr[14] = 0;
    mr[15] = 1.0f;
    const float p4[4] = { P[0], P[1], P[2], 1.0f }, a4[4] = { A[0], A[1], A[2], 1.0f };
    float cp[4], cax[4], wv[4], pref[4], wp[4];
    Vec4Transform(p4, m, cp);
    Vec4Transform(a4, mr, cax);
    Cross(d, A, wv);
    Cross(A, wv, pref);
    const float r4[4] = { pref[0], pref[1], pref[2], 1.0f };
    Vec4Transform(r4, mr, wp);
    // turned by -pi/4 about the part's axis: the quaternion (0x41e8d0, the
    // game's sine and cosine) as rows (0x49c020)
    const float half = -0.785398185f * 0.5f;
    const float n = 1.0f / sqrtf((cax[0] * cax[0] + cax[1] * cax[1]) + cax[2] * cax[2]);
    const float sn = Articulated::Sin(half), cs = Articulated::Cos(half);
    const float q[4] = { (n * cax[0]) * sn, (n * cax[1]) * sn, (n * cax[2]) * sn, cs };
    float rows[3][4];
    QuatToRows(q, rows);
    const float rq[16] = { rows[0][0], rows[0][1], rows[0][2], 0, rows[1][0], rows[1][1], rows[1][2], 0,
                           rows[2][0], rows[2][1], rows[2][2], 0, 0, 0, 0, 1 };
    const float w4[4] = { wp[0], wp[1], wp[2], 1.0f };
    float cref[4];
    Vec4Transform(w4, rq, cref);
    for (int i = 0; i < 3; ++i) {
        out.cp[i] = cp[i];
        out.cax[i] = cax[i];
        out.cref[i] = cref[i];
        out.pp[i] = P[i];
        out.pax[i] = A[i];
        out.pref[i] = pref[i];
    }
    return true;
}

void PanelJoint(Articulated::Group& g, int k, int partType, int partId)
{
    float* rec = g.members[k].rec;
    rec[0x358 / 4] = -0.785398185f;
    rec[0x35c / 4] = 0.785398185f;
    g.q[k] = -0.785398185f;  // 0x5a18f0
    // 0x4f5040: spring, damper, stop gain, restitution
    float spring = 0.0f, damper = 9.0f, gain = 1.5f, e = 0.2f;
    if (partId == 4) { spring = 24.0f; damper = 1.0f; gain = 2.0f; e = 0.6f; }
    if (partId == 5) { spring = 12.0f; damper = 1.0f; gain = 0.1f; e = 0.6f; }
    rec[0x350 / 4] = spring;
    rec[0x354 / 4] = damper;
    rec[0x364 / 4] = gain;
    rec[0x360 / 4] = e;
    // the bumpers and part 4 take the impulse flag (body +0x74 = 2 too)
    if ((partType == 1 && (partId == 0x23 || partId == 0x24)) || partId == 4)
        *reinterpret_cast<int*>(rec + 0x368 / 4) = 1;
}

int HingePanel(Articulated::Group& g, int parent, RigidBody* panel, float* rec, const PanelHinge& h, int partType,
               int partId)
{
    const int k = g.AddMember(parent, panel, rec, h.cp, h.cax, h.cref, h.pp, h.pax, h.pref, 1);
    PanelJoint(g, k, partType, partId);
    return k;
}

bool CarGroup::Hinge(const VehicleData& v, const VehicleData::Part& p, RigidBody& car, const float carMatrix[16],
                     const float X[16], const float half[2], float width)
{
    if (bodies.size() >= 9) return false;  // 0x4f7ff0: slot +0x18c0 < 9
    PanelHinge h;
    if (p.record < 0 || p.record >= static_cast<int>(v.damage.size()) ||
        !PanelHingeInputs(v, p, X, half[0], half[1], width, h))
        return false;
    if (!Active()) {
        recs.clear();
        recs.emplace_back(0x3c0 / 4, 0.0f);
        group.MakeRoot(&car, recs[0].data());
    }
    bodies.emplace_back();
    RigidBody& b = bodies.back();
    PanelBody(v.damage[p.record], X, carMatrix, car, b);
    recs.emplace_back(0x3c0 / 4, 0.0f);
    HingePanel(group, 0, &b, recs.back().data(), h, p.type, p.id);
    records.push_back(p.record);
    group.Link();
    return true;
}

void CarGroup::Step(float h)
{
    if (!Active()) return;
    group.Step(h);
    for (Articulated::Group::Member& m : group.members) m.body->UpdateMatrix();  // 0x5a9bf0
}

int CarGroup::MemberOfRecord(int record) const
{
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i] != record) continue;
        for (size_t k = 0; k < group.members.size(); ++k)
            if (group.members[k].body == &bodies[i]) return static_cast<int>(k);
    }
    return -1;
}
