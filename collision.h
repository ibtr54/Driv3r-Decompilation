// The static collision world (BOOT_TO_MENU.md section 63, driv3r-re\PHYSICS.md).
// The game keeps the city's collision as objects in the physics world
// [0x7e9bb0]: a placed city instance (GEBI record) whose model has a PMLD
// record gets that record's PDL entries (the region's GEPM block); each
// entry is a list of primitive shapes (box, cylinder about y, sphere) with a
// matrix in the instance. Rays (0x5a0820 batch / 0x5a03f0 single, through
// 0x5a7390) test each object's shapes with 0x5a3e40: the ray moved into the
// shape's frame (shape matrix x instance matrix, 0x605392), then 0x5b6790
// (box), 0x5b6980 (cylinder) or 0x5b6cd0 (sphere); the hit's surface is the
// shape's byte +0x51, its normal goes back to the world.
//
// Here: the objects (one per PDL entry; with several shapes, shape 0 only
// bounds the others) are built for every instance at load (the game streams
// them by region, 0x5a6110 / 0x5bc760) and kept in a uniform grid; the
// primitive tests are the textbook ones, not yet the game's operation order.
#pragma once

#include <cstdint>
#include <vector>

#include "city.h"

class CollisionWorld
{
public:
    struct Hit
    {
        float distance = 0;
        int surface = -1;
        float normal[3] = { 0, 1, 0 };
        int instance = -1;     // the city instance hit (-1: the ground)
        int shape = -1;        // its shape (ShapeAt; -1: the ground)
    };
    // The objects of every instance with a PMLD record in its region, but
    // the dynamic props (CityPlacement::dynamicModels: bodies in the game).
    void Build(const CityPlacement& city);
    // The nearest shape hit along dir (unit) within length; a start inside a
    // shape hits at 0 (as 0x5a3e40 returns 0 for an entry distance below 0)
    // with the normal of the shape's nearest face (0x5a5000).
    // Shapes of surface 0x13 do not stop rays (0x5bc760 only notes them).
    // inflate: the ray's radius (0x5a03f0's fifth argument, 0x5a3e40's
    // last): box half extents + inflate (0x5b6790); cylinder radius and half
    // height, sphere radius + inflate (inferred).
    bool Ray(const float start[3], const float dir[3], float length, Hit& hit, float inflate = 0.0f) const;
    // A physics ray as 0x5a0820 answers it: the shapes, then the ground
    // (0x5a0090 with the probe 0x4e9d60 / 0x4e9da0, one step): the GRTD
    // height under the start; a start below it hits at 0; a near-vertical
    // ray (|dir.x|, |dir.z| < 0.05) hits at (start.y - height) / -dir.y;
    // others where the straight line from the start crosses below the
    // height under the end (probed again there for the normal and surface).
    // The ground wins only when nearer than the shapes.
    bool Cast(const CityGround* ground, const float start[3], const float dir[3], float length, Hit& hit,
              float inflate = 0.0f) const;
    size_t Objects() const { return objects_.size(); }
    // Diagnostics: the shapes of an instance (type, surface, size, world
    // matrix), one line each.
    std::vector<std::vector<float>> InstanceShapes(int instance) const;
    size_t Shapes() const { return shapes_.size(); }

    struct Shape
    {
        int type = 0;          // 0 box, 1 cylinder (y), 2 sphere
        int surface = 0;
        float size[3] = {};
        float world[16] = {};  // shape matrix x instance matrix (row vector)
    };
    struct Object
    {
        size_t first = 0, count = 0;  // in shapes_
        int instance = -1;
        bool bounded = false;          // shape 0 bounds the others
        float lo[3] = {}, hi[3] = {};  // world AABB
    };
    // The objects whose bounding box overlaps [lo, hi] (each once; the
    // game's broad phase gives the body these pairs, contact.h).
    void Overlapping(const float lo[3], const float hi[3], std::vector<uint32_t>& out) const;
    const Object& ObjectAt(uint32_t i) const { return objects_[i]; }
    const Shape& ShapeAt(size_t i) const { return shapes_[i]; }

private:
    std::vector<Shape> shapes_;
    std::vector<Object> objects_;
    // Grid of kCell-unit squares on x / z.
    static constexpr float kCell = 32.0f;
    int x0_ = 0, z0_ = 0, nx_ = 0, nz_ = 0;
    std::vector<std::vector<uint32_t>> cells_;
    mutable std::vector<uint32_t> stamp_;
    mutable uint32_t query_ = 0;
};
