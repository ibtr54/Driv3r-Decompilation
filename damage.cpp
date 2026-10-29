// The vehicle's damage (damage.h). The statements follow the game's
// routines in their operation order (the FPU runs in single precision);
// checked by tests\damagereplay.cpp against run 15 (D3PROXY_DAMAGE).
#include "damage.h"

#include <cmath>
#include <cstring>

#include "articulated.h"
#include "panels.h"

namespace
{
const float kInv120 = 0.00833333377f;  // 0x78e8d4

float Abs(float x) { return x > 0 ? x : -x; }  // fcom 0 / fchs (and 0x406640)

// 0x44a820(a, b): |a - b| < 5e-6 x max(|2a|, 1).
bool Near(float a, float b)
{
    float d = a - b;
    if (d <= 0) d = -d;
    float m = a + a;
    if (m <= 0) m = -m;
    if (m < 1.0f) m = 1.0f;
    return d < 4.99999987e-06f * m;
}

// 0x442ba0: sqrt((x x + y y) + z z).
float Length(const float v[3]) { return sqrtf((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]); }

bool IsType1(int type, int id, std::initializer_list<int> ids)
{
    if (type != 1) return false;
    for (int i : ids)
        if (i == id) return true;
    return false;
}

// 0x4f4960: the parts whose hits go to the linked record.
bool Linked(int type, int id) { return IsType1(type, id, { 2, 0x21, 0x22, 0x25 }); }
// 0x4f4940
bool IsPart70(int type, int id) { return type == 1 && id == 0x70; }
// 0x4f4910: the parts that need twice the damage to hinge.
bool Stiff(int type, int id) { return type == 1 && (id == 1 || (id >= 0x1f && id <= 0x39)); }
// 0x4f48f0: the bumpers.
bool Bumper(int type, int id) { return IsType1(type, id, { 0x23, 0x24 }); }
// 0x5140d0: vehicles whose parts never reach stage 2.
bool NoSmash(int vehicleId)
{
    switch (vehicleId) {
    case 9: case 0xe: case 0x39: case 0x59: case 0x5a: case 0x5b: return true;
    default: return false;
    }
}
const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
}  // namespace

bool DamageFrontGroup(int type, int id, uint32_t flags)
{
    if (flags & 0x400000) return true;
    if (type != 1 && type != 8) return false;
    switch (id) {
    case 2: case 4: case 6: case 9: case 0xe: case 0x21: case 0x22: case 0x23: case 0x25: case 0x32: case 0x33:
    case 0x46: case 0x47: case 0x70: return true;
    default: return false;
    }
}

bool DamageBackGroup(int type, int id, uint32_t flags)
{
    if (flags & 0x800000) return true;
    if (type != 1 && type != 8) return false;
    switch (id) {
    case 5: case 10: case 0xb: case 0xc: case 0xd: case 0xf: case 0x24: case 0x27: case 0x2e: case 0x2f: case 0x30:
    case 0x31: case 0x36: case 0x37: case 0x38: case 0x39: case 0x4e: case 0x4f: case 0x71: return true;
    default: return false;
    }
}

bool DamageMayTear(int type, int id, uint32_t flags)
{
    if (type == 2) return false;
    if (IsType1(type, id, { 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x57, 0x58, 0x59 })) return false;
    if (flags & 2) return true;
    return IsType1(type, id, { 1, 2, 4, 5, 6, 7, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x28, 0x29, 0x34, 0x35, 0x36,
                               0x37, 0x38, 0x39 });
}

bool DamageMayHinge(int type, int id, uint32_t flags)
{
    if (type == 2) return false;
    if (IsType1(type, id, { 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x57, 0x58, 0x59, 0x2d, 0x54, 0x55 }))
        return false;
    if ((flags & 0x3f0000) && (flags & 1)) return true;
    return IsType1(type, id, { 4, 5, 0x1f, 0x34, 0x36, 0x38, 0x20, 0x35, 0x37, 0x39, 0x23, 0x24 });
}

