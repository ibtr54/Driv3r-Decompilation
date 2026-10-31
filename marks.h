// The car's damage marks: bullet holes and glass cracks (BOOT_TO_MENU.md
// section 92). Each vehicle's UPVH holds preset points per body mesh (=
// damage record; VehicleData::markPoints); a hit at a point (0x507bb0, slot
// vtbl+0xa4: gunfire) places the nearest free one as a small quad that
// follows the dent (rest and dented position, blended by the record's two
// halves). A crash never makes one (run 37: 23 impacts, no call).
//
// The marks object (slot +0x550, 0x4f6780 from a world pool of 3): one byte
// per point (its region 0..7, 0xff free); per mesh and side (0 hole, 1 glass)
// 8 regions of at most 16 marks, each region drawing from a buffer of 16
// quads taken from the world's pool (16 hole buffers, sfx material
// 0x1f000a; 4 glass buffers, 0x1f000c; 0x50e239).
#pragma once

#include <cstdint>
#include <vector>

#include "damage.h"
#include "vehicle.h"

// A mark buffer's vertex (0x30 bytes, as the skid ribbon's: position,
// normal, u, v, colour RGBA floats; 0x4f34a0).
struct MarkVertex
{
    float pos[3];
    float normal[3];
    float uv[2];
    float colour[4];
};
static_assert(sizeof(MarkVertex) == 48, "the buffer's stride");

// One buffer (0x4f33a0 with capacity 16): 16 quads of 4 vertices in a ring.
struct MarkBuffer
{
    static const uint32_t kQuads = 16;
    int side = 0;
    MarkVertex v[kQuads * 4];
    uint32_t count = 0;  // +8: quads drawn (0x4f3650 stops at 15)
    uint32_t at = 0;     // +0xc: the next quad written
};

// The world's buffers (world +0xf30 + side x 0x44: a stack of free ones).
class MarkPool
{
public:
    MarkPool();
    int Take(int side);   // -1: none free (0x50f270's first test)
    void Give(int buffer);
    std::vector<MarkBuffer> buffers;
    std::vector<int> free[2];
};

// What the drawing needs of one buffer (0x4f5a80): the record it belongs to,
// its side and its quads.
struct MarkDraw
{
    int record = 0, side = 0;
    const MarkBuffer* buffer = nullptr;
};

class CarMarks
{
public:
    static const int kMeshes = 0x14, kPoints = 800;

    // 0x4f6780 (0x4f5390 / 0x4f53d0 / 0x4f5480): every point free, no
    // buffer; the meshes' anchors from the damage entries (-(R t), the
    // part's centre in the vehicle).
    void Init(const VehicleData& v);
    bool active = false;  // the slot has its marks object (made by the first hit)

    // 0x50bd20: places point p of mesh m. r: the mesh's record (nullptr: t
    // 0); fresh: a new hole asks for sparks (not ported). car: the car's
    // matrix (row vectors), for the pool's give-back. Glass with a record
    // steps its stage (0x4f5190). Returns false when nothing was placed.
    bool Place(const VehicleData& v, MarkPool& pool, int p, int m, DamageRecord* r, const float car[16]);
    // 0x4f5890 (per hit from 0x503fb0, mesh k): when the record's halves
    // moved by more than 0.125 since the last placement, every mark of the
    // mesh is placed again at the record's present dent.
    void Replace(const VehicleData& v, MarkPool& pool, int m, DamageRecord& r, const float car[16]);
    // 0x507bb0's marks share for a hit at point (in the vehicle's space):
    // the nearest free point within 0.375 m on the parts still on the car
    // whose first physics shape (radius x 1.5) holds the point. Returns the
    // point placed (-1: none; then a placed point that near only adds to its
    // record's +0x24, glass stepping its stage).
    int Hit(const VehicleData& v, MarkPool& pool, std::vector<DamageRecord>& records,
            const std::vector<int>& recordPart, const float point[3], const float car[16]);

    // The buffers to draw (0x4f5a80 for each record; glass only while not
    // broken and the part's dent averages at most 0.5).
    void Draws(const VehicleData& v, const MarkPool& pool, const std::vector<DamageRecord>& records,
               const std::vector<int>& recordPart, std::vector<MarkDraw>& out) const;

    uint8_t region[kPoints];
    uint32_t marks[kMeshes][2][8] = {};  // +0x334: marks per region
    float last[kMeshes][2] = {};         // +0x374 / +0x378: the record's halves when placed
    float anchor[kMeshes][4] = {};       // +0x324
    int buffer[kMeshes][2][8];           // +0xaa4: pool index, -1 none
    uint32_t perSide[kMeshes][2] = {};   // +0x37c / +0x380
    uint32_t used = 0;                   // +0

private:
    void Free(const VehicleData& v, MarkPool& pool, int m, int side, int reg);  // 0x4f5b10
    bool GiveBack(const VehicleData& v, MarkPool& pool, const float point[4], const float car[16]);  // 0x4f5c30
    int meshes_ = 0;
};

// The packed point (0x4f4ca0 / 0x4f4d10) and normal (0x4f4ba0 / 0x4f4c20).
void MarkPosition(uint32_t v, float out[4]);
void MarkNormal(const uint8_t b[3], float out[4]);
// The points of mesh m: [first, end).
void MarkRange(const VehicleData& v, int m, uint32_t& first, uint32_t& end);
// 0x4f5190: a glass record's crack stage (+0x31 up to 3; 2 cracked, 3 broken;
// vehicles 9, 14, 57, 89, 90, 91 never break: 0x5140d0).
void GlassStep(DamageRecord& r, int vehicleId);
