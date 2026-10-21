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
    ground.Clear();
    collision.clear();
    dynamicModels.clear();
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
            if (memcmp(&d[c], "GRTD", 4) == 0 && !ground.Add(d, g + U32(d, c + 4), U32(d, c + 12))) {
                error_ = "GRTD";
                return false;
            }
            if (memcmp(&d[c], "GEPM", 4) == 0 || memcmp(&d[c], "PMLD", 4) == 0) {
                size_t off = g + U32(d, c + 4), size = U32(d, c + 12);
                if (off + size > d.size() || size < 0x10) { error_ = "GEPM / PMLD"; return false; }
                int region = int(U32(d, off));
                CityCollisionRegion* cr = nullptr;
                for (CityCollisionRegion& r : collision)
                    if (r.region == region) cr = &r;
                if (!cr) {
                    collision.emplace_back();
                    cr = &collision.back();
                    cr->region = region;
                }
                if (d[c] == 'G') {
                    size_t pdl = off + U32(d, off + 4);
                    if (pdl + 0x20 > off + size || memcmp(&d[pdl + 0x10], "PDL001", 6) != 0) { error_ = "GEPM"; return false; }
                    cr->pdl.assign(d.begin() + pdl, d.begin() + off + size);
                } else {
                    uint32_t n = U32(d, off + 4);
                    size_t rec = off + U32(d, off + 8);
                    if (rec + size_t(n) * 0x14 > off + size) { error_ = "PMLD"; return false; }
                    for (uint32_t i = 0; i < n; ++i) {
                        size_t r = rec + size_t(i) * 0x14;
                        cr->models.push_back({ U32(d, r), U32(d, r + 4), U32(d, r + 8), F32(d, r + 12) });
                    }
                }
            }
            if (memcmp(&d[c], "GEPD", 4) == 0) {
                size_t off = g + U32(d, c + 4), size = U32(d, c + 12);
                if (off + 16 <= d.size() && off + size <= d.size() && memcmp(&d[off], "CHNK", 4) == 0) {
                    for (uint32_t i = 0; i < U32(d, off + 8); ++i) {
                        size_t cc = off + 16 + size_t(i) * 16;
                        if (cc + 16 > off + size || memcmp(&d[cc], "HRDT", 4) != 0) continue;
                        size_t h = off + U32(d, cc + 4);
                        uint32_t n = U32(d, h);
                        for (uint32_t gi = 0; gi < n && h + 8 + 4 * size_t(gi) <= d.size(); ++gi) {
                            size_t rec = h + U32(d, h + 4 + 4 * size_t(gi));
                            if (rec + 0x18 <= d.size() && memcmp(&d[rec], "AWHF", 4) == 0)
                                dynamicModels.push_back(U32(d, rec + 0x14));
                        }
                    }
                }
            }
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

bool CityGround::Add(const std::vector<uint8_t>& d, size_t off, size_t size)
{
    // 0x4ea130 accepts "THD v3.0" (2 / 3: other versions, not loaded).
    if (off + size > d.size() || size < 0x50 || memcmp(&d[off + 4], "THD v3.0", 8) != 0) return false;
    Chunk c;
    c.d.assign(d.begin() + ptrdiff_t(off), d.begin() + ptrdiff_t(off + size));
    c.nodes = int32_t(U32(c.d, 0xc));
    c.cols = int32_t(U32(c.d, 0x10));
    c.rows = int32_t(U32(c.d, 0x14));
    c.ox = F32(c.d, 0x18);
    c.oz = F32(c.d, 0x20);
    c.sx = F32(c.d, 0x24);
    c.sz = F32(c.d, 0x28);
    c.ix = F32(c.d, 0x2c);
    c.iz = F32(c.d, 0x30);
    uint32_t* t[7] = { &c.types, &c.words, &c.pairs, &c.layers, &c.lines, &c.leaves, &c.planes };
    for (int k = 0; k < 7; ++k) {
        *t[k] = U32(c.d, 0x34 + 4 * size_t(k));
        if (*t[k] > size) return false;
    }
    if (c.cols <= 0 || c.rows <= 0 || c.nodes < c.cols * c.rows || c.types + size_t(c.nodes + 1) / 2 > size ||
        c.words + size_t(c.nodes) * 2 > size)
        return false;
    chunks_.push_back(std::move(c));
    return true;
}