void VehicleDamage::Init(const VehicleData& v, const float sz[3], const float handling[kHandlingKeys])
{
    int count = 0;
    for (const VehicleData::Part& p : v.parts)
        if (p.record >= count) count = p.record + 1;
    records.assign(count, DamageRecord());
    recordPart.assign(count, -1);
    for (size_t i = 0; i < v.parts.size(); ++i)
        if (v.parts[i].record >= 0 && recordPart[v.parts[i].record] < 0) recordPart[v.parts[i].record] = int(i);
    for (int k = 0; k < 3; ++k) size[k] = sz[k];
    reach = handling[0x8a];
    strength = handling[0x8b];
    crushScale = handling[0x25];
    // the wheels: 0x5115c0's place (offset + hardpoint, w 1), part order
    wheelScale = handling[0xbd];
    for (Wheel& w : wheels) w = Wheel();
    wheelPlaces.clear();
    const float off = WheelOffset(handling, HandlingClass(v.id));
    for (const VehicleWheel& w : v.wheels) {
        if (w.slot < 0 || w.slot > 5) continue;
        wheelPlaces.push_back({ w.slot, { 0.0f + w.hardpoint[0], off + w.hardpoint[1], 0.0f + w.hardpoint[2], 1.0f } });
    }
    wheelOff = -1;
}

// 0x507980: a hit of strength s at L (the car's frame, w 0) on the nearest
// wheel (squared distance over the four components below 4); a hit below
// 0.25 does nothing, above 1 counts 1; a wheel not gone and out of its
// cool-down adds it and comes off at 1.5 (0x506cd0). Model 0x13's check of
// [0x8ac3ac] vtbl+0x98 is not ported.
void VehicleDamage::WheelHit(const float L[4], float s)
{
    float h = (((wheelScale * hitScale) * strength) * s) * 0.00999999978f;
    if (1.0f < h) h = 1.0f;
    else if (h < 0.25f) return;
    float best = 4.0f;
    int slot = -1;
    for (const auto& wp : wheelPlaces) {
        const float* p = wp.second.data();
        const float d[4] = { p[0] - L[0], p[1] - L[1], p[2] - L[2], p[3] - L[3] };
        const float q = ((d[3] * d[3] + d[2] * d[2]) + d[1] * d[1]) + d[0] * d[0];
        if (q < best) best = q, slot = wp.first;
    }
    if (slot < 0) return;
    Wheel& w = wheels[slot];
    if (w.gone || !(w.cooldown <= 0)) return;
    w.cooldown = 0.25f;
    w.damage = h + w.damage;
    if (1.5f <= w.damage) {
        w.gone = true;  // 0x506cd0 (the loose wheel's body: section 87)
        wheelOff = slot;
    }
}

void VehicleDamage::Tick(const VehicleData& v, const float car[16], std::vector<DamageRequest>& out)
{
    // 0x4fbcc0: the wheels' cool-downs (+0x17fc + 0xc k), then the records'
    for (Wheel& w : wheels) {
        const float c = w.cooldown - kInv120;
        w.cooldown = c < 0 ? 0.0f : c;
    }
    for (size_t k = 0; k < records.size(); ++k) {
        DamageRecord& r = records[k];
        if (r.cooldown != 0) {
            const float c = r.cooldown - kInv120;
            r.cooldown = c <= 0 ? 0.0f : c;
        }
        // a hinged part of id 4 (the bonnet) held open beyond 0.687 for 4 s
        // comes off (then the game pushes the loose part back by the car's
        // velocity, 0x5a1700 / 0x5b11c0: TornPanelKick, panels.h)
        if (v.parts[recordPart[k]].id != 4 || !r.body || r.state != 2) continue;
        if (r.jointQ < tearQ) {
            openTime = 0;
            continue;
        }
        openTime = openTime + kInv120;
        if (!(tearTime <= openTime)) continue;
        openTime = 0;
        float inv[16];
        InverseRows(car, false, inv);  // (the first row's order checked in the listing, not traced)
        DamageRequest q;
        q.record = int(k);
        q.state = 3;
        Articulated::MatMul(r.bodyMatrix, inv, q.matrix);
        out.push_back(q);
    }
}

float VehicleDamage::Apply(DamageRecord& t, DamageRecord& hit, float s, float dist, float reachR, float side, float f,
                           bool own, bool addTotal)
{
    // 0x50a130 (ecx = t, edx = hit, edi = the slot's total; own and addTotal
    // are its byte arguments 6 and 7)
    const float raw = s * f * 0.00499999989f;
    const float c = 1.0f < raw ? 1.0f : raw;
    const float u = 1.0f - c;
    const float v = 1.0f - u * u * u;
    const float q = dist / (reachR * 0.666666687f);
    float g;
    if (q > 1.0f) {
        g = 2.0f - q;
        if (0.0f > g) g = 0;
    } else {
        g = 1.0f;
    }
    const float dmg = g * v;
    if (own) {
        t.half[0] = (1.0f - side) * dmg * 2.0f + t.half[0];
        t.half[1] = side * dmg * 2.0f + t.half[1];
        // clamped to 1 from the HIT record (the same one unless linked)
        t.half[0] = 1.0f < hit.half[0] ? 1.0f : hit.half[0];
        t.half[1] = 1.0f < hit.half[1] ? 1.0f : hit.half[1];
        t.impact = raw * 0.25f + t.impact;
        t.dent = dmg * 0.25f + t.dent;
    }
    if (!addTotal) return 0;
    // the slot's total when the game's damage switch ([[0x8ac444]] +0x31,
    // clear in the traces) allows it
    float sum = raw / float(records.size()) + total;
    if (1.0f < sum) sum = 1.0f;
    else if (0.0f > sum) sum = 0;
    total = sum;
    return hit.impact > raw ? hit.impact : raw;
}

