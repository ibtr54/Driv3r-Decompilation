#include "mdpc.h"

#include <cstring>
#include <utility>

namespace {

struct In
{
    const std::vector<uint8_t>& d;
    size_t base, size;
    bool Has(size_t off, size_t n) const { return off <= size && n <= size - off; }
    uint32_t U32(size_t off) const
    {
        uint32_t v = 0;
        if (Has(off, 4)) memcpy(&v, &d[base + off], 4);
        return v;
    }
    uint16_t U16(size_t off) const
    {
        uint16_t v = 0;
        if (Has(off, 2)) memcpy(&v, &d[base + off], 2);
        return v;
    }
    float F32(size_t off) const
    {
        float v = 0;
        if (Has(off, 4)) memcpy(&v, &d[base + off], 4);
        return v;
    }
};

void Collect(const std::vector<uint8_t>& d, size_t base, std::vector<std::pair<size_t, size_t>>& out, int depth)
{
    if (depth > 8 || base + 16 > d.size() || memcmp(&d[base], "CHNK", 4) != 0) return;
    uint32_t count;
    memcpy(&count, &d[base + 8], 4);
    for (uint32_t k = 0; k < count && base + 16 + size_t(k) * 16 + 16 <= d.size(); ++k) {
        size_t e = base + 16 + size_t(k) * 16;
        uint32_t off, size;
        memcpy(&off, &d[e + 4], 4);
        memcpy(&size, &d[e + 12], 4);
        if (base + off > d.size()) continue;
        if (memcmp(&d[e], "MDPC", 4) == 0) out.push_back({ base + off, size });
        Collect(d, base + off, out, depth + 1);
    }
}

}  // namespace

std::vector<std::pair<size_t, size_t>> FindMdpcChunks(const std::vector<uint8_t>& file)
{
    std::vector<std::pair<size_t, size_t>> out;
    Collect(file, 0, out, 0);
    return out;
}

