// The static collision world (collision.h).
#include "collision.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

uint32_t U32(const std::vector<uint8_t>& d, size_t o)
{
    uint32_t v;
    memcpy(&v, &d[o], 4);
    return v;
}
float F32(const std::vector<uint8_t>& d, size_t o)
{
    float v;
    memcpy(&v, &d[o], 4);
    return v;
}

// Row-vector 4 x 4 product a b.
void Mul(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            out[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k] + a[r * 4 + 3] * b[12 + k];
}

// The ray in the shape's frame (rows 0..2 of m are its axes in the world,
// row 3 its origin).
void ToLocal(const float* m, const float p[3], const float d[3], float lp[3], float ld[3])
{
    const float q[3] = { p[0] - m[12], p[1] - m[13], p[2] - m[14] };
    for (int j = 0; j < 3; ++j) {
        lp[j] = q[0] * m[j * 4] + q[1] * m[j * 4 + 1] + q[2] * m[j * 4 + 2];
        ld[j] = d[0] * m[j * 4] + d[1] * m[j * 4 + 1] + d[2] * m[j * 4 + 2];
    }
}

// Entry distance (may be < 0: the start is inside) and local normal; false:
// no hit in front of the start.
bool RayBox(const float h[3], const float p[3], const float d[3], float& t, float n[3])
{
    float tmin = -1e30f, tmax = 1e30f;
    int axis = -1;
    float sign = 0;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-12f) {
            if (p[i] < -h[i] || p[i] > h[i]) return false;
            continue;
        }
        float t1 = (-h[i] - p[i]) / d[i], t2 = (h[i] - p[i]) / d[i];
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > tmin) {
            tmin = t1;
            axis = i;
            sign = d[i] > 0 ? -1.0f : 1.0f;
        }
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }
    if (tmax < 0 || axis < 0) return false;
    t = tmin;
    n[0] = n[1] = n[2] = 0;
    n[axis] = sign;
    return true;
}

bool RayCylinder(float r, float hy, const float p[3], const float d[3], float& t, float n[3])
{
    float best = 1e30f;
    bool hit = false;
    // Caps.
    if (std::fabs(d[1]) > 1e-12f) {
        for (int s = -1; s <= 1; s += 2) {
            float tc = (s * hy - p[1]) / d[1];
            float x = p[0] + tc * d[0], z = p[2] + tc * d[2];
            if (x * x + z * z <= r * r && tc < best) {
                best = tc;
                n[0] = 0; n[1] = float(s); n[2] = 0;
                hit = true;
            }
        }
    }
    // Side.
    const float a = d[0] * d[0] + d[2] * d[2];
    if (a > 1e-12f) {
        const float b = p[0] * d[0] + p[2] * d[2], c = p[0] * p[0] + p[2] * p[2] - r * r;
        const float disc = b * b - a * c;
        if (disc >= 0) {
            const float s = std::sqrt(disc);
            const float roots[2] = { (-b - s) / a, (-b + s) / a };
            for (float tc : roots) {
                float y = p[1] + tc * d[1];
                if (y >= -hy && y <= hy && tc < best) {
                    best = tc;
                    float x = p[0] + tc * d[0], z = p[2] + tc * d[2];
                    n[0] = x / r; n[1] = 0; n[2] = z / r;
                    hit = true;
                }
            }
        }
    }
    if (!hit) return false;
    // Inside (the start within the cylinder): entry behind the start.
    if (p[0] * p[0] + p[2] * p[2] <= r * r && p[1] >= -hy && p[1] <= hy) best = -1;
    else if (best < 0) return false;
    t = best;
    return true;
}

bool RaySphere(float r, const float p[3], const float d[3], float& t, float n[3])
{
    const float a = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    const float b = p[0] * d[0] + p[1] * d[1] + p[2] * d[2];
    const float c = p[0] * p[0] + p[1] * p[1] + p[2] * p[2] - r * r;
    const float disc = b * b - a * c;
    if (disc < 0 || a <= 0) return false;
    const float s = std::sqrt(disc);
    const float t0 = (-b - s) / a, t1 = (-b + s) / a;
    if (t1 < 0) return false;
    t = t0;
    for (int k = 0; k < 3; ++k) n[k] = (p[k] + t0 * d[k]) / r;
    return true;
}

}  // namespace