void VehicleDamage::Stage(DamageRecord& r, int vehicleId)
{
    // 0x4f5210
    if (r.half[0] + r.half[1] > 0.75f && r.stage == 0) {
        if (r.flags & 2) glass = true;
        if (r.worst < 2) r.worst = 2;
        r.stage = 1;
    }
    if (r.half[0] + r.half[1] > 1.0f && r.stage == 1 && !NoSmash(vehicleId)) {
        // (bit 1: 0x50b100, the glass's break, not ported)
        if (r.worst < 3) r.worst = 3;
        r.stage = 2;
    }
}

void VehicleDamage::Share(const VehicleData& v)
{
    // 0x4fd480 (argument 1)
    float f0 = 0, f1 = 0, b0 = 0, b1 = 0;
    for (size_t k = 0; k < records.size(); ++k) {
        const VehicleData::Part& p = v.parts[recordPart[k]];
        const DamageRecord& r = records[k];
        if (DamageFrontGroup(p.type, p.id, p.flags8)) {
            if (f0 <= r.half[0]) f0 = r.half[0];
            if (f1 <= r.half[1]) f1 = r.half[1];
        }
        if (DamageBackGroup(p.type, p.id, p.flags8)) {
            if (b0 <= r.half[0]) b0 = r.half[0];
            if (b1 <= r.half[1]) b1 = r.half[1];
        }
    }
    for (size_t k = 0; k < records.size(); ++k) {
        const VehicleData::Part& p = v.parts[recordPart[k]];
        DamageRecord& r = records[k];
        if (DamageFrontGroup(p.type, p.id, p.flags8)) { r.half[0] = f0; r.half[1] = f1; }
        if (DamageBackGroup(p.type, p.id, p.flags8)) { r.half[0] = b0; r.half[1] = b1; }
    }
    front[0] = f0; front[1] = f1; back[0] = b0; back[1] = b1;
    // the dent (argument 1, slot +0x1f5d clear, the body's own shapes)
    if (dentShape < 0 || !dentShapes || dentShape >= static_cast<int>(dentShapes->size()) ||
        dentShape >= static_cast<int>(dentRest.size()))
        return;
    const VehicleData::Shape& rest = dentRest[dentShape];
    VehicleData::Shape& live = (*dentShapes)[dentShape];
    const float fr = (f1 + f0) * 0.5f * dentScale[0];
    const float bk = (b1 + b0) * 0.5f * dentScale[1];
    live.size[dentAxis] = ((rest.size[dentAxis] + rest.size[dentAxis]) - (bk + fr)) * 0.5f;
    memcpy(live.local, rest.local, sizeof(live.local));
    live.local[14] = (bk - fr) * 0.5f + rest.local[14];
}

void VehicleDamage::InitDent(int vehicleId, std::vector<VehicleData::Shape>* shapes,
                             const float handling[kHandlingKeys])
{
    // 0x5091b0
    dentShape = -1;
    dentAxis = 0;
    dentScale[0] = handling[0x26];
    dentScale[1] = handling[0x27];
    dentShapes = shapes;
    dentRest = shapes ? *shapes : std::vector<VehicleData::Shape>();
    // 0x513ec0: the vehicles that never dent
    static const unsigned char kNoDent[] = { 0x00, 0x01, 0x0f, 0x12, 0x13, 0x14, 0x17, 0x18, 0x1a, 0x1e, 0x1f, 0x20,
                                             0x24, 0x27, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x34, 0x35, 0x36,
                                             0x3a, 0x3b, 0x3c, 0x40, 0x41, 0x43, 0x44, 0x45, 0x46, 0x47, 0x49, 0x4c,
                                             0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5f,
                                             0x60 };
    for (unsigned char id : kNoDent)
        if (vehicleId == id) return;
    const size_t n = dentRest.size();
    if (n >= 7) return;
    float best = 0;
    int pick = 0;
    float h[3] = {};
    for (size_t i = n > 1 ? 1 : 0; i < n; ++i) {
        const float* s = dentRest[i].size;
        const float m = (s[2] * s[2] + s[0] * s[0]) + s[1] * s[1];  // the listing's order (z, x, y)
        if (best < m) {
            best = m;
            pick = static_cast<int>(i);
            memcpy(h, s, sizeof(h));
        }
    }
    dentShape = pick;
    if (h[1] < h[0] && h[2] < h[0]) dentAxis = 0;
    else dentAxis = h[1] <= h[2] ? 2 : 1;
}

