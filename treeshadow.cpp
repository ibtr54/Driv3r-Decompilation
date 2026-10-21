#include "treeshadow.h"

#include <cmath>
#include <cstring>

namespace {

struct V4
{
    float x = 0, y = 0, z = 0;
};

V4 Add(V4 a, V4 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V4 Sub(V4 a, V4 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V4 Scale(V4 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
V4 Cross(V4 a, V4 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float Dot(V4 a, V4 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

const float kRadius = 200.0f;        // 0x561845: 0x43480000
const int kMaxTrees = 0x100;         // 0x561836
const float kRebuild = 4444.4443f;   // 0x78f0dc, squared distance
const float kScale = 0.3153f;        // 0x78f0d4
const float kLift = 0.1f;            // 0x78e9ac

// One tree in the cache layout of 0x562070: ten points and their normals.
struct TreeGround
{
    V4 point[10], normal[10];
    bool fromRay = false;  // placed by the physics ray
};

// 0x562070 (the cache of 256 entries by position is left out: the result
// is the same). False when there is no ground (the tree is skipped).
bool Ground(const CityTree& t, V4 d, const TreeShadows::Probe& probe, const TreeShadows::Ray& ray, TreeGround& g)
{
    const V4 p0 = { t.position[0], t.position[1], t.position[2] };
    float start = p0.y + 5.0f;  // 0x78e954
    float height = 0, n[3] = {};
    bool flat = false;
    if (!probe(p0.x, start, p0.z, height, n)) {
        // The physics ray 0x5a03f0 ([0x7e9bb0], object mask 0x20): 17 units
        // (0x41880000) straight down from p + d, 5 above the tree (type
        // 0x1d: from p + 4 d at the tree's height); no hit: no shadow. The
        // ground is the hit's height (start - distance) with its normal.
        float x = p0.x + d.x, y = start, z = p0.z + d.z;
        if (t.type == 0x1d) {
            y -= 5.0f;
            x += d.x * 3.0f;
            z += d.z * 3.0f;
        }
        const float from[3] = { x, y, z }, down[3] = { 0, -1, 0 };
        float dist = 0;
        if (!ray || !ray(from, down, 17.0f, dist, n)) return false;
        height = y - dist;
        flat = true;
        g.fromRay = true;
    }
    float h = std::fabs(start - height);
    h += h;
    V4 nn = { n[0], n[1], n[2] };
    V4 p = { p0.x, height, p0.z };
    // The across axis: -(n x d), or n x (0, 0, 1) when that is too short;
    // the along axis: n x across (along d on flat ground). Both x h x 0.3153.
    V4 a = Scale(Cross(nn, d), -1.0f);
    if (Dot(a, a) < 1e-10f) {
        a = Cross(nn, { 0, 0, 1 });
        if (Dot(a, a) < 1e-10f) return false;
    }
    a = Scale(a, 1.0f / std::sqrt(Dot(a, a)));
    V4 b = Cross(nn, a);
    float s = h * kScale;
    a = Scale(a, s);
    b = Scale(b, s);
    if (t.type == 0x11) {
        // A palm: five rows along b, half size.
        a = Scale(a, 0.5f);
        b = Scale(b, 0.5f);
        for (int k = 0; k < 5; ++k) {
            V4 row = Add(p, Scale(b, float(k)));
            g.point[2 * k] = Sub(row, a);
            g.point[2 * k + 1] = Add(row, a);
        }
    } else {
        // A fan around p moved by b (0x4064c0; type 0x1d: by b / 2), checked
        // against the original's vertices.
        p = Add(p, t.type == 0x1d ? Scale(b, 0.5f) : b);
        g.point[0] = p;
        g.point[1] = Add(Add(p, a), b);
        g.point[2] = Add(p, a);
        g.point[3] = Sub(Add(p, a), b);
        g.point[4] = Sub(p, b);
        g.point[5] = Sub(Sub(p, a), b);
        g.point[6] = Sub(p, a);
        g.point[7] = Add(Sub(p, a), b);
        g.point[8] = Add(p, b);
        g.point[9] = Add(p, b);
    }
    for (int k = 0; k < 10; ++k) {
        V4& q = g.point[k];
        V4 qn = nn;
        if (!flat && t.type != 0x1d) {
            // Each point on the ground below it, probed from 10 above
            // (0x78e9b8); none: the tree's own ground.
            float qh, qnv[3];
            if (probe(q.x, q.y + 10.0f, q.z, qh, qnv)) {
                q.y = qh;
                qn = { qnv[0], qnv[1], qnv[2] };
            } else {
                q.y = height;
            }
        } else {
            q.y = height;
        }
        g.normal[k] = qn;
        q = Add(q, Scale(qn, kLift));
    }
    return true;
}

// 0x561750: one written vertex. The atlas row of the type (0x11 0.5, 0x12
// 0.25, 0x13 0, 0x14 0.75) is added to a2 x 0.25 except for type 0x1d; the
// texture coordinate is (1 - a3, that).
void Write(std::vector<TreeShadowVertex>& out, const TreeGround& g, int k, float a2, float a3, int type)
{
    TreeShadowVertex v;
    v.position[0] = g.point[k].x;
    v.position[1] = g.point[k].y;
    v.position[2] = g.point[k].z;
    v.normal[0] = g.normal[k].x;
    v.normal[1] = g.normal[k].y;
    v.normal[2] = g.normal[k].z;
    float v2 = a2;
    if (type != 0x1d) {
        v2 *= 0.25f;
        if (type == 0x11) v2 += 0.5f;
        else if (type == 0x12) v2 += 0.25f;
        else if (type == 0x14) v2 += 0.75f;
    }
    v.uv[0] = 1.0f - a3;
    v.uv[1] = v2;
    for (float& c : v.colour) c = 1.0f;
    out.push_back(v);
}

// 0x561800: the written vertices of one tree.
void WriteTree(std::vector<TreeShadowVertex>& out, const TreeGround& g, int type)
{
    struct W { int point; float a2, a3; };
    static const W kPalm[16] = {
        { 0, 0, 1 }, { 1, 1, 1 }, { 2, 0, 0.75f }, { 3, 1, 0.75f }, { 2, 0, 0.75f }, { 3, 1, 0.75f },
        { 4, 0, 0.5f }, { 5, 1, 0.5f }, { 4, 0, 0.5f }, { 5, 1, 0.5f }, { 6, 0, 0.25f }, { 7, 1, 0.25f },
        { 6, 0, 0.25f }, { 7, 1, 0.25f }, { 8, 0, 0 }, { 9, 1, 0 },
    };
    static const W kFan[16] = {
        { 0, 0.5f, 0.5f }, { 2, 0, 0.5f }, { 8, 0.5f, 0 }, { 1, 0, 0 },
        { 0, 0.5f, 0.5f }, { 4, 0.5f, 1 }, { 2, 0, 0.5f }, { 3, 0, 1 },
        { 0, 0.5f, 0.5f }, { 6, 1, 0.5f }, { 4, 0.5f, 1 }, { 5, 1, 1 },
        { 0, 0.5f, 0.5f }, { 8, 0.5f, 0 }, { 6, 1, 0.5f }, { 7, 1, 0 },
    };
    static const W kRound[4] = { { 5, 1, 1 }, { 3, 0, 1 }, { 7, 1, 0 }, { 1, 0, 0 } };
    const W* w = type == 0x11 ? kPalm : type == 0x1d ? kRound : kFan;
    int n = type == 0x1d ? 4 : 16;
    for (int k = 0; k < n; ++k) Write(out, g, w[k].point, w[k].a2, w[k].a3, type);
}

// The ATRD query ([0x8ac358] vtbl+0x14, kind 14): the chunks in file order,
// in each the implicit kd tree of its records (node (lo + hi) / 2, split by
// the record's axis byte, the side of the point first); a tree is returned
// when it lies within the radius in x / z.
void Query(const CityTreeCell& cell, float qx, float qz, int lo, int hi, std::vector<const CityTree*>& out)
{
    if (lo >= hi || int(out.size()) >= kMaxTrees) return;
    int m = (lo + hi) / 2;
    const CityTree& t = cell.trees[size_t(m)];
    float dx = t.position[0] - qx, dz = t.position[2] - qz;
    if (dx * dx + dz * dz < kRadius * kRadius) out.push_back(&t);
    if (t.axis != 0 && t.axis != 2) return;
    float q = t.axis == 0 ? qx : qz, c = t.position[t.axis];
    bool leftFirst = q < c;
    for (int side = 0; side < 2; ++side) {
        bool left = (side == 0) == leftFirst;
        if (side == 1 && std::fabs(q - c) >= kRadius) continue;  // nothing within the radius there
        if (left) Query(cell, qx, qz, lo, m, out);
        else Query(cell, qx, qz, m + 1, hi, out);
    }
}

}  // namespace

bool TreeShadows::Update(const CityPlacement& city, const float point[2], const float light[3], const Probe& probe,
                         const Ray& ray)
{
    if (built_) {
        float dx = point[0] - last_[0], dz = point[1] - last_[1];
        if (dx * dx + dz * dz < kRebuild) return false;
    }
    built_ = true;
    last_[0] = point[0];
    last_[1] = point[1];
    // Mood +0x170 (0x472fc0): the light's horizontal direction tilted down by
    // SUN_ELEVATION (Mood::sun; read in the running game).
    const V4 d = { light[0], light[1], light[2] };
    std::vector<const CityTree*> query;
    for (const CityTreeCell& cell : city.treeCells) {
        float x0 = cell.corner[0], z0 = cell.corner[1];
        float cx = std::fmax(x0, std::fmin(point[0], x0 + cell.size[0]));
        float cz = std::fmax(z0, std::fmin(point[1], z0 + cell.size[1]));
        if ((cx - point[0]) * (cx - point[0]) + (cz - point[1]) * (cz - point[1]) >= kRadius * kRadius) continue;
        Query(cell, point[0], point[1], 0, int(cell.trees.size()), query);
    }
    found = int(query.size());
    noGround = 0;
    rayGround = 0;
    std::vector<TreeShadowVertex> written[3];
    for (int b = 0; b < 3; ++b) trees[b] = 0;
    // Buffers 0 and 1: types 0x11..0x14, 0x80 trees each (buffer 1 once 0
    // is full); buffer 2: type 0x1d, 0x40 trees (a second pass).
    int cur = 0;
    for (const CityTree* t : query) {
        if (t->type < 0x11 || t->type > 0x14) continue;
        if (cur == 0 && trees[0] >= 0x80) cur = 1;
        if (trees[cur] >= 0x80) continue;
        TreeGround g;
        if (!Ground(*t, d, probe, ray, g)) { ++noGround; continue; }
        rayGround += g.fromRay;
        WriteTree(written[cur], g, t->type);
        ++trees[cur];
    }
    for (const CityTree* t : query) {
        if (t->type != 0x1d || trees[2] >= 0x40) continue;
        TreeGround g;
        if (!Ground(*t, d, probe, ray, g)) { ++noGround; continue; }
        rayGround += g.fromRay;
        WriteTree(written[2], g, t->type);
        ++trees[2];
    }
    for (int b = 0; b < 3; ++b) {
        list[b].clear();
        const std::vector<TreeShadowVertex>& w = written[b];
        for (size_t q = 0; q + 3 < w.size(); q += 4) {
            const size_t order[6] = { q, q + 1, q + 2, q + 2, q + 1, q + 3 };
            for (size_t i : order) list[b].push_back(w[i]);
        }
        triangles[b] = int(w.size() / 3);
    }
    return true;
}
