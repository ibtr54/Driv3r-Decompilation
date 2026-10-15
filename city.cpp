#include "city.h"

#include <cstring>

namespace {

uint32_t U32(const std::vector<uint8_t>& d, size_t p)
{
    uint32_t v = 0;
    if (p + 4 <= d.size()) memcpy(&v, &d[p], 4);
    return v;
}

float F32(const std::vector<uint8_t>& d, size_t p)
{
    float v = 0;
    if (p + 4 <= d.size()) memcpy(&v, &d[p], 4);
    return v;
}

// 0x473830: a record whose +0x1c is above 100.0 (0x78e8d8) holds the cosine
// and sine of a turn about the vertical axis (0x418020 gets the rows
// (c, 0, s, 0), (0, 1, 0, 0), (-s, 0, c, 0), (x, y, z, 1)); any other record
// holds a quaternion, turned into a matrix by 0x59f650 (cached there by
// record address in a 0x641-entry pool).
void WorldMatrix(CityInstance& in)
{
    float* m = in.world;
    const float* q = in.rotation;
    if (q[3] > 100.0f) {
        float c = q[0], s = q[1];
        const float r[12] = { c, 0, s, 0,  0, 1, 0, 0,  -s, 0, c, 0 };
        memcpy(m, r, sizeof(r));
    } else {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        m[0] = 1 - 2 * y * y - 2 * z * z; m[1] = 2 * x * y + 2 * w * z;     m[2] = 2 * x * z - 2 * w * y;     m[3] = 0;
        m[4] = 2 * x * y - 2 * w * z;     m[5] = 1 - 2 * x * x - 2 * z * z; m[6] = 2 * y * z + 2 * w * x;     m[7] = 0;
        m[8] = 2 * x * z + 2 * w * y;     m[9] = 2 * y * z - 2 * w * x;     m[10] = 1 - 2 * x * x - 2 * y * y; m[11] = 0;
    }
    m[12] = in.position[0];
    m[13] = in.position[1];
    m[14] = in.position[2];
    m[15] = 1;
}

}  // namespace

bool CityPlacement::Load(const std::vector<uint8_t>& d)
{
    instances.clear();
    error_.clear();
    if (d.size() < 16 || memcmp(d.data(), "CHNK", 4) != 0) { error_ = "not a CHNK file"; return false; }
    // Top level: GESR regions, GEIR cells, IRCT, PINF, GEGL, PCSL. The GEBI
    // is a child of a GEIR (the cell's instances) or of a GESR.
    uint32_t count = U32(d, 8);
    int cells = 0;
    for (uint32_t k = 0; k < count; ++k) {
        size_t e = 16 + size_t(k) * 16;
        if (e + 16 > d.size()) break;
        if (memcmp(&d[e], "GEIR", 4) == 0) ++cells;
        else if (memcmp(&d[e], "GESR", 4) != 0) continue;
        size_t g = U32(d, e + 4);
        if (g + 16 > d.size() || memcmp(&d[g], "CHNK", 4) != 0) continue;
        uint32_t children = U32(d, g + 8);
        for (uint32_t j = 0; j < children; ++j) {
            size_t c = g + 16 + size_t(j) * 16;
            if (c + 16 > d.size()) break;
            if (memcmp(&d[c], "GEBI", 4) != 0) continue;
            if (!LoadGebi(d, g + U32(d, c + 4), U32(d, c + 12))) return false;
        }
    }
    while ((gridSide + 1) * (gridSide + 1) <= cells) ++gridSide;
    return true;
}

bool CityPlacement::LoadGebi(const std::vector<uint8_t>& d, size_t off, size_t size)
{
    // Header (the handler keeps +4 = data, +8 = data + 0x40): +0 records,
    // +4 / +8 cell column / row, +0x0c..+0x20 the cumulative ends of six
    // ranges (0x4dcaf0 and 0x4de6cd walk [+0x10, +0x14) and [+0x18, +0x1c)
    // looking each model up in the ANOD lists of 0x8ac354).
    if (off + size > d.size() || size < 0x40) { error_ = "GEBI header"; return false; }
    uint32_t n = U32(d, off);
    if (0x40 + size_t(n) * 0x20 > size) { error_ = "GEBI records"; return false; }
    uint32_t ends[6];
    for (int r = 0; r < 6; ++r) ends[r] = U32(d, off + 0x0c + 4 * r);
    for (uint32_t i = 0; i < n; ++i) {
        size_t p = off + 0x40 + size_t(i) * 0x20;
        CityInstance in;
        for (int k = 0; k < 3; ++k) in.position[k] = F32(d, p + 4 * k);
        memcpy(&in.model, &d[p + 0x0c], 2);
        memcpy(&in.extra, &d[p + 0x0e], 2);
        for (int k = 0; k < 4; ++k) in.rotation[k] = F32(d, p + 0x10 + 4 * k);
        in.column = int(U32(d, off + 4));
        in.row = int(U32(d, off + 8));
        while (in.range < 5 && i >= ends[in.range]) ++in.range;
        WorldMatrix(in);
        instances.push_back(in);
    }
    return true;
}
