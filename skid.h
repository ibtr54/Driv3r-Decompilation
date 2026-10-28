// The skid marks (BOOT_TO_MENU.md section 84). The world object keeps four
// ribbons for every car (0x56ede0, made by the world init at 0x55c6ea; pool
// +0x2240), each a ring of 1600 vertices drawn as one triangle strip. The
// car's 60 Hz effects block (CarSim::Effects, 0x4b3481) takes one per rear
// wheel the first time that wheel skids (0x55f7a0), with the tyre's width,
// and switches it on or off with an intensity; every drawn frame the car
// (vtbl+0x84 = 0x4b0830) gives each of its ribbons the wheel's ground point
// (SkidPoint), and the ribbon adds a pair of vertices across the wheel's
// path or moves its last pair (SkidRibbon::Add = 0x56f3c0).
#pragma once

#include <cstdint>

// The ribbon's vertex (0x30 bytes; the traced draws' stride 48): position,
// normal, u = the length along the mark, v = 0 / 1 across it, colour.
struct SkidVertex
{
    float pos[3];
    float normal[3];
    float uv[2];
    float colour[4];
};
static_assert(sizeof(SkidVertex) == 48, "the traced stride");

class SkidRibbon
{
public:
    static const int kVertices = 0x640;  // 1600

    SkidVertex v[kVertices];  // +4
    uint32_t count = 0;       // +0x12c04: vertices in use (up to 1600)
    uint32_t head = 0;        // +0x12c08: the next pair's index
    float last[4] = {};       // +0x12c14: the last pair's centre
    float lastPair[4] = {};   // +0x12c24: x = length, then the centre (0x56f3c0's end)
    float width = 0.2f;       // +0x12c34
    float length = 0;         // +0x12c3c: u of the last pair
    bool on = false;          // +0x12c40
    bool fresh = true;        // +0x12c41: the next point starts a segment
    uint32_t life = 10;       // +0x12c44: counts down per point
    uint32_t segFirst = 0;    // +0x12c4c, +0x12c50: the segment's first pairs
    uint32_t segSecond = 0;
    float dir[4] = {};        // +0x12c54: the segment's direction
    float intensity = 0;      // +0x12c64

    // 0x56ede0 / 0x56ed50: empty, every vertex colour (1, 1, 1, 1).
    SkidRibbon() { Reset(); }
    void Reset();
    // 0x56f3c0: point p (x y z w), the wheel's velocity vel (its contact
    // velocity, record +0xa0) and the ground normal n (w 0). surface: the
    // colour table's row (the game looks the ground under p up with
    // 0x4e9da0; the program passes the wheel's contact surface); mood: the
    // mood's colour at +0x210 (r g b, alpha 1).
    void Add(const float p[4], const float vel[4], const float n[4], int surface, const float mood[3]);

private:
    void Pair(const float a[3], const float b[3], const float n[3], const float colour[4]);  // 0x56ef60
    uint32_t Repeat();   // 0x56f070
    void Rebase();       // 0x56f100
    void Move(const float a[4], const float b[4], const float n[3], float u, const float colour[4]);  // 0x56eef0
};

// 0x4b0830's point for one wheel: L = the wheel's place in the car (local),
// lifted by lift (0x513db0 of the blended compression), through the
// matrix M (rows x, y, z, position; the drawn body), moved by the tyre's
// width along M's x (wheels 2, 3, 5: -x, the others +x) and, for the
// models 0x23 / 0x3b, by 0.1 along M's y.
void SkidPoint(const float local[3], float lift, const float M[16], float width, int wheel, int model, float out[4]);
