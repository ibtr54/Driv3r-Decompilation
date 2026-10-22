#include "vehicle.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>

namespace {

uint32_t U32(const std::vector<uint8_t>& d, size_t o)
{
    uint32_t v = 0;
    if (o + 4 <= d.size()) memcpy(&v, &d[o], 4);
    return v;
}

uint16_t U16(const std::vector<uint8_t>& d, size_t o)
{
    uint16_t v = 0;
    if (o + 2 <= d.size()) memcpy(&v, &d[o], 2);
    return v;
}

float F32(const std::vector<uint8_t>& d, size_t o)
{
    float v = 0;
    if (o + 4 <= d.size()) memcpy(&v, &d[o], 4);
    return v;
}

// 0x512c11: the wheel slot of a part id (0x1b..0x2b, table 0x5133b4 into
// the jump table 0x513398); other ids: -1.
int WheelSlot(int id)
{
    switch (id) {
    case 0x1c: return 0;
    case 0x1e: return 1;
    case 0x1b: return 2;
    case 0x1d: return 3;
    case 0x2b: return 4;
    case 0x2a: return 5;
    default: return -1;
    }
}

// One UPVH (0x5126a9..). False when the tables do not fit or the physics
// block is not where the counts put it.
bool ReadUpvh(const std::vector<uint8_t>& d, size_t up, size_t size, VehicleData& v)
{
    if (size < 0x30 || up + size > d.size() || memcmp(&d[up], "AWHF", 4) != 0 || U32(d, up + 0xc) < 5) return false;
    v.id = int(U32(d, up + 0x14));
    size_t parts = up + 0x30, nparts = U32(d, up + 0x10);
    size_t hard = parts + nparts * 0x20, nhard = U16(d, up + 0x22);
    size_t pdl = hard + nhard * 0x20 + size_t(U16(d, up + 0x24)) * 0x50 + size_t(U16(d, up + 0x20)) * 0x10 +
                 size_t(U16(d, up + 0x26)) * 0x40;
    if (pdl + 0x20 > up + size || memcmp(&d[pdl + 0x10], "PDL001", 6) != 0) return false;
    size_t entries = pdl + U32(d, pdl + 8), nentries = U32(d, pdl);
    size_t points = hard + nhard * 0x20 + size_t(U16(d, up + 0x24)) * 0x50, npoints = U16(d, up + 0x20);
    bool rimFound = false;
    for (size_t i = 0; i < nparts; ++i) {
        size_t r = parts + i * 0x20;
        uint16_t type = U16(d, r), ref = U16(d, r + 0x12);
        if (type == 3 && U16(d, r + 2) == 6 && U16(d, r + 0x16) < npoints)
            for (int k = 0; k < 3; ++k) v.exhausts.push_back(F32(d, points + U16(d, r + 0x16) * 0x10 + k * 4));
        if (type == 8 && U16(d, r + 2) == 0x48 && !rimFound) {  // 0x5139f0: the first match
            rimFound = true;
            v.rim = (ref >> 8) == 0xfc ? ref & 0xff : -1;
        }
        if ((ref >> 8) != 0xfc) continue;  // no model
        int model = ref & 0xff;
        if (type == 1) v.body.push_back(model);
        if (type != 2) continue;
        VehicleWheel w;
        w.model = model;
        w.slot = WheelSlot(U16(d, r + 2));
        uint16_t hp = U16(d, r + 0x18), entry = U16(d, r + 0x14);
        if (hp < nhard)
            for (int k = 0; k < 3; ++k) w.hardpoint[k] = F32(d, hard + hp * 0x20 + k * 4);
        if (entry < nentries) {
            size_t e = entries + size_t(entry) * 16;
            size_t shape = e + U32(d, e + 4);  // relocated by 0x5a5a50: from the entry
            w.radius = F32(d, shape);
            w.width = F32(d, shape + 4);
        }
        v.wheels.push_back(w);
    }
    // An entry's shapes as the body gets them.
    auto readShapes = [&](uint32_t entry, std::vector<VehicleData::Shape>& out) {
        if (entry >= nentries) return;
        size_t e = entries + size_t(entry) * 16;
        size_t first = e + U32(d, e + 4);
        for (uint32_t s = 0; s < U32(d, e); ++s) {
            size_t sh = first + size_t(s) * 0x60;
            if (sh + 0x60 > up + size) break;
            VehicleData::Shape shape;
            shape.type = d[sh + 0x50];
            shape.surface = d[sh + 0x51];
            for (int k = 0; k < 3; ++k) shape.size[k] = F32(d, sh + 4 * k);
            // The PDL is z-up with y forward (vehicle 6: the bounding box
            // 0.88 x 2.29 x 0.49 about (0, 0.02, 0.33), the roof at y +0.36,
            // the hood at +1.48); the body is y-up with z forward: each row
            // (x, y, z) -> (-x, z, y), a rotation (where the game turns it,
            // 0x4b6500's matrix from the model's +0x14.. before 0x5a0b60,
            // was not traced).
            for (int r = 0; r < 4; ++r) {
                const float x = F32(d, sh + 0x10 + 16 * r), y = F32(d, sh + 0x14 + 16 * r), z = F32(d, sh + 0x18 + 16 * r);
                shape.local[4 * r] = -x;
                shape.local[4 * r + 1] = z;
                shape.local[4 * r + 2] = y;
                shape.local[4 * r + 3] = F32(d, sh + 0x1c + 16 * r);
            }
            out.push_back(shape);
        }
    };
    // The damage parts' tables.
    auto index = [](uint16_t u) { return u == 0xffff ? -1 : int(u); };
    for (size_t i = 0; i < nparts; ++i) {
        size_t r = parts + i * 0x20;
        VehicleData::Part p;
        p.type = U16(d, r);
        p.id = U16(d, r + 2);
        p.entry = index(U16(d, r + 0x14));
        p.hardpoint = index(U16(d, r + 0x18));
        p.record = index(U16(d, r + 0x1a));
        p.flags4 = U32(d, r + 4);
        p.flags8 = U32(d, r + 8);
        p.span = U16(d, r + 0xc);
        if (U16(d, r + 0x14) < nentries) {
            size_t e = entries + size_t(U16(d, r + 0x14)) * 16;
            size_t sh = e + U32(d, e + 4);
            p.shapes = int(U32(d, e));
            if (p.shapes > 0 && sh + 0x50 <= up + size) {
                for (int k = 0; k < 3; ++k) p.shapeSize[k] = F32(d, sh + 4 * k);
                for (int k = 0; k < 16; ++k) p.shapeMatrix[k] = F32(d, sh + 0x10 + 4 * k);
            }
            readShapes(U16(d, r + 0x14), p.collision);
        }
        v.parts.push_back(p);
    }
    for (size_t i = 0; i < nhard; ++i) {
        VehicleData::Hardpoint h;
        for (int k = 0; k < 4; ++k) {
            h.pos[k] = F32(d, hard + i * 0x20 + 4 * k);
            h.axis[k] = F32(d, hard + i * 0x20 + 0x10 + 4 * k);
        }
        v.hardpoints.push_back(h);
    }
    for (size_t i = 0; i < U16(d, up + 0x24); ++i) {
        size_t r = hard + nhard * 0x20 + i * 0x50;
        VehicleData::DamageEntry e;
        for (int k = 0; k < 16; ++k) e.matrix[k] = F32(d, r + 4 * k);
        for (int k = 0; k < 3; ++k) e.inertia[k] = F32(d, r + 0x40 + 4 * k);
        e.mass = F32(d, r + 0x4c);
        v.damage.push_back(e);
    }
    // The body's shapes: the physics entry of the part of type 4.
    uint32_t bodyEntry = 0;
    for (size_t i = 0; i < nparts; ++i)
        if (U16(d, parts + i * 0x20) == 4 && U16(d, parts + i * 0x20 + 0x14) < nentries) {
            bodyEntry = U16(d, parts + i * 0x20 + 0x14);
            break;
        }
    readShapes(bodyEntry, v.collision);
    return true;
}

}  // namespace