bool CityGround::Query(const Chunk& c, float x, float y, float z, float& height, uint32_t& plane) const
{
    const std::vector<uint8_t>& d = c.d;
    auto type = [&](uint32_t n) {
        if (n >= uint32_t(c.nodes)) return 0;
        uint8_t b = d[c.types + n / 2];
        return int(n % 2 == 0 ? b >> 4 : b & 0xf);
    };
    auto word = [&](uint32_t n) { return uint32_t(d[c.words + 2 * n] | d[c.words + 2 * n + 1] << 8); };
    auto planeHeight = [&](uint32_t i, float lx, float lz, float& h) {
        size_t p = c.planes + size_t(i) * 20;
        if (p + 16 > d.size()) return false;
        float b = F32(d, p + 4);
        if (b == 0.0f) return false;
        h = (F32(d, p + 12) - F32(d, p) * lx - F32(d, p + 8) * lz) / b;
        return true;
    };
    // 0x4ea170: the square, then the split nodes.
    float lx = x - c.ox, lz = z - c.oz;
    if (!(lx >= 0 && lx < float(c.cols) * c.sx && lz >= 0 && lz < float(c.rows) * c.sz)) return false;
    float fx = c.ix * lx, fz = c.iz * lz;
    int cx = int(fx), cz = int(fz);
    fx -= float(cx);
    fz -= float(cz);
    uint32_t n = uint32_t(c.cols * cz + cx);
    int t = type(n);
    bool splitX = true;
    for (int depth = 0; t == 0xe && depth < 32; ++depth) {
        size_t pair = c.pairs + size_t(word(n)) * 8;
        if (pair + 8 > d.size()) return false;
        float& f = splitX ? fx : fz;
        if (f < 0.5f) {
            n = U32(d, pair);
            f += f;
        } else {
            n = U32(d, pair + 4);
            f = (f - 0.5f) * 2.0f;
        }
        splitX = !splitX;
        t = type(n);
    }
    if (t == 0 || t == 0xe) return false;
    if (t == 0xf) {
        plane = word(n);
        return planeHeight(plane, lx, lz, height);
    }
    // n stacked layers; 0x4e9ef0 chooses.
    const float kNone = -500.0f;
    float best = kNone;
    bool found = false;
    for (int k = 0; k < t; ++k) {
        size_t entry = c.layers + (size_t(word(n)) + size_t(k)) * 8;
        if (entry + 8 > d.size()) return found;
        size_t first = c.lines + U32(d, entry), leaves = c.leaves + U32(d, entry + 4);
        size_t line = first;
        uint32_t pi = 0;
        bool leaf = false;
        for (int steps = 0; steps < 256 && line + 8 <= d.size(); ++steps) {
            int16_t a, b, cc;
            memcpy(&a, &d[line], 2);
            memcpy(&b, &d[line + 2], 2);
            memcpy(&cc, &d[line + 4], 2);
            float v = float(b) * fz + float(a) * fx + float(cc);
            int8_t child = int8_t(v > -1e-5f ? d[line + 6] : d[line + 7]);
            if (child >= 0) {
                size_t q = leaves + size_t(child) * 2;
                if (q + 2 > d.size()) break;
                pi = uint32_t(d[q] | d[q + 1] << 8);
                leaf = true;
                break;
            }
            line = first + size_t(-int(child)) * 8;
        }
        float h;
        if (!leaf || !planeHeight(pi, lx, lz, h) || h == kNone) continue;
        float top = y + 0.5f;
        if ((best < h && h < top) || top < best || best == kNone) {
            best = h;
            plane = pi;
            found = true;
        }
    }
    if (found) height = best;
    return found;
}

bool CityGround::Probe(float x, float y, float z, float& height, float normal[3], int* surface) const
{
    for (const Chunk& c : chunks_) {
        uint32_t plane = 0;
        if (!Query(c, x, y, z, height, plane)) continue;
        size_t p = c.planes + size_t(plane) * 20;
        for (int k = 0; k < 3; ++k) normal[k] = F32(c.d, p + 4 * size_t(k));
        if (surface) *surface = int(c.d[p + 16] | (c.d[p + 17] << 8));
        return true;
    }
    return false;
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
