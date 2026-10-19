#include "vehicle.h"

#include <cmath>
#include <cstring>

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
    for (size_t i = 0; i < nparts; ++i) {
        size_t r = parts + i * 0x20;
        uint16_t type = U16(d, r), ref = U16(d, r + 0x12);
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
        // The wheel model's pivot: its slot 0 submesh matrix keeps the
        // hardpoint it sits at in the 4th row as (x, -z, y).
        float pivot[3] = { w.hardpoint[0], w.hardpoint[1], w.hardpoint[2] };
        if (w.model >= 0 && size_t(w.model) < p.models.size() && !p.models[size_t(w.model)].lods[0].empty() &&
            p.models[size_t(w.model)].lods[0][0].hasMatrix) {
            const float* m = p.models[size_t(w.model)].lods[0][0].matrix;
            pivot[0] = m[12];
            pivot[1] = m[14];
            pivot[2] = -m[13];
        }
        // (vertex - pivot) x turn + hardpoint + (0, compression, 0), then the
        // body; turn = 180 degrees about y when the hardpoint is on the other
        // side of the pivot.
        float t = (w.hardpoint[0] < 0) != (pivot[0] < 0) ? -1.0f : 1.0f;
        const float local[16] = { t, 0, 0, 0, 0, 1, 0, 0, 0, 0, t, 0,
                                  -t * pivot[0] + w.hardpoint[0], -pivot[1] + w.hardpoint[1] + compression,
                                  -t * pivot[2] + w.hardpoint[2], 1 };
        float out[16];
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k)
                out[r * 4 + k] = local[r * 4] * b[k] + local[r * 4 + 1] * b[4 + k] + local[r * 4 + 2] * b[8 + k] +
                                 local[r * 4 + 3] * b[12 + k];
        wheels.insert(wheels.end(), out, out + 16);
    }
}