bool ReadVehicles(const std::vector<uint8_t>& d, std::vector<VehicleData>& out)
{
    out.clear();
    if (d.size() < 16 || memcmp(d.data(), "CHNK", 4) != 0) return false;
    uint32_t count = U32(d, 8);
    int mdpc = 0;
    for (uint32_t k = 0; k < count; ++k) {
        size_t e = 16 + size_t(k) * 16;
        if (e + 16 > d.size()) break;
        size_t off = U32(d, e + 4), size = U32(d, e + 12);
        if (memcmp(&d[e], "MDPC", 4) == 0) continue;
        // A vehicle entry: a CHNK with its UPVH.
        if (off + 16 > d.size() || memcmp(&d[off], "CHNK", 4) != 0) continue;
        for (uint32_t j = 0; j < U32(d, off + 8); ++j) {
            size_t f = off + 16 + size_t(j) * 16;
            if (f + 16 > d.size() || memcmp(&d[f], "UPVH", 4) != 0) continue;
            VehicleData v;
            if (!ReadUpvh(d, off + U32(d, f + 4), U32(d, f + 12), v)) continue;
            v.mdpc = mdpc;
            out.push_back(v);
        }
        ++mdpc;
        (void)size;
    }
    return !out.empty();
}

void VehicleCreationPose(const VehicleData& v, const MdpcPackage& p, const float pose[4], float ground, float compression,
                         float body[16], std::vector<float>& wheels, float& height)
{
    // 0x513c10: the wheel of the lowest slot (0x884 + slot x 4), radius
    // (+0x8fc) - hardpoint y (0x511520: +0x89c y - radius) - 0x5138c0.
    const VehicleWheel* first = nullptr;
    for (const VehicleWheel& w : v.wheels)
        if (w.slot >= 0 && (!first || w.slot < first->slot)) first = &w;
    height = first ? first->radius - (first->hardpoint[1] + compression) : 0.0f;
    // 0x50e940: the heading as a turn about y; rows x = (cos h, 0, -sin h),
    // y = up, z = (sin h, 0, cos h) (the vehicle's forward), then the
    // position.
    float c = std::cos(pose[3]), s = std::sin(pose[3]);
    const float b[16] = { c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, pose[0], ground + height, pose[2], 1 };
    memcpy(body, b, sizeof(b));
    wheels.clear();
    for (const VehicleWheel& w : v.wheels) {
        float local[16];
        VehicleWheelLocal(w, p, 0.0f, 0.0f, compression, local);
        float out[16];
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k)
                out[r * 4 + k] = local[r * 4] * b[k] + local[r * 4 + 1] * b[4 + k] + local[r * 4 + 2] * b[8 + k] +
                                 local[r * 4 + 3] * b[12 + k];
        wheels.insert(wheels.end(), out, out + 16);
    }
}