bool MdpcPackage::Load(const std::vector<uint8_t>& file, size_t mdpc, size_t size)
{
    vertexBuffers.clear();
    indices.clear();
    models.clear();
    error_.clear();
    In h{ file, mdpc, size };
    if (!h.Has(0, 0x44)) { error_ = "header"; return false; }
    version = h.U32(0x00);
    uid = h.U32(0x04);
    // Vertex buffers (0x5ca590: one per header +0x3c, table at +0x40).
    uint32_t vbCount = h.U32(0x3c), vbTable = h.U32(0x40);
    for (uint32_t v = 0; v < vbCount; ++v) {
        size_t e = vbTable + size_t(v) * 0x1c;
        MdpcVertexBuffer vb;
        vb.count = h.U32(e);
        uint32_t bytes = h.U32(e + 4), off = h.U32(e + 8);
        vb.stride = h.U16(e + 0xc);
        if (!h.Has(off, bytes) || vb.stride == 0 || uint64_t(vb.count) * vb.stride > bytes) {
            error_ = "vertex buffer";
            return false;
        }
        vb.data.assign(file.begin() + long(mdpc + off), file.begin() + long(mdpc + off + bytes));
        vertexBuffers.push_back(std::move(vb));
    }
    // Index buffer: 16-bit, +0x30 indices, +0x34 bytes, +0x38 offset.
    uint32_t ibBytes = h.U32(0x34), ibOff = h.U32(0x38);
    if (!h.Has(ibOff, ibBytes)) { error_ = "index buffer"; return false; }
    indices.resize(ibBytes / 2);
    if (!indices.empty()) memcpy(indices.data(), &file[mdpc + ibOff], indices.size() * 2);
    // Models (header +0x08 count, +0x0c offset, 0x188 each).
    uint32_t count = h.U32(0x08), table = h.U32(0x0c);
    if (!h.Has(table, size_t(count) * 0x188)) { error_ = "models"; return false; }
    models.resize(count);
    for (uint32_t m = 0; m < count; ++m) {
        size_t mo = table + size_t(m) * 0x188;
        MdpcModel& model = models[m];
        model.id = h.U32(mo + 0x04);
        model.vertexBuffer = h.U16(mo + 0x18);
        model.vertexType = h.U16(mo + 0x1a);
        model.flags = h.U16(mo + 0x1c);
        for (int c = 0; c < 8; ++c)
            for (int k = 0; k < 3; ++k) model.box[c][k] = h.F32(mo + 0x28 + c * 16 + k * 4);
        for (int lod = 0; lod < 7; ++lod) {
            uint32_t smOff = h.U32(mo + 0xa8 + lod * 0x20), smCount = h.U32(mo + 0xb0 + lod * 0x20);
            if (!smOff || !smCount) continue;
            if (!h.Has(smOff, size_t(smCount) * 0x58)) { error_ = "submeshes"; return false; }
            for (uint32_t s = 0; s < smCount; ++s) {
                size_t so = smOff + size_t(s) * 0x58;
                MdpcSubmesh sm;
                for (int k = 0; k < 16; ++k) sm.matrix[k] = h.F32(so + 8 + k * 4);
                sm.hasMatrix = h.Has(so + 0x4a, 1) && file[mdpc + so + 0x4a] != 0;
                uint32_t gOff = h.U32(so), gCount = h.U16(so + 0x48);
                if (!h.Has(gOff, size_t(gCount) * 0x38)) { error_ = "groups"; return false; }
                for (uint32_t g = 0; g < gCount; ++g) {
                    size_t go = gOff + size_t(g) * 0x38;
                    MdpcGroup gr;
                    gr.type = h.U32(go);
                    gr.baseVertex = int32_t(h.U32(go + 4));
                    gr.minIndex = h.U32(go + 8);
                    gr.numVertices = h.U32(go + 0xc);
                    gr.startIndex = h.U32(go + 0x10);
                    gr.primitives = h.U32(go + 0x14);
                    gr.material = h.U32(go + 0x30);
                    sm.groups.push_back(gr);
                }
                model.lods[lod].push_back(std::move(sm));
            }
        }
    }
    // Materials (PCMP at header +0x2c): +0x08 count, +0x0c table of 0x18
    // entries {+0 offset of the substance offsets, +4 count}; offsets are
    // from the PCMP. A substance's bytes +0 / +4 / +5 / +6 choose the effect
    // in 0x5d68e0 (table 0x8accc8), +0x10 / +0x14 its textures.
    materials.clear();
    uint32_t pcmp = h.U32(0x2c);
    if (pcmp && h.Has(pcmp, 0x10) && memcmp(&file[mdpc + pcmp], "PCMP", 4) == 0) {
        uint32_t mCount = h.U32(pcmp + 8), mTable = h.U32(pcmp + 0xc);
        for (uint32_t m = 0; m < mCount && h.Has(pcmp + mTable + size_t(m) * 0x18, 0x18); ++m) {
            size_t e = pcmp + mTable + size_t(m) * 0x18;
            uint32_t list = h.U32(e), n = h.U32(e + 4);
            std::vector<MdpcSubstance> subs;
            for (uint32_t s = 0; s < n && s < 16; ++s) {
                size_t so = pcmp + h.U32(pcmp + list + s * 4);
                MdpcSubstance sub;
                if (h.Has(so, 8)) memcpy(sub.bytes, &file[mdpc + so], 8);
                subs.push_back(sub);
            }
            materials.push_back(std::move(subs));
        }
    }
    return true;
}

void MdpcPackage::Triangles(const MdpcGroup& g, std::vector<uint32_t>& out) const
{
    auto at = [&](uint32_t k) { return uint32_t(int32_t(indices[g.startIndex + k]) + g.baseVertex); };
    if (g.type == 5) {  // D3DPT_TRIANGLESTRIP
        if (g.startIndex + g.primitives + 2 > indices.size()) return;
        for (uint32_t i = 0; i < g.primitives; ++i) {
            uint32_t a = at(i), b = at(i + 1), c = at(i + 2);
            if (a == b || b == c || a == c) continue;
            if (i & 1) std::swap(a, b);
            out.insert(out.end(), { a, b, c });
        }
    } else if (g.type == 4) {  // D3DPT_TRIANGLELIST
        if (g.startIndex + g.primitives * 3 > indices.size()) return;
        for (uint32_t i = 0; i < g.primitives * 3; ++i) out.push_back(at(i));
    }
}
