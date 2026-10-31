// The car's damage marks (marks.h; BOOT_TO_MENU.md section 92).
#include "marks.h"

#include <cmath>
#include <cstring>

namespace {

const float kWeightScale = 1.5259021893143654e-05f;  // 0x78e930: 1 / 65535
const float kNormalScale = 0.003921568859368563f;    // 0x78e934: 1 / 255
const float kXyScale = 0.005859375f;                 // 0x78eaac
const float kZScale = 0.00732421875f;                // 0x78eab0
const float kTiny = 9.999999439624929e-11f;          // 0x78eb28
const float kLift = 0.005f;                          // 0x3ba3d70a: along the normal
const float kHalf = 0.0375f;                         // 0x78ea40: the quad's half size
const float kGlassScale = 3.0f;                      // 0x78e964
const float kMoved = 0.125f;                         // 0x78e92c: 0x4f5890's threshold
const float kPickRange = 0.140625f;                  // 0x78f194: 0.375 m squared
const float kShapeRange = 2.25f;                     // 0x78f198: (radius x 1.5) squared
const float kHitDamage = 0.04629629850387573f;       // 0x78f190: record +0x24 per hit

int Side(const VehicleData::MarkPoint& q) { return q.normal[3] != 0 ? 1 : 0; }

// The row vector (x, y, z, w) times the matrix (D3DXVec4Transform, 0x605106).
void Transform4(const float p[4], const float m[16], float out[4])
{
    float r[4];
    for (int j = 0; j < 4; ++j) r[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + p[3] * m[12 + j];
    memcpy(out, r, sizeof(r));
}

// 0x417ea0: v x 1 / sqrt(((x x + y y) + z z) + w w), all four components.
void Normalise4(float v[4])
{
    const float s = 1.0f / sqrtf(((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]) + v[3] * v[3]);
    for (int k = 0; k < 4; ++k) v[k] = s * v[k];
}

// 0x41ea80 (0x407640): a x b, w 0.
void Cross(const float a[4], const float b[4], float out[4])
{
    const float x = a[1] * b[2] - a[2] * b[1];
    const float y = a[2] * b[0] - a[0] * b[2];
    const float z = a[0] * b[1] - a[1] * b[0];
    out[0] = x, out[1] = y, out[2] = z, out[3] = 0;
}

// 0x41b9b0
bool Tiny4(const float v[4]) { return v[3] * v[3] + v[2] * v[2] + v[1] * v[1] + v[0] * v[0] < kTiny; }

}  // namespace

void MarkPosition(uint32_t v, float out[4])
{
    const int32_t s = int32_t(v);
    out[0] = float(int32_t(uint32_t(s) << 22) >> 22) * kXyScale;
    out[1] = float(int32_t(uint32_t(s) << 12) >> 22) * kXyScale;
    out[2] = float(s >> 20) * kZScale;
    out[3] = 1.0f;
}

void MarkNormal(const uint8_t b[3], float out[4])
{
    for (int k = 0; k < 3; ++k) {
        const float f = float(b[k]) * kNormalScale - 0.5f;
        out[k] = f + f;
    }
    out[3] = 0;
}

void MarkRange(const VehicleData& v, int m, uint32_t& first, uint32_t& end)
{
    first = end = 0;
    if (m < 0 || size_t(m) >= v.markFirst.size()) return;
    first = v.markFirst[size_t(m)];
    end = size_t(m) + 1 < v.markFirst.size() ? v.markFirst[size_t(m) + 1] : uint32_t(v.markPoints.size());
    if (end > v.markPoints.size()) end = uint32_t(v.markPoints.size());
    if (first > end) first = end;
}

void GlassStep(DamageRecord& r, int vehicleId)
{
    // 0x4f5190 (esi = the record): the worst stage up to 3; at 2 the glass
    // cracks (+0x30 1), at 3 it breaks (+0x30 2) unless 0x5140d0 says the
    // vehicle's glass never breaks (ids 9, 14, 57, 89, 90, 91: table
    // 0x5140fc). Not ported: slot +0x1f64 (record flag bit 1) and the
    // breaking effect 0x50b100 (flag bit 1, not yet broken).
    if (r.worst < 3) ++r.worst;
    if (r.worst > 1 && int8_t(r.stage) < 1) {
        if (r.worst < 2) r.worst = 2;
        r.stage = 1;
    }
    const bool unbreakable = vehicleId == 9 || vehicleId == 14 || vehicleId == 57 || vehicleId == 89 ||
                             vehicleId == 90 || vehicleId == 91;
    if (r.worst > 2 && int8_t(r.stage) < 2 && !unbreakable) {
        if (r.worst < 3) r.worst = 3;
        r.stage = 2;
    }
}

MarkPool::MarkPool()
{
    // 0x50e239: 16 hole buffers, then 4 glass buffers; each side's free
    // stack takes them in order and gives the last one pushed first.
    for (int side = 0; side < 2; ++side) {
        const int n = side == 0 ? 16 : 4;
        for (int i = 0; i < n; ++i) {
            MarkBuffer b;
            b.side = side;
            for (MarkVertex& x : b.v) {
                memset(&x, 0, sizeof(x));
                for (float& c : x.colour) c = 1.0f;
            }
            free[side].push_back(int(buffers.size()));
            buffers.push_back(b);
        }
    }
}

int MarkPool::Take(int side)
{
    if (free[side].empty()) return -1;
    const int i = free[side].back();
    free[side].pop_back();
    buffers[size_t(i)].count = buffers[size_t(i)].at = 0;
    return i;
}

void MarkPool::Give(int buffer) { free[buffers[size_t(buffer)].side].push_back(buffer); }

void CarMarks::Init(const VehicleData& v)
{
    memset(region, 0xff, sizeof(region));
    memset(marks, 0, sizeof(marks));
    memset(last, 0, sizeof(last));
    memset(perSide, 0, sizeof(perSide));
    memset(anchor, 0, sizeof(anchor));
    for (auto& a : buffer)
        for (auto& b : a)
            for (int& c : b) c = -1;
    used = 0;
    active = false;
    meshes_ = int(v.markFirst.size());
    // 0x4f5480 (edx = the damage entries): anchor = -(R t) of entry k, w 1.
    for (size_t k = 0; k < v.damage.size() && k < size_t(kMeshes); ++k) {
        const float* e = v.damage[k].matrix;
        anchor[k][0] = (e[0] * e[12] + e[1] * e[13] + e[2] * e[14]) * -1.0f;
        anchor[k][1] = (e[4] * e[12] + e[5] * e[13] + e[6] * e[14]) * -1.0f;
        anchor[k][2] = (e[8] * e[12] + e[9] * e[13] + e[10] * e[14]) * -1.0f;
        anchor[k][3] = 1.0f;
    }
}

void CarMarks::Free(const VehicleData& v, MarkPool& pool, int m, int side, int reg)
{
    // 0x4f5b10: the buffer back to the pool, the region's points free.
    int& b = buffer[m][side][reg];
    if (b >= 0) {
        pool.Give(b);
        b = -1;
        --used;
        --perSide[m][side];
    }
    marks[m][side][reg] = 0;
    uint32_t first, end;
    MarkRange(v, m, first, end);
    for (uint32_t j = end; j-- > first;)
        if (Side(v.markPoints[j]) == side && region[j] == reg) region[j] = 0xff;
}

bool CarMarks::GiveBack(const VehicleData& v, MarkPool& pool, const float point[4], const float car[16])
{
    // 0x50f270 asks the farthest vehicle with marks (vtbl+0xd8 = 0x4f6670 ->
    // 0x4f5c30; here the only car): its mesh and side with buffers whose
    // anchor (x the car) lies farthest from the point gives the buffer of
    // its last region in use. The car must lie farther than 0 from the
    // point (0x50f270's farthest test starts at 0).
    {
        float d = 0;
        for (int k = 0; k < 4; ++k) d += (car[12 + k] - point[k]) * (car[12 + k] - point[k]);
        if (!(d > 0)) return false;
    }
    float best = 0;
    int bm = -1, bs = -1;
    for (int m = 0; m < kMeshes; ++m)
        for (int s = 0; s < 2; ++s) {
            if (perSide[m][s] == 0) continue;
            float a[4];
            Transform4(anchor[m], car, a);
            const float d = (a[0] - point[0]) * (a[0] - point[0]) + (a[1] - point[1]) * (a[1] - point[1]) +
                            (a[2] - point[2]) * (a[2] - point[2]) + (a[3] - point[3]) * (a[3] - point[3]);
            if (best < d) best = d, bm = m, bs = s;
        }
    if (bm < 0) return false;
    uint32_t found = 0;
    int reg = 0;
    for (; reg < 8; ++reg) {
        if (perSide[bm][bs] <= found) break;
        if (buffer[bm][bs][reg] >= 0) ++found;
    }
    Free(v, pool, bm, bs, reg - 1);
    return true;
}

bool CarMarks::Place(const VehicleData& v, MarkPool& pool, int p, int m, DamageRecord* r, const float car[16])
{
    if (p < 0 || size_t(p) >= v.markPoints.size() || p >= kPoints || m < 0 || m >= kMeshes) return false;
    if (region[p] != 0xff) return false;
    const VehicleData::MarkPoint& q = v.markPoints[size_t(p)];
    const int side = Side(q);
    // The region: the first with fewer than 16 marks of this side; all full
    // -> the region of the placed point of the mesh (same side) farthest
    // from this one, emptied.
    int reg = -1;
    for (int b = 0; b < 8; ++b)
        if (marks[m][side][b] < 16) {
            reg = b;
            break;
        }
    if (reg < 0) {
        float rp[4];
        MarkPosition(q.rest, rp);
        uint32_t first, end;
        MarkRange(v, m, first, end);
        float best = 0;
        uint8_t pick = 0;
        for (uint32_t j = end; j-- > first;) {
            if (Side(v.markPoints[j]) != side || region[j] == 0xff) continue;
            float o[4];
            MarkPosition(v.markPoints[j].rest, o);
            const float dx = o[0] - rp[0], dy = o[1] - rp[1], dz = o[2] - rp[2];
            const float d = (dy * dy + dz * dz) + dx * dx;
            if (best < d) best = d, pick = region[j];
        }
        if (pick > 7) return false;
        Free(v, pool, m, side, pick);
        reg = pick;
    }
    int& b = buffer[m][side][reg];
    if (b < 0) {
        float a[4];
        Transform4(anchor[m], car, a);
        int got = pool.Take(side);
        if (got < 0 && GiveBack(v, pool, a, car)) got = pool.Take(side);
        if (got < 0) return false;
        b = got;
        ++used;
        ++perSide[m][side];
    }
    if (side == 1 && r) GlassStep(*r, v.id);
    region[p] = uint8_t(reg);
    ++marks[m][side][reg];

    // The position and normal between rest and dented (0x50c028..).
    float t = 0;
    if (r) {
        const float w = float(q.weight) * kWeightScale;
        t = (1.0f - w) * r->half[0] + w * r->half[1];
    }
    const float u = 1.0f - t;
    float rest[4], dent[4], nr[4], nd[4], P[4], N[4];
    MarkPosition(q.dented, dent);
    MarkPosition(q.rest, rest);
    for (int k = 0; k < 4; ++k) P[k] = dent[k] * t + rest[k] * u;
    MarkNormal(q.normal + 4, nd);
    MarkNormal(q.normal, nr);
    for (int k = 0; k < 4; ++k) N[k] = nd[k] * t + nr[k] * u;
    P[3] = 0;
    N[3] = 0;
    if (!((N[0] * N[0] + N[2] * N[2]) + N[1] * N[1] >= kTiny)) {
        // (the game has the region taken and counted by now; so has the port)
        return true;
    }
    Normalise4(N);
    for (int k = 0; k < 4; ++k) P[k] = P[k] + N[k] * kLift;
    static const float kUp[4] = { 0, 1, 0, 0 }, kFront[4] = { 0, 0, 1, 0 };
    float T1[4], T2[4];
    Cross(N, kUp, T1);
    if (Tiny4(T1)) {
        Cross(N, kFront, T1);
        if (Tiny4(T1)) return true;
    }
    Normalise4(T1);
    Cross(N, T1, T2);
    T2[3] = 0;
    T1[3] = 0;
    const float h = (side ? kGlassScale : 1.0f) * kHalf;
    for (int k = 0; k < 4; ++k) T1[k] = T1[k] * h;
    for (int k = 0; k < 4; ++k) T2[k] = T2[k] * h;

    // 0x4f3650: the quad at the write index; the index wraps at 16, the
    // count stops at 15.
    MarkBuffer& buf = pool.buffers[size_t(b)];
    MarkVertex* o = &buf.v[buf.at * 4];
    const float corner[4][2] = { { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
    const float uv[4][2] = { { 1, 1 }, { 0, 1 }, { 1, 0 }, { 0, 0 } };
    for (int c = 0; c < 4; ++c) {
        float a[3];
        for (int k = 0; k < 3; ++k) a[k] = corner[c][0] > 0 ? P[k] + T1[k] : P[k] - T1[k];
        for (int k = 0; k < 3; ++k) o[c].pos[k] = corner[c][1] > 0 ? a[k] + T2[k] : a[k] - T2[k];
        for (int k = 0; k < 3; ++k) o[c].normal[k] = N[k];
        o[c].uv[0] = uv[c][0];
        o[c].uv[1] = uv[c][1];
        for (float& x : o[c].colour) x = 1.0f;
    }
    if (++buf.at >= MarkBuffer::kQuads) buf.at = 0;
    if (++buf.count >= MarkBuffer::kQuads) buf.count = MarkBuffer::kQuads - 1;
    return true;
}

void CarMarks::Replace(const VehicleData& v, MarkPool& pool, int m, DamageRecord& r, const float car[16])
{
    if (!active || m < 0 || m >= kMeshes) return;
    if (!(fabsf(r.half[0] - last[m][0]) > kMoved) && !(fabsf(r.half[1] - last[m][1]) > kMoved)) return;
    r.worst = 0;
    memset(marks[m], 0, sizeof(marks[m]));
    // (the game also zeroes the buffers-per-side counts +0x37c / +0x380 here
    // while the buffers stay: kept, so 0x4f5c30 skips this mesh afterwards
    // and a later free wraps the count below 0, as in the game)
    perSide[m][0] = perSide[m][1] = 0;
    last[m][0] = r.half[0];
    last[m][1] = r.half[1];
    for (int s = 0; s < 2; ++s)
        for (int reg = 0; reg < 8; ++reg)
            if (buffer[m][s][reg] >= 0) {
                MarkBuffer& b = pool.buffers[size_t(buffer[m][s][reg])];
                b.count = b.at = 0;
            }
    uint32_t first, end;
    MarkRange(v, m, first, end);
    for (int s = 0; s < 2; ++s)
        for (int reg = 0; reg < 8; ++reg) {
            if (buffer[m][s][reg] < 0) continue;
            for (uint32_t j = first; j < end; ++j)
                if (region[j] == reg) {
                    region[j] = 0xff;
                    Place(v, pool, int(j), m, &r, car);
                }
        }
}

int CarMarks::Hit(const VehicleData& v, MarkPool& pool, std::vector<DamageRecord>& records,
                  const std::vector<int>& recordPart, const float point[3], const float car[16])
{
    if (v.markPoints.empty()) return -1;
    active = true;  // 0x4f6780 (the world's 3 marks objects are not limited here)
    float bestFree = 10000.0f, bestPlaced = 10000.0f;
    int pickFree = -1, pickPlaced = -1, meshFree = -1, meshPlaced = -1;
    for (size_t k = 0; k < records.size() && k < recordPart.size(); ++k) {
        const int pi = recordPart[k];
        if (pi < 0 || size_t(pi) >= v.parts.size()) continue;
        const VehicleData::Part& part = v.parts[size_t(pi)];
        const DamageRecord& rec = records[k];
        if (rec.state >= 3) continue;
        if (part.shapes < 1 || k >= v.damage.size()) continue;
        // A part with a body of its own (hinged or loose) is tested in its
        // body's frame (0x508773..): not ported, such parts get no marks.
        if (rec.body != 0) continue;
        // C: the translation of (shape matrix x the entry's rigid inverse).
        const float* e = v.damage[k].matrix;
        float inv[16] = { e[0], e[4], e[8], e[3], e[1], e[5], e[9], e[7], e[2], e[6], e[10], e[11], 0, 0, 0, 1 };
        for (int i = 0; i < 3; ++i) inv[12 + i] = (e[12] * e[4 * i] + e[13] * e[4 * i + 1] + e[14] * e[4 * i + 2]) * -1.0f;
        const float* s = part.shapeMatrix;
        float C[3];
        for (int j = 0; j < 3; ++j) C[j] = s[12] * inv[j] + s[13] * inv[4 + j] + s[14] * inv[8 + j] + s[15] * inv[12 + j];
        const float vx = C[0] - point[0], vy = C[1] - point[1], vz = C[2] - point[2];
        const float dist = (vx * vx + vz * vz) + vy * vy;
        const float rad = sqrtf((part.shapeSize[0] * part.shapeSize[0] + part.shapeSize[1] * part.shapeSize[1]) +
                                part.shapeSize[2] * part.shapeSize[2]);
        if (rad * rad * kShapeRange < dist) continue;
        uint32_t first, end;
        MarkRange(v, int(k), first, end);
        for (uint32_t j = first; j < end; ++j) {
            float o[4];
            MarkPosition(v.markPoints[j].rest, o);
            const float dx = o[0] - point[0], dy = o[1] - point[1], dz = o[2] - point[2];
            const float d = (dy * dy + dz * dz) + dx * dx;
            if (!(d < bestFree) || !(d < kPickRange)) continue;
            if (region[j] == 0xff) {
                bestFree = d, pickFree = int(j), meshFree = int(k);
            } else if (d < bestPlaced) {
                bestPlaced = d, pickPlaced = int(j), meshPlaced = int(k);
            }
        }
    }
    if (pickFree >= 0) {
        DamageRecord& r = records[size_t(meshFree)];
        r.f24 = r.f24 + kHitDamage;
        Place(v, pool, pickFree, meshFree, &r, car);
        return pickFree;
    }
    if (pickPlaced >= 0) {
        DamageRecord& r = records[size_t(meshPlaced)];
        r.f24 = r.f24 + kHitDamage;
        if (Side(v.markPoints[size_t(pickPlaced)])) GlassStep(r, v.id);
    }
    return -1;
}

void CarMarks::Draws(const VehicleData& v, const MarkPool& pool, const std::vector<DamageRecord>& records,
                     const std::vector<int>& recordPart, std::vector<MarkDraw>& out) const
{
    out.clear();
    if (!active) return;
    for (int m = 0; m < kMeshes && size_t(m) < records.size(); ++m) {
        // 0x4fe3e0 / 0x4fe6cc: glass only while not broken (+0x30 < 2) and
        // the drawn dent's two halves average at most 0.5 (each clamped to
        // [0, 1] when the part shows dents, else 0).
        const DamageRecord& r = records[size_t(m)];
        float c[2] = {};
        const int pi = size_t(m) < recordPart.size() ? recordPart[size_t(m)] : -1;
        if (pi >= 0 && size_t(pi) < v.parts.size() &&
            DamageShowsDent(v.parts[size_t(pi)].type, v.parts[size_t(pi)].id, v.parts[size_t(pi)].flags8))
            for (int k = 0; k < 2; ++k) c[k] = r.half[k] < 0 ? 0.0f : r.half[k] > 1 ? 1.0f : r.half[k];
        const bool glass = int8_t(r.stage) < 2 && (c[1] + c[0]) * 0.5f <= 0.5f;
        for (int s = 0; s < 2; ++s) {
            if (s == 1 && !glass) continue;
            for (int reg = 0; reg < 8; ++reg) {
                const int b = buffer[m][s][reg];
                if (b < 0 || pool.buffers[size_t(b)].count == 0) continue;
                MarkDraw d;
                d.record = m;
                d.side = s;
                d.buffer = &pool.buffers[size_t(b)];
                out.push_back(d);
            }
        }
    }
}
