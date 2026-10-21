// Where the city places its models: the GEBI chunks of a .d3c. Each
// GEBI is 0x40 bytes of header and then 0x20-byte instance records; one
// sits in every non-empty GEIR cell (a square grid of 480-unit cells
// centred on 0) and one in the first GESR (header +4 = -2). A record names a
// model of a package in <city>\SuperRegions.pcs and places it with a
// position and a rotation. Read like the GEBI handler (vtbl 0x780de8,
// load 0x44cf90), the matrix as 0x473830 / 0x59f650 build it and the
// model as 0x5cbdd0 / 0x5dcc50 look it up (package table 0x8b5310,
// filled by uid in 0x57c058).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CityInstance
{
    float position[3] = {};   // +0x00 x, y (up), z
    uint16_t model = 0;       // +0x0c package uid << 10 | model index
                              // (index 0x3ff: no model)
    uint16_t extra = 0;       // +0x0e (0xffff in most records)
    float rotation[4] = {};   // +0x10: cos, sin, -, flag > 100 (yaw form)
                              // or a quaternion x, y, z, w
    // Row-major, row vector times matrix (Direct3D): rows 0-2 the rotation,
    // row 3 the position.
    float world[16] = {};
    int column = 0, row = 0;  // GEBI header +4 / +8 (-2 / -2: the GESR one)
    int range = 0;            // which of the header's six ranges (+0x0c..)

    bool HasModel() const { return (model & 0x3ff) != 0x3ff; }
    uint32_t PackageUid() const { return model >> 10; }
    uint32_t ModelIndex() const { return model & 0x3ff; }
    // A point in model space to the world.
    void Transform(const float* p, float out[3]) const
    {
        for (int k = 0; k < 3; ++k)
            out[k] = p[0] * world[k] + p[1] * world[4 + k] + p[2] * world[8 + k] + world[12 + k];
    }
};

// SCND (a sibling of the GEBI; handler vtbl 0x781340, load 0x45b150, which
// stores it in the cell's slot +0xc): the cell's quadtree. Header: +0
// nodes (offset), +4 index table (offset, when +0xc > 0), +8 node count,
// +0xc index count, +0x10 index stride, +0x14 cell size, +0x18 / +0x1c
// origin x / z (the cell's lowest corner), +0x20 / +0x24 column / row.
// Node (16 bytes, 0x4dec70): u16 first index (low 12 bits) and count (high
// 4 bits), u16 level, u16 x / z on the level's grid, u16 children[4]
// (node numbers, 0: none). A node at level l is size / 2^l wide and
// centred on origin + (x or z + 0.5) x that width.
struct CityNode
{
    uint16_t first = 0, count = 0, level = 0, x = 0, z = 0;
    uint16_t child[4] = {};
};

struct CityCell
{
    int column = 0, row = 0;  // -2 / -2: the GESR's (drawn in mode 3)
    float size = 0, origin[2] = {};
    std::vector<CityNode> nodes;   // node 0: the root
    std::vector<uint32_t> index;   // GEBI record numbers
    size_t firstInstance = 0;      // instances[] of the GEBI's record 0
    size_t instanceCount = 0;
};

// GWTR (a child of the GESR; handler vtbls 0x781134 / 0x78111c, load
// 0x450be0 when the chunk entry's version byte is 5, tag 'GWTR' at object
// +0x784): the sea. Header: +0 tile count, +4 shore line count, +8 water
// height (object +0x774, default -10.0; 0x450fc0 returns it), +0xc bytes:
// texture count, colour steps, colour sets. Then the tiles (0x20 bytes: x,
// y, z, w, u16 model id), the colours (sets x steps x RGBA floats; the
// mood's SEA_COLOUR - 1, 0..3, picks the set: 0x450df0 / 0x450e50), the
// texture references (u32 each, 0x450fd0) and the shore lines (0x454b00).
// Each tile is a model of the city's packages (flat at y 0), drawn with
// an identity rotation at the tile's point (0x451030).
// A shore line (0x4546f0): 0x18-byte header {+0 point count, +4 ?, +8 / +0xc
// centre x / z, +0x10 / +0x14 half size x / z} and per point 6 floats: the
// two ends (x1, z1, x2, z2) of a cross-section, then u and v.
struct ShoreLine
{
    float centre[2] = {}, half[2] = {};
    float radius = 0;           // |half| (record +4)
    std::vector<float> points;  // 6 per point
    size_t Count() const { return points.size() / 6; }
};