// 0x4ff530's rotation, RotX(spin) x RotY(steer) (slots 0 and 2 build both,
// the others only RotX: steer 0), 3 x 3 rows.
static void WheelRotation(float spin, float steer, float r[9])
{
    const float id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    memcpy(r, id, sizeof(id));
    if (spin == 0.0f && steer == 0.0f) return;
    const float cx = std::cos(spin), sx = std::sin(spin), cy = std::cos(steer), sy = std::sin(steer);
    const float rx[9] = { 1, 0, 0, 0, cx, sx, 0, -sx, cx };
    const float ry[9] = { cy, 0, -sy, 0, 1, 0, sy, 0, cy };
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) r[i * 3 + k] = rx[i * 3] * ry[k] + rx[i * 3 + 1] * ry[3 + k] + rx[i * 3 + 2] * ry[6 + k];
}

void VehicleRimLocal(const VehicleWheel& w, float spin, float steer, float lift, float out[16])
{
    float r[9];
    WheelRotation(spin, steer, r);
    const float m = (w.slot == 0 || w.slot == 1 || w.slot == 4) ? -1.0f : 1.0f;  // rows 0 and 2
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) out[i * 4 + k] = (i == 1 ? 1.0f : m) * r[i * 3 + k];
        out[i * 4 + 3] = 0;
    }
    out[12] = w.hardpoint[0];
    out[13] = w.hardpoint[1] + lift;
    out[14] = w.hardpoint[2];
    out[15] = 1;
}