void VehicleDamage::TearShare(int record)
{
    // 0x4f9810 (the switch [[0x8ac444]] +0x31 clear in the traces, as Apply)
    float d = 1.0f - records[record].impact;
    if (d < 0.0f) d = 0;
    d = d / static_cast<float>(records.size()) + total;
    if (1.0f < d) d = 1.0f;
    else if (d < 0.0f) d = 0;
    total = d;
}

int VehicleDamage::PartTest(const VehicleData& v, const VehicleData::Part& p, int k, const VehicleData::DamageEntry& e,
                            float s, const float D[4], float side, const float M[16], const float L[4], bool own)
{
    // 0x5099c0
    DamageRecord& r = records[k];
    if (!(r.cooldown <= 0)) return 0;
    if (p.entry < 0 || p.shapes < 1) return 0;
    float d[3];
    if (!r.body) {
        // the shape's matrix in the part's frame: shape x inverse(entry)
        float inv[16], m[16];
        InverseRows(e.matrix, false, inv);
        Articulated::MatMul(p.shapeMatrix, inv, m);
        for (int i = 0; i < 3; ++i) d[i] = m[12 + i] - L[i];  // 0x425f60
    } else {
        for (int i = 0; i < 3; ++i) d[i] = M[12 + i] - L[i];
    }
    if (!(Abs(d[0]) <= size[0] * 0.75f)) return 0;
    if (!(Abs(d[2]) <= size[2] * 0.675000012f)) return 0;
    // the reach shrinks along the hit
    if (!Near(0, D[0])) d[0] = d[0] / (Abs(D[0]) * 0.75f + 1.0f);
    if (!Near(0, D[2])) d[2] = d[2] / (Abs(D[2]) + Abs(D[2]) + 1.0f);
    const float R = Length(p.shapeSize) * reach * 1.5f;
    if (R * R < (d[0] * d[0] + d[2] * d[2]) + d[1] * d[1]) return 0;
    r.cooldown = 0.25f;
    lastPassed = true;
    DamageRecord* t = &r;
    if (linked != -1 && records[linked].state <= 2 && Linked(p.type, p.id)) t = &records[linked];
    const float f = strength * hitScale;
    // the hit's strength: the larger of the record's summed impact and this
    // hit's raw strength
    const float hit = Apply(*t, r, s, Length(d), R, side, f, own, true);
    if (!own) return 0;
    Stage(r, v.id);
    const float half = IsPart70(p.type, p.id) ? 0.5f : 1.0f;
    const float base = 1.0f - half * tearScale * crushScale;
    if (0.09375f * base <= hit && r.dent > 1.5f * base && DamageMayTear(p.type, p.id, p.flags8)) return 3;
    const float need = Stiff(p.type, p.id) ? 2.0f : 1.0f;
    if (hit >= 0.09375f * need && DamageMayHinge(p.type, p.id, p.flags8)) return 2;
    return 0;
}

