// The trees' ground shadows (BOOT_TO_MENU.md section 56): the object made
// by 0x5613f0 (world object +0x34; only when the ride's time of day, setup
// vtbl+0x64, is not 3) keeps three buffer objects of the manager [0x8ac240]
// (D3DPT_TRIANGLELIST): 0x800 vertices with sfx.pmu material 0x1f0016 (the
// atlas of four tree silhouettes, 128 x 256) twice, and 0x100 vertices with
// 0x1f0015 (a round crown seen from above). 0x5616f0 rebuilds them
// (0x561800) when the point has moved 66.7 units (squared 4444.44) since the
// last build: the ATRD query (kind 14 within 200 units, at most 256), each
// tree's ten ground points (0x562070) and their quads (0x561750). The draw
// (0x5616c0, vtbl+0x10 of each buffer with vertices) writes the stencil and
// the destination alpha (substance type 4, preset 0x27) in the mode 7 pass,
// before the shadowed redraw.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "city.h"

// 48 bytes, as the buffer objects' vertices (declaration 1).
struct TreeShadowVertex
{
    float position[3], normal[3], uv[2], colour[4];
};

class TreeShadows
{
public:
    // The ground under (x, z) below y (0x4e9da0): height and up normal;
    // false when there is none (the game: height below -499).
    using Probe = std::function<bool(float x, float y, float z, float& height, float normal[3])>;

    // 0x5616f0: rebuilds when the point (x, z) is 66.7 units from the last
    // build's (or on the first call). light: the mood's +0x170 (Mood::sun).
    // True when it rebuilt.
    bool Update(const CityPlacement& city, const float point[2], const float light[3], const Probe& probe);
    void Reset() { built_ = false; }

    // Per buffer: the triangle list the game puts in the device buffer
    // (every quad of the written vertices as (a, b, c), (c, b, d)), the
    // triangles it draws (the written vertex count / 3: the game draws only
    // the first two thirds of the list), and the trees in it.
    std::vector<TreeShadowVertex> list[3];
    int triangles[3] = {};
    int trees[3] = {};
    int found = 0;      // trees returned by the query
    int noGround = 0;   // trees left out by 0x562070 (no ground)

private:
    bool built_ = false;
    float last_[2] = {};
};