void CollisionWorld::Build(const CityPlacement& city)
{
    shapes_.clear();
    objects_.clear();
    cells_.clear();
    for (size_t ii = 0; ii < city.instances.size(); ++ii) {
        const CityInstance& in = city.instances[ii];
        if (!in.HasModel()) continue;
        const uint32_t id = in.model;
        if (std::find(city.dynamicModels.begin(), city.dynamicModels.end(), id) != city.dynamicModels.end()) continue;
        const CityCollisionRegion* reg = nullptr;
        for (const CityCollisionRegion& r : city.collision)
            if (uint32_t(r.region) == (id >> 10) && !r.pdl.empty()) reg = &r;
        if (!reg) continue;
        const CityCollisionRegion::Model* model = nullptr;
        for (const CityCollisionRegion::Model& m : reg->models)
            if (m.id == id) model = &m;
        if (!model) continue;
        const std::vector<uint8_t>& p = reg->pdl;
        const uint32_t entries = U32(p, 0);
        const size_t table = U32(p, 8);
        // One object per PDL entry (0x5a3e40 tests one entry's shape list).
        for (uint32_t e = model->first; e < model->first + model->count && e < entries; ++e) {
            const size_t ent = table + size_t(e) * 16;
            if (ent + 16 > p.size()) break;
            const uint32_t n = U32(p, ent);
            const size_t first = ent + U32(p, ent + 4);
            Object obj;
            obj.instance = int(ii);
            obj.first = shapes_.size();
            obj.bounded = n > 1;
            for (int k = 0; k < 3; ++k) obj.lo[k] = 1e30f, obj.hi[k] = -1e30f;
            for (uint32_t s = 0; s < n; ++s) {
                const size_t sh = first + size_t(s) * 0x60;
                if (sh + 0x60 > p.size()) break;
                Shape shape;
                const uint32_t flags = U32(p, sh + 0x50);
                shape.type = int(flags & 0xff);
                shape.surface = int((flags >> 8) & 0xff);
                for (int k = 0; k < 3; ++k) shape.size[k] = F32(p, sh + 4 * k);
                float local[16];
                for (int k = 0; k < 16; ++k) local[k] = F32(p, sh + 0x10 + 4 * k);
                Mul(local, in.world, shape.world);
                // The world AABB of the shape's bounding box.
                float h[3] = { shape.size[0], shape.size[1], shape.size[2] };
                if (shape.type == 1) h[0] = h[2] = shape.size[0];
                if (shape.type == 2) h[0] = h[1] = h[2] = shape.size[0];
                for (int k = 0; k < 3; ++k) {
                    const float c = shape.world[12 + k];
                    const float e2 = std::fabs(shape.world[k]) * h[0] + std::fabs(shape.world[4 + k]) * h[1] +
                                     std::fabs(shape.world[8 + k]) * h[2];
                    obj.lo[k] = std::min(obj.lo[k], c - e2);
                    obj.hi[k] = std::max(obj.hi[k], c + e2);
                }
                shapes_.push_back(shape);
            }
            obj.count = shapes_.size() - obj.first;
            if (obj.count) objects_.push_back(obj);
        }
    }
    // The grid.
    if (objects_.empty()) return;
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (const Object& o : objects_) {
        lo[0] = std::min(lo[0], o.lo[0]); hi[0] = std::max(hi[0], o.hi[0]);
        lo[1] = std::min(lo[1], o.lo[2]); hi[1] = std::max(hi[1], o.hi[2]);
    }
    x0_ = int(std::floor(lo[0] / kCell));
    z0_ = int(std::floor(lo[1] / kCell));
    nx_ = int(std::floor(hi[0] / kCell)) - x0_ + 1;
    nz_ = int(std::floor(hi[1] / kCell)) - z0_ + 1;
    cells_.assign(size_t(nx_) * size_t(nz_), {});
    for (uint32_t i = 0; i < objects_.size(); ++i) {
        const Object& o = objects_[i];
        const int ax = int(std::floor(o.lo[0] / kCell)) - x0_, bx = int(std::floor(o.hi[0] / kCell)) - x0_;
        const int az = int(std::floor(o.lo[2] / kCell)) - z0_, bz = int(std::floor(o.hi[2] / kCell)) - z0_;
        for (int z = az; z <= bz; ++z)
            for (int x = ax; x <= bx; ++x) cells_[size_t(z) * nx_ + x].push_back(i);
    }
    stamp_.assign(objects_.size(), 0);
    query_ = 0;
}