void VehicleDamage::Impact(const VehicleData& v, const float car[16], float carMass, int hitRecord, float massA,
                           const float J[3], const float point[3], int surface, std::vector<DamageRequest>& out)
{
    const bool own = hitRecord < 0;
    lastPassed = false;
    wheelOff = -1;
    // 0x5054b0: the hit's strength and direction
    const float inv = 1.0f / carMass;  // 0x487780
    const float dv[3] = { inv * J[0], inv * J[1], inv * J[2] };
    const float ss = (dv[2] * dv[2] + dv[1] * dv[1]) + dv[0] * dv[0];
    float s = ss < 9.99999944e-11f ? 0.0f : sqrtf(ss);
    float dir[4] = {};
    if (!Near(0, s)) {
        const float k = 1.0f / s;  // 0x448220
        dir[0] = k * dv[0]; dir[1] = k * dv[1]; dir[2] = k * dv[2];
    }
    const float eff = own ? s : carMass / massA * s;
    if (!(eff >= 5.75f)) return;
    if (records.empty()) return;  // 0x4f67e0
    // (head-on hits on surfaces 0xb / 0x18 set two timers, slot +0x189c /
    // +0x18a0, and strong hits call 0x5063f0: neither touches the records)
    (void)surface;
    // 0x5056a0: the car's matrix and its inverse (slot +0x17f0 clear: 0x5025c0
    // changes nothing)
    float carInv[16];
    InverseRows(car, false, carInv);
    const float p4[4] = { point[0], point[1], point[2], 1.0f };
    float L[4];
    Vec4Transform(p4, carInv, L);  // 0x605119
    L[3] = 0;
    // 0x509970: the side weight
    const float x = (L[0] / (size[0] * 0.5f) + 1.0f) * 0.5f;
    const float t = 1.0f - Abs(x);
    const float side = (1.0f - t * t * t) * x;
    // 0x504424: the wheel hit (the car's own contacts)
    if (own) WheelHit(L, s);  // s before the raise below (runs 15..25: all 12 hits bit-exact)
    // the direction in the car's frame (the inverse without its offset, w 1)
    float rot[16];
    memcpy(rot, carInv, sizeof(rot));
    rot[12] = 0; rot[13] = 0; rot[14] = 0; rot[15] = 1.0f;
    const float d4[4] = { dir[0], dir[1], dir[2], 1.0f };
    float D[4];
    Vec4Transform(d4, rot, D);
    D[3] = 0;
    // s raised by the hit's sideways part near the middle of the length
    float zw = 1.0f - Abs(L[2] / size[2] + L[2] / size[2]);
    if (1.0f < zw) zw = 1.0f;
    else if (0.0f > zw) zw = 0;
    s = (Abs(zw * D[0]) + 1.0f) * s;
    lastS = s; lastSide = side;
    memcpy(lastPoint, L, sizeof(L));
    memcpy(lastDir, D, sizeof(D));

    const int count = int(records.size());
    const float f = strength * hitScale;
    // the part tree: part 0 and its +0x0c - 1 descendants
    const size_t end = v.parts.empty() ? 0 : size_t(v.parts[0].span) < v.parts.size() ? v.parts[0].span : v.parts.size();
    if (!own) {
        // a hinged part's body was hit (0x504905): its record takes the hit
        // at distance 1 of reach 1, split evenly, without the total; a
        // panel already swung beyond 45 degrees (bumpers aside) takes it
        // 7500 times; torn off when the summed impact passes 1.5 x (1 -
        // [+0x94] x tear scale)
        for (size_t i = 0; i < end;) {
            const VehicleData::Part& p = v.parts[i];
            if (p.record >= count) break;
            if (p.record >= 0) {
                DamageRecord& r = records[p.record];
                if (r.state >= 3) { i += p.span; continue; }
                if (p.record == hitRecord && r.state >= 2) {
                    if (r.cooldown <= 0) {
                        float s2 = s;
                        if (!Bumper(p.type, p.id) && !(Abs(r.jointQ) < 0.785398185f)) s2 = s * 7500.0f;
                        Apply(r, r, s2, 1.0f, 1.0f, 0.5f, f, true, false);
                        const float k2 = IsPart70(p.type, p.id) ? 0.5f : 1.0f;
                        if (r.impact > (1.0f - crushScale * tearScale * k2) * 1.5f) {
                            DamageRequest q;
                            q.record = p.record;
                            q.state = 3;
                            Articulated::MatMul(r.bodyMatrix, carInv, q.matrix);
                            out.push_back(q);
                        }
                    }
                    break;
                }
            }
            i += (p.flags4 & 1) ? 1 : p.span;
        }
    }
    // every part with a record still on the car or hinged
    for (size_t i = 0; i < end;) {
        const VehicleData::Part& p = v.parts[i];
        if (p.record >= count) break;
        if (p.record < 0) { i += (p.flags4 & 1) ? 1 : p.span; continue; }
        DamageRecord& r = records[p.record];
        if (r.state >= 3) { i += p.span; continue; }
        const VehicleData::DamageEntry& e = v.damage[p.record];
        if (r.state >= 2 && r.body) {
            float M[16];
            Articulated::MatMul(r.bodyMatrix, carInv, M);
            const int st = PartTest(v, p, p.record, e, s, D, side, M, L, own);
            if (st) {
                DamageRequest q;
                q.record = p.record;
                q.state = st;
                memcpy(q.matrix, M, sizeof(M));
                out.push_back(q);
            }
            i += p.span;
            continue;
        }
        const int st = PartTest(v, p, p.record, e, s, D, side, kIdentity, L, own);
        if (st) {
            DamageRequest q;
            q.record = p.record;
            q.state = st;
            memcpy(q.matrix, kIdentity, sizeof(kIdentity));
            out.push_back(q);
        }
        i += (p.flags4 & 1) ? 1 : p.span;
    }
    Share(v);
}
