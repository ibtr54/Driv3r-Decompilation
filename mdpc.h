// The model part of an MDPC chunk ("model package"): vertex buffers, one
// index buffer, models with up to 7 levels of detail, their submeshes and
// primitive groups. Read like the model package loader (0x5ca1a0 with
// 0x5ca020, 0x5ca590, 0x5cab40, 0x5cc3d0, 0x5d68e0) and drawn like
// 0x5dfe20. The materials of the chunk (PCMP, header +0x2c) are read by
// LoadMaterials (mec.h).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MdpcGroup
{
    // Header +0x1c, 0x38 bytes each. +0x18 (byte, 0 in the files) selects
    // DrawIndexedPrimitive (0) or DrawPrimitive (1) in 0x5dfe20; +0x1c / +0x1e
    // are filled at load (0x5d68e0: vertex buffer handle, shader).
    uint32_t type = 0;        // +0x00 D3DPRIMITIVETYPE (5 strip, 4 list)
    int32_t baseVertex = 0;   // +0x04
    uint32_t minIndex = 0;    // +0x08
    uint32_t numVertices = 0; // +0x0c
    uint32_t startIndex = 0;  // +0x10
    uint32_t primitives = 0;  // +0x14
    uint32_t material = 0;    // +0x30: package uid << 16 | material index
                              // (0x5cab40; 0xffff0000 / 0xcccccccc: none)
};

struct MdpcSubmesh
{
    // Header +0x14, 0x58 bytes each: +0x00 group offset, +0x08..+0x47 a
    // 4 x 4 matrix, +0x48 u16 group count, +0x4a byte: the matrix is used.
    // The draw setup (0x5de4e0) passes transpose(world) x matrix to the
    // shader (0x6055f6, 0x6053a5) when +0x4a is set, so a vertex goes to
    // the model's space as matrix x (x, y, z, 1) (column vector).
    float matrix[16] = {};
    bool hasMatrix = false;
    std::vector<MdpcGroup> groups;
    // The position in the model's space.
    void Transform(const float* p, float out[3]) const
    {
        for (int k = 0; k < 3; ++k)
            out[k] = hasMatrix ? matrix[4 * k] * p[0] + matrix[4 * k + 1] * p[1] + matrix[4 * k + 2] * p[2] + matrix[4 * k + 3]
                               : p[k];
    }
};

struct MdpcModel
{
    // Header +0x0c, 0x188 bytes each.
    uint32_t id = 0;          // +0x04
    uint16_t vertexBuffer = 0;// +0x18 (0x5d68e0 puts it in the group's handle)
    uint16_t vertexType = 0;  // +0x1a (0x5ca590: 2-4 -> declaration 2, 5 -> 5,
                              // 6 -> 6, 8 -> 7, others -> 1; table 0x8acc70)
    uint16_t flags = 0;       // +0x1c
    float box[8][3] = {};     // +0x28: 8 corners (x, y, z, w = 1)
    // +0xa8 + 0x20 * lod: {+0 submesh offset, +4 pointer, +8 count, ...,
    // +0x14 mask}. A slot is drawn when its mask has a bit of the object's
    // +0x62 (0x5dcc50); 0x5ca020 sets slot 5's mask to 0x20 when it has
    // submeshes, else slot 6's.
    std::vector<MdpcSubmesh> lods[7];
    uint32_t lodMask[7] = {};
};

struct MdpcVertexBuffer
{
    // Header +0x40, 0x1c bytes each: +0 vertices, +4 size, +8 offset,
    // +0xc stride (+0xe and +0x10.. are filled at load).
    uint32_t count = 0, stride = 0;
    std::vector<uint8_t> data;
    // Position of vertex i (the first float3 of every declaration but 7).
    const float* Position(uint32_t i) const { return reinterpret_cast<const float*>(&data[size_t(i) * stride]); }
};

// The first 8 bytes of a PCMP substance (a material's pass), as the game has
// them after loading (ConvertSubstance): +0 type (selects the render-state
// preset, 0x5cb9d0), +1 / +2 more presets (0x5d8340), +4 & 0x7f / +5 / +6
// pick the effect with the model's render id (0x5d68e0).
struct MdpcSubstance
{
    uint8_t bytes[8] = {};
    // The file's byte 7 before the conversion: the runtime substance's
    // +0x15 that the vehicle binders switch on (0x5e0b20 textures, 0x5df200
    // g_colourWeightsVec: 0, 4, 8, 0x10); inferred from every traced car
    // draw (vehicle 6 and CarGlobals), where the conversion keeps it was
    // not traced.
    uint8_t colourMode = 0;
    // +0x10 / +0x14: offsets of its texture entries (index into
    // MdpcPackage::textures); 0x5e0af0 binds the first as the diffuse
    // texture, 0x5e0e70 the second as the normal map.
    std::vector<int> textures;
};

// A PCMP texture entry (0x20 bytes: +4 hash, +8 data offset from PCMP +0x30,
// +0xc size, +0x10 type, +0x14 / +0x16 width / height): its DDS file.
struct MdpcTexture
{
    uint32_t hash = 0;
    std::vector<uint8_t> dds;
};

// The game converts every substance of a package when it is first loaded
// (0x5d6490): 0x5cbf80 turns the file's dword +0 into the runtime type
// (byte 0, table 0x5cc1c4), byte 1 and the flags of byte 2 (bits 0x8000,
// 0x10000, 0x20000, 0x40000; 0x100 / 0x200 only with flag), then 0x5cc270
// moves byte 6 to byte 3 and rewrites bytes 1, 2 and 4..7 from bytes 4, 5
// and 7. flag is set only for substance 0x20 of a package of type 0x1f.
void ConvertSubstance(uint8_t s[8], bool flag = false);

// Offset and size of every MDPC chunk in a CHNK file, depth first (a
// .vvs holds one per car, SuperRegions.pcs one per region group).
std::vector<std::pair<size_t, size_t>> FindMdpcChunks(const std::vector<uint8_t>& file);

class MdpcPackage
{
public:
    // The MDPC chunk data at mdpc / size inside file (see FindChunk).
    bool Load(const std::vector<uint8_t>& file, size_t mdpc, size_t size);
    const std::string& Error() const { return error_; }

    uint32_t version = 0;     // +0x00 (6)
    uint32_t uid = 0;         // +0x04 (material references use it)
    std::vector<MdpcVertexBuffer> vertexBuffers;
    std::vector<uint16_t> indices;   // +0x30 count, +0x34 size, +0x38 offset
    std::vector<MdpcModel> models;
    // Per material index (group material & 0xffff), its substances.
    std::vector<std::vector<MdpcSubstance>> materials;
    // Per material, the entry's +8 flags (bit 0: the substances are frames
    // of an animation) and +0xc speed (0x5de890, 0x5cfe78).
    std::vector<uint32_t> materialFlags;
    std::vector<float> materialSpeed;
    std::vector<MdpcTexture> textures;  // the PCMP texture table, in order

    // Triangles of a group as vertex indices into its model's vertex
    // buffer (strips unrolled with alternating winding, degenerate ones
    // dropped).
    void Triangles(const MdpcGroup& g, std::vector<uint32_t>& out) const;

private:
    std::string error_;
};