void VehicleWheelLocal(const VehicleWheel& w, const MdpcPackage& p, float spin, float steer, float lift, float out[16])
{
    // The wheel model's pivot: its slot 0 submesh matrix keeps the hardpoint
    // it sits at in the 4th row as (x, -z, y).
    float pivot[3] = { w.hardpoint[0], w.hardpoint[1], w.hardpoint[2] };
    if (w.model >= 0 && size_t(w.model) < p.models.size() && !p.models[size_t(w.model)].lods[0].empty() &&
        p.models[size_t(w.model)].lods[0][0].hasMatrix) {
        const float* m = p.models[size_t(w.model)].lods[0][0].matrix;
        pivot[0] = m[12];
        pivot[1] = m[14];
        pivot[2] = -m[13];
    }
    // turn = 180 degrees about y when the hardpoint is on the other side of
    // the pivot.
    const float t = (w.hardpoint[0] < 0) != (pivot[0] < 0) ? -1.0f : 1.0f;
    float r[9];
    WheelRotation(spin, steer, r);
    const float turn[3] = { t, 1.0f, t };
    const float q[3] = { -t * pivot[0], -pivot[1], -t * pivot[2] };  // -pivot x turn
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) out[i * 4 + k] = turn[i] * r[i * 3 + k];
        out[i * 4 + 3] = 0;
    }
    if (spin == 0.0f && steer == 0.0f) {
        out[12] = q[0] + w.hardpoint[0];
        out[13] = q[1] + w.hardpoint[1] + lift;
        out[14] = q[2] + w.hardpoint[2];
    } else {
        for (int k = 0; k < 3; ++k) out[12 + k] = q[0] * r[k] + q[1] * r[3 + k] + q[2] * r[6 + k] + w.hardpoint[k];
        out[13] += lift;
    }
    out[15] = 1;
}

namespace {

// Driv3r.exe mapped by its section headers: a virtual address's file offset
// (SIZE_MAX outside the sections' raw data).
size_t ExeOffset(const std::vector<uint8_t>& exe, uint32_t va)
{
    uint32_t pe = U32(exe, 0x3c);
    if (pe + 0xf8 > exe.size()) return SIZE_MAX;
    uint16_t n = U16(exe, pe + 6), opt = U16(exe, pe + 20);
    uint32_t base = U32(exe, pe + 0x34);
    for (uint16_t i = 0; i < n; ++i) {
        size_t s = pe + 24 + opt + i * 40;
        uint32_t lo = base + U32(exe, s + 12), raw = U32(exe, s + 16), ro = U32(exe, s + 20);
        if (va >= lo && va < lo + raw) return ro + (va - lo);
    }
    return SIZE_MAX;
}

// The parameter blocks the static initialisers build in .bss (0x6da080 for
// 0x8b4050, 0x6da7c0 for 0x8b4348, 0x6daf00 for 0x8b4640, 0x6db640 for
// 0x8b4938), run on a copy of memory: dword / byte constant stores, mov
// [abs], eax, or eax, -1, mov ecx / esi / edi, imm, rep movsd, push / pop
// esi / edi, ret. False on any other instruction (another executable).
bool RunInitialisers(const std::vector<uint8_t>& exe, std::map<uint32_t, uint32_t>& mem)
{
    auto rd = [&](uint32_t a) {
        auto it = mem.find(a);
        if (it != mem.end()) return it->second;
        size_t o = ExeOffset(exe, a);
        return o == SIZE_MAX ? 0u : U32(exe, o);
    };
    static const uint32_t kInit[4] = { 0x6da080, 0x6da7c0, 0x6daf00, 0x6db640 };
    for (uint32_t start : kInit) {
        size_t o = ExeOffset(exe, start);
        if (o == SIZE_MAX) return false;
        uint32_t eax = 0, ecx = 0, esi = 0, edi = 0;
        for (int guard = 0;; ++guard) {
            if (guard > 2000 || o + 10 > exe.size()) return false;
            uint8_t b = exe[o];
            if (b == 0x56 || b == 0x57 || b == 0x5e || b == 0x5f) { o += 1; continue; }
            if (b == 0xc3) break;
            if (b == 0xc7 && exe[o + 1] == 0x05) { mem[U32(exe, o + 2)] = U32(exe, o + 6); o += 10; continue; }
            if (b == 0xc6 && exe[o + 1] == 0x05) {
                uint32_t a = U32(exe, o + 2), w = rd(a & ~3u), sh = (a & 3) * 8;
                mem[a & ~3u] = (w & ~(0xffu << sh)) | (uint32_t(exe[o + 6]) << sh);
                o += 7;
                continue;
            }
            if (b == 0xa3) { mem[U32(exe, o + 1)] = eax; o += 5; continue; }
            if (b == 0x83 && exe[o + 1] == 0xc8 && exe[o + 2] == 0xff) { eax = 0xffffffffu; o += 3; continue; }
            if (b == 0xb9) { ecx = U32(exe, o + 1); o += 5; continue; }
            if (b == 0xbe) { esi = U32(exe, o + 1); o += 5; continue; }
            if (b == 0xbf) { edi = U32(exe, o + 1); o += 5; continue; }
            if (b == 0xf3 && exe[o + 1] == 0xa5) {
                for (uint32_t i = 0; i < ecx; ++i) mem[edi + 4 * i] = rd(esi + 4 * i);
                o += 2;
                continue;
            }
            return false;
        }
    }
    return true;
}

}  // namespace

