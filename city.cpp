#include "city.h"

#include <cmath>
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
    cells.clear();
    water = CityWater();
    treeCells.clear();
    error_.clear();
    if (d.size() < 16 || memcmp(d.data(), "CHNK", 4) != 0) { error_ = "not a CHNK file"; return false; }
    // Top level: GESR regions, GEIR cells, IRCT, PINF, GEGL, PCSL. The GEBI
    // is a child of a GEIR (the cell's instances) or of a GESR.
    uint32_t count = U32(d, 8);
    int geirCells = 0;
    for (uint32_t k = 0; k < count; ++k) {
        size_t e = 16 + size_t(k) * 16;
        if (e + 16 > d.size()) break;
        if (memcmp(&d[e], "GEIR", 4) == 0) ++geirCells;
        else if (memcmp(&d[e], "GESR", 4) != 0) continue;
        size_t g = U32(d, e + 4);
        if (g + 16 > d.size() || memcmp(&d[g], "CHNK", 4) != 0) continue;
        uint32_t children = U32(d, g + 8);
        size_t first = instances.size();
        bool gebi = false;
        size_t scnd = 0, scndSize = 0;
        for (uint32_t j = 0; j < children; ++j) {
            size_t c = g + 16 + size_t(j) * 16;
            if (c + 16 > d.size()) break;
            if (memcmp(&d[c], "GWTR", 4) == 0 && d[c + 8] == 5 && !LoadGwtr(d, g + U32(d, c + 4), U32(d, c + 12)))
                return false;
            if (memcmp(&d[c], "ATRD", 4) == 0 && !LoadAtrd(d, g + U32(d, c + 4), U32(d, c + 12))) return false;
            if (memcmp(&d[c], "SCND", 4) == 0 && !scnd) {
                scnd = g + U32(d, c + 4);
                scndSize = U32(d, c + 12);
            }
            if (memcmp(&d[c], "GEBI", 4) != 0) continue;
            if (!LoadGebi(d, g + U32(d, c + 4), U32(d, c + 12))) return false;
            gebi = true;
        }
        if (gebi && scnd) {
            CityCell cell;
            cell.firstInstance = first;
            cell.instanceCount = instances.size() - first;
            if (!LoadScnd(d, scnd, scndSize, cell)) return false;
            cells.push_back(std::move(cell));
        }
    }
    while ((gridSide + 1) * (gridSide + 1) <= geirCells) ++gridSide;
    return true;
}

bool CityPlacement::LoadAtrd(const std::vector<uint8_t>& d, size_t off, size_t size)
{
    if (off + size > d.size() || size < 0x64) { error_ = "ATRD header"; return false; }
    CityTreeCell cell;
    for (int k = 0; k < 2; ++k) {
        cell.corner[k] = F32(d, off + 4 * k);
        cell.size[k] = F32(d, off + 8 + 4 * k);
    }
    uint32_t from = U32(d, off + 0x10 + 14 * 4), to = U32(d, off + 0x10 + 15 * 4);
    if (from > to || to > size || (to - from) % 16) { error_ = "ATRD kind 14"; return false; }
    for (uint32_t r = from; r < to; r += 16) {
        size_t p = off + r;
        int32_t w0 = int32_t(U32(d, p)), w1 = int32_t(U32(d, p + 4));
        CityTree t;
        t.position[0] = float(w0) * (1.0f / 524288.0f);
        t.position[1] = float(w0 & 0x1fff) * (1.0f / 64.0f) - 10.0f;
        t.position[2] = float(w1) * (1.0f / 524288.0f);
        t.type = d[p + 10];
        t.axis = d[p + 11];
        cell.trees.push_back(t);
    }
    treeCells.push_back(std::move(cell));
    return true;
}