struct CityWater
{
    float height = -10.0f;
    std::vector<CityInstance> tiles;  // column / row -2 (the GESR's)
    int colourSteps = 0, colourSets = 0;
    std::vector<float> colours;      // sets x steps x 4
    std::vector<uint32_t> textures;  // texture references (materials: [0] the shore waves, [6] the far quad)
    uint32_t shoreLines = 0;
    std::vector<ShoreLine> shore;
};

// ATRD (a GEIR child; handler [0x8ac358], per-chunk object vtbl 0x7811e0,
// tag method 0x456590): a cell's points by kind. Header: 4 floats (the
// cell's lowest corner x / z and its size x / z, 480), then 21 offsets from
// the chunk start, kind k's records being [offset k, offset k + 1); records
// of 16 bytes. Position (0x40fa80): x = int32 +0 / 2^19, y = (+0 & 0x1fff)
// / 64 - 10, z = int32 +4 / 2^19; +0xa a type byte; +0xb the kd split of the
// kind's records (0 x, 2 z, 0xff leaf; the tree is implicit: the node of
// [lo, hi) is (lo + hi) / 2). Kept here: kind 14, the trees whose ground
// shadows 0x561800 draws (type 0x11 palms, 0x12..0x14 other trees in the
// rows of the shadow atlas, 0x1d round trees), y the crown's height.
struct CityTree
{
    float position[3] = {};
    uint8_t type = 0, axis = 0xff;
};

struct CityTreeCell
{
    float corner[2] = {}, size[2] = {};
    std::vector<CityTree> trees;  // kind 14, in file (kd) order
};

// GRTD (a GEIR child, tag method 0x44d220, handler vtbl 0x780e1c; checked
// by 0x4e9fe0, registered in the nine slots of [0x8ac344]): the cell's
// ground heights, "THD v3.0" at +4. Header: +0xc node count, +0x10 / +0x14
// squares per row / column, +0x18 / +0x1c / +0x20 origin x / y / z (the
// cell's lowest corner), +0x24 / +0x28 square size (20), +0x2c / +0x30
// their inverses, then offsets from the chunk: +0x34 node types (a nibble
// per node, the high one for even nodes), +0x38 a u16 per node, +0x3c child
// pairs (two u32 node numbers), +0x40 layer lists (8 bytes: offsets into the
// +0x44 lines and +0x48 leaves), +0x44 lines (s16 a, b, c, s8 front, back),
// +0x48 leaves (u16 plane numbers), +0x4c planes (20 bytes: a, b, c, d,
// two u16 ids). Nodes 0 .. squares-1 are the squares (row-major).
class CityGround
{
public:
    bool Add(const std::vector<uint8_t>& d, size_t off, size_t size);
    void Clear() { chunks_.clear(); }
    size_t Chunks() const { return chunks_.size(); }
    // 0x4e9da0: the ground at (x, z) for a point at height y: the chunk
    // whose squares hold (x, z) (0x4ea060, then 0x4ea170): a split node
    // (type 0xe) halves the square, x then z, down to a plane (0xf) or to
    // n stacked layers, each a 2D BSP (0x4ea380) of lines a x fx + b x fz +
    // c (fx, fz in the node, > -1e-5: front) ending in a plane; 0x4e9ef0
    // keeps the highest layer below y + 0.5 (else a lower one). Height =
    // (d - a lx - c lz) / b (lx, lz from the origin); normal (a, b, c).
    // surface: the plane's u16 +0x10 (the probe's +0x14, the ground's
    // surface for the physics rays, 0x4e9e46). False: no ground (the game's
    // -500, surface 0).
    bool Probe(float x, float y, float z, float& height, float normal[3], int* surface = nullptr) const;

private:
    struct Chunk
    {
        std::vector<uint8_t> d;
        int cols = 0, rows = 0, nodes = 0;
        float ox = 0, oz = 0, sx = 0, sz = 0, ix = 0, iz = 0;
        uint32_t types = 0, words = 0, pairs = 0, layers = 0, lines = 0, leaves = 0, planes = 0;
    };
    bool Query(const Chunk& c, float x, float y, float z, float& height, uint32_t& plane) const;
    std::vector<Chunk> chunks_;
};