bool CollisionWorld::Ray(const float start[3], const float dir[3], float length, Hit& hit) const
{
    if (cells_.empty()) return false;
    const float end[3] = { start[0] + dir[0] * length, start[1] + dir[1] * length, start[2] + dir[2] * length };
    const float lo[3] = { std::min(start[0], end[0]), std::min(start[1], end[1]), std::min(start[2], end[2]) };
    const float hi[3] = { std::max(start[0], end[0]), std::max(start[1], end[1]), std::max(start[2], end[2]) };
    int ax = int(std::floor(lo[0] / kCell)) - x0_, bx = int(std::floor(hi[0] / kCell)) - x0_;
    int az = int(std::floor(lo[2] / kCell)) - z0_, bz = int(std::floor(hi[2] / kCell)) - z0_;
    ax = std::max(ax, 0); az = std::max(az, 0);
    bx = std::min(bx, nx_ - 1); bz = std::min(bz, nz_ - 1);
    if (++query_ == 0) {
        std::fill(stamp_.begin(), stamp_.end(), 0u);
        query_ = 1;
    }
    float best = length;
    bool found = false;
    for (int z = az; z <= bz; ++z)
        for (int x = ax; x <= bx; ++x)
            for (uint32_t i : cells_[size_t(z) * nx_ + x]) {
                if (stamp_[i] == query_) continue;
                stamp_[i] = query_;
                const Object& o = objects_[i];
                if (o.hi[0] < lo[0] || o.lo[0] > hi[0] || o.hi[1] < lo[1] || o.lo[1] > hi[1] || o.hi[2] < lo[2] ||
                    o.lo[2] > hi[2])
                    continue;
                // 0x5a3e40: with several shapes, shape 0 bounds the others: a
                // ray that misses it (or leaves it behind the start, or meets
                // it beyond the length) skips the entry, and it is no hit itself.
                for (size_t s = o.first; s < o.first + o.count; ++s) {
                    const Shape& sh = shapes_[s];
                    float lp[3], ld[3], t = 0, n[3];
                    ToLocal(sh.world, start, dir, lp, ld);
                    bool ok = sh.type == 0 ? RayBox(sh.size, lp, ld, t, n)
                            : sh.type == 1 ? RayCylinder(sh.size[0], sh.size[1], lp, ld, t, n)
                            : sh.type == 2 ? RaySphere(sh.size[0], lp, ld, t, n)
                                           : false;
                    if (ok && t > length) ok = false;
                    if (s == o.first && o.bounded) {
                        if (!ok) break;
                        continue;
                    }
                    if (!ok) continue;
                    if (sh.surface == 0x13) continue;
                    if (t < 0) t = 0;
                    if (t > best || (found && t == best)) continue;
                    best = t;
                    found = true;
                    hit.distance = t;
                    hit.surface = sh.surface;
                    hit.instance = o.instance;
                    for (int k = 0; k < 3; ++k)
                        hit.normal[k] = n[0] * sh.world[k] + n[1] * sh.world[4 + k] + n[2] * sh.world[8 + k];
                }
            }
    return found;
}

bool CollisionWorld::Cast(const CityGround* ground, const float start[3], const float dir[3], float length,
                          Hit& hit) const
{
    Hit shapeHit;
    const bool found = Ray(start, dir, length, shapeHit);
    const float best = found ? shapeHit.distance : length;
    hit = shapeHit;
    if (!ground) return found;
    auto probe = [&](float x, float y, float z, float& h, float n[3], int& surf) {
        n[0] = 0; n[1] = 1; n[2] = 0;
        surf = 0;
        if (!ground->Probe(x, y, z, h, n, &surf)) h = -500.0f;
    };
    float h0, n0[3];
    int s0;
    probe(start[0], start[1], start[2], h0, n0, s0);
    const float d0 = start[1] - h0;
    auto take = [&](float t, const float n[3], int surf) {
        hit.distance = t;
        hit.surface = surf;
        hit.instance = -1;
        for (int k = 0; k < 3; ++k) hit.normal[k] = n[k];
        return true;
    };
    if (d0 < 0) return take(0.0f, n0, s0);
    if (std::fabs(dir[0]) < 0.0500000007f && std::fabs(dir[2]) < 0.0500000007f) {
        const float t = -(d0 / dir[1]);
        if (best <= t || t < 0) return found;
        return take(t, n0, s0);
    }
    // One step to the end of the ray.
    const float end[3] = { start[0] + dir[0] * length, start[1] + dir[1] * length, start[2] + dir[2] * length };
    float h1, n1[3];
    int s1;
    probe(end[0], start[1], end[2], h1, n1, s1);
    const float below = (h1 - start[1]) - dir[1] * length;
    if (!(below > 0)) return found;
    const float t = length * (d0 / (below + d0));
    float h2, n2[3];
    int s2;
    probe(start[0] + dir[0] * t, start[1] + dir[1] * t, start[2] + dir[2] * t, h2, n2, s2);
    if (best <= t) return found;
    return take(t, n2, s2);
}

std::vector<std::vector<float>> CollisionWorld::InstanceShapes(int instance) const
{
    std::vector<std::vector<float>> out;
    for (const Object& o : objects_) {
        if (o.instance != instance) continue;
        for (size_t s = o.first; s < o.first + o.count; ++s) {
            const Shape& sh = shapes_[s];
            std::vector<float> v = { float(sh.type), float(sh.surface), sh.size[0], sh.size[1], sh.size[2] };
            v.insert(v.end(), sh.world, sh.world + 16);
            out.push_back(v);
        }
    }
    return out;
}