bool CityPlacement::LoadScnd(const std::vector<uint8_t>& d, size_t off, size_t size, CityCell& cell)
{
    if (off + size > d.size() || size < 0x28) { error_ = "SCND header"; return false; }
    uint32_t nodes = U32(d, off), table = U32(d, off + 4), nodeCount = U32(d, off + 8);
    uint32_t indexCount = U32(d, off + 0x0c), stride = U32(d, off + 0x10);
    if (size_t(nodes) + size_t(nodeCount) * 16 > size || (indexCount && (stride < 4 || size_t(table) + size_t(indexCount) * stride > size))) {
        error_ = "SCND tables";
        return false;
    }
    cell.size = F32(d, off + 0x14);
    cell.origin[0] = F32(d, off + 0x18);
    cell.origin[1] = F32(d, off + 0x1c);
    cell.column = int(U32(d, off + 0x20));
    cell.row = int(U32(d, off + 0x24));
    for (uint32_t i = 0; i < nodeCount; ++i) {
        size_t p = off + nodes + size_t(i) * 16;
        uint16_t w[8];
        memcpy(w, &d[p], sizeof(w));
        CityNode n;
        n.first = w[0] & 0xfff;
        n.count = w[0] >> 12;
        n.level = w[1];
        n.x = w[2];
        n.z = w[3];
        for (int k = 0; k < 4; ++k) n.child[k] = w[4 + k] < nodeCount ? w[4 + k] : 0;
        if (size_t(n.first) + n.count > indexCount) n.count = 0;
        cell.nodes.push_back(n);
    }
    for (uint32_t i = 0; i < indexCount; ++i) {
        uint32_t r = U32(d, off + table + size_t(i) * stride);
        cell.index.push_back(r < cell.instanceCount ? r : 0xffffffffu);
    }
    return true;
}

bool CityPlacement::LoadGwtr(const std::vector<uint8_t>& d, size_t off, size_t size)
{
    if (off + size > d.size() || size < 0x10) { error_ = "GWTR header"; return false; }
    uint32_t tiles = U32(d, off);
    int textures = d[off + 0xc], steps = d[off + 0xd], sets = d[off + 0xe];
    size_t colours = off + 0x10 + size_t(tiles) * 0x20, refs = colours + size_t(steps) * sets * 16;
    if (refs + size_t(textures) * 4 > off + size) { error_ = "GWTR tables"; return false; }
    water = CityWater();
    water.height = F32(d, off + 8);
    water.shoreLines = U32(d, off + 4);
    water.colourSteps = steps;
    water.colourSets = sets;
    for (uint32_t i = 0; i < tiles; ++i) {
        size_t p = off + 0x10 + size_t(i) * 0x20;
        CityInstance in;
        for (int k = 0; k < 3; ++k) in.position[k] = F32(d, p + 4 * k);
        memcpy(&in.model, &d[p + 0x10], 2);
        in.extra = 0xffff;
        in.column = in.row = -2;
        in.rotation[3] = 1.0f;  // the identity quaternion
        WorldMatrix(in);
        water.tiles.push_back(in);
    }
    for (size_t i = 0; i < size_t(steps) * sets * 4; ++i) water.colours.push_back(F32(d, colours + 4 * i));
    for (int i = 0; i < textures; ++i) water.textures.push_back(U32(d, refs + 4 * size_t(i)));
    // The shore lines (0x454b00 -> 0x4546f0, records of 0x18 + points x 0x18).
    size_t p = refs + size_t(textures) * 4;
    for (uint32_t i = 0; i < water.shoreLines; ++i) {
        if (p + 0x18 > off + size) { error_ = "GWTR shore lines"; return false; }
        uint32_t n = U32(d, p);
        if (p + 0x18 + size_t(n) * 0x18 > off + size) { error_ = "GWTR shore points"; return false; }
        ShoreLine line;
        for (int k = 0; k < 2; ++k) {
            line.centre[k] = F32(d, p + 8 + 4 * k);
            line.half[k] = F32(d, p + 0x10 + 4 * k);
        }
        line.radius = std::sqrt(line.half[0] * line.half[0] + line.half[1] * line.half[1]);
        for (size_t k = 0; k < size_t(n) * 6; ++k) line.points.push_back(F32(d, p + 0x18 + 4 * k));
        water.shore.push_back(std::move(line));
        p += 0x18 + size_t(n) * 0x18;
    }
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