bool VehicleHandling(const std::vector<uint8_t>& exe, const std::vector<uint8_t>& bo3, int id, float out[kHandlingKeys])
{
    // 0x5142a0: ids 0x13..0x5b through the byte table 0x514308 and the jump
    // table 0x5142e8 (each case mov eax, block; ret), else 0x7a3570.
    uint32_t block = 0x7a3570;
    if (id >= 0x13 && id - 0x13 <= 0x48) {
        size_t t = ExeOffset(exe, 0x514308 + uint32_t(id - 0x13)), j = ExeOffset(exe, 0x5142e8);
        if (t == SIZE_MAX || j == SIZE_MAX) return false;
        size_t c = ExeOffset(exe, U32(exe, j + 4 * exe[t]));
        if (c == SIZE_MAX || exe[c] != 0xb8) return false;
        block = U32(exe, c + 1);
    }
    std::map<uint32_t, uint32_t> mem;
    if (block >= 0x8b0000 && !RunInitialisers(exe, mem)) return false;
    for (int k = 0; k < kHandlingKeys; ++k) {
        uint32_t a = block + 4 * k, v = 0;
        auto it = mem.find(a);
        if (it != mem.end()) v = it->second;
        else {
            size_t o = ExeOffset(exe, a);
            if (o == SIZE_MAX) return false;
            v = U32(exe, o);
        }
        memcpy(&out[k], &v, 4);
    }
    // The BO3 record (0x511819 / 0x514360): offset table +0x380c = file +0x20
    // (u16 per id, count +0x18), data +0x3808 = file +0x108.
    if (bo3.size() < 0x108 || memcmp(bo3.data(), "BVO3", 4) != 0 || uint32_t(id) > U32(bo3, 0x18)) return true;
    uint16_t rel = U16(bo3, 0x20 + size_t(id) * 2);
    if (rel == 0xffff || rel >= 0x3000) return true;
    size_t r = 0x108 + rel;
    if (r + 12 > bo3.size() || memcmp(&bo3[r], "DVO3", 4) != 0) return true;
    uint32_t count = U32(bo3, r + 8);
    size_t p = r + 12;
    for (uint32_t i = 0; i < count && p + 4 <= bo3.size(); ++i) {
        uint32_t key = U32(bo3, p);
        if (key == 0xaaaaffffu) break;
        key &= 0x5555ffffu;
        p += 4;
        if (key < uint32_t(kHandlingKeys)) {
            out[key] = F32(bo3, p);
            p += 4;
        }
    }
    return true;
}

int HandlingClass(int id)
{
    switch (id) {
    case 0x1f: case 0x2c: case 0x43: case 0x44: case 0x51: case 0x52:
        return 0x115;
    case 0x2d: case 0x2e: case 0x2f: case 0x30: case 0x45: case 0x46: case 0x47:
    case 0x53: case 0x54: case 0x55: case 0x59: case 0x5a: case 0x5b:
        return 0x116;
    case 0x31:
        return 0x81;
    case 0x56:
        return 0x82;
    default:
        return 0x11b;
    }
}

float RestCompression(const float t[kHandlingKeys])
{
    float c = -t[0x8c] * t[0] * 0.01f / t[1];  // [0x78e8dc] = 0.01
    if (c > 1.0f) return 1.0f;
    return c < 0.0f ? 0.0f : c;
}

float WheelOffset(const float t[kHandlingKeys], int cls)
{
    return cls == 0x11b ? -(t[3] * t[0x19]) : 0.0f;
}

float WheelLift(const float t[kHandlingKeys], int cls, float compression)
{
    if (cls != 0x11b) return 0.0f;
    const float c = compression < t[0xbc] ? compression : t[0xbc];
    return c - t[0x19] * t[3];
}