// A region's collision shapes (BOOT_TO_MENU.md section 63): the GESR
// children GEPM (handler vtbl 0x780d90, tag 0x44ce20, register 0x44ce50:
// +0 region, +4 offset of a "PDL001.002.003a" block, relocated by 0x5a5a50
// and kept at [0x7e9bb0]+0x20c +0x32cc + region x 4) and PMLD (+0 region, +4
// count, +8 offset of 0x14-byte records {u32 model id = package uid << 10 |
// model index, u32 first PDL entry, u32 entry count, f32 radius, u32}).
// PDL block: +0 entries, +8 entry table (0x10 bytes: shape count, shapes'
// offset from the entry, radius), shapes 0x60 bytes: +0 size (box half
// extents; cylinder radius / half height about y; sphere radius), +0x10 the
// shape's 4 x 4 matrix in its object, +0x50 type (low byte: 0 box, 1
// cylinder, 2 sphere) | surface << 8, +0x54 / +0x58 / +0x5c (0.7, 0.6, 1).
struct CityCollisionRegion
{
    int region = -1;
    std::vector<uint8_t> pdl;  // the PDL block as stored (offsets unrelocated)
    struct Model { uint32_t id, first, count; float radius; };
    std::vector<Model> models; // PMLD records
};

class CityPlacement
{
public:
    // All GEBI chunks of a .d3c file (GEIR cells and GESR regions).
    bool Load(const std::vector<uint8_t>& d3c);
    const std::string& Error() const { return error_; }

    std::vector<CityInstance> instances;
    // Cells with a GEBI and a SCND (GEIR cells and the GESR).
    std::vector<CityCell> cells;
    // The GESR's GWTR (no tiles: the city has none).
    CityWater water;
    // The ATRD chunks' trees, one entry per chunk in file order (the order
    // the query of 0x561800 visits them).
    std::vector<CityTreeCell> treeCells;
    // The GRTD chunks: the ground heights of the cells.
    CityGround ground;
    // The GEPM / PMLD chunks of the GESR regions (collision).
    std::vector<CityCollisionRegion> collision;
    // The dynamic props: the model ids of the GEPD / HRDT groups ('AWHF'
    // records like the vehicles' UPVH, model id at +0x14; 0x44f823 loads
    // them). Their instances are physics bodies in the game, not static
    // collision.
    std::vector<uint32_t> dynamicModels;
    // GEIR cells per side (12 in Miami, 15 in Nice and Istanbul); a cell
    // is 480 units and the grid is centred on 0, so column c starts at
    // x = 480 * (c - gridSide / 2) and row r ends at z = -480 * (r - gridSide / 2)
    // (the cell's first GEBR row holds that corner).
    int gridSide = 0;

private:
    bool LoadGebi(const std::vector<uint8_t>& d, size_t off, size_t size);
    bool LoadScnd(const std::vector<uint8_t>& d, size_t off, size_t size, CityCell& cell);
    bool LoadGwtr(const std::vector<uint8_t>& d, size_t off, size_t size);
    bool LoadAtrd(const std::vector<uint8_t>& d, size_t off, size_t size);
    std::string error_;
};
