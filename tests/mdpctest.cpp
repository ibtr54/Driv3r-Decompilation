// Console check of the MDPC model decoder (mdpc.cpp): every MDPC chunk of
// the files given is decoded, and for every model and level of detail
//   - each group's indices lie inside the index buffer,
//   - each vertex lies inside the model's vertex buffer (header +0x18),
//   - each position lies inside the model's bounding box (+0x28, 0.05 slack).
// Build: tests\build_mdpctest.bat. Usage: mdpctest <file>...
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../mdpc.h"

static std::vector<uint8_t> Read(const char* path)
{
    std::vector<uint8_t> d;
    FILE* f = fopen(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d.resize(size_t(n));
    if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}

static uint32_t U32(const std::vector<uint8_t>& d, size_t p)
{
    uint32_t v = 0;
    if (p + 4 <= d.size()) memcpy(&v, &d[p], 4);
    return v;
}

// Every MDPC chunk (CHNK containers, depth first).
static void Collect(const std::vector<uint8_t>& d, size_t base, std::vector<std::pair<size_t, size_t>>& out)
{
    if (base + 16 > d.size() || memcmp(&d[base], "CHNK", 4) != 0) return;
    uint32_t count = U32(d, base + 8);
    for (uint32_t k = 0; k < count; ++k) {
        size_t e = base + 16 + k * 16, o = base + U32(d, e + 4);
        if (memcmp(&d[e], "MDPC", 4) == 0) out.push_back({ o, U32(d, e + 12) });
        Collect(d, o, out);
    }
}

int main(int argc, char** argv)
{
    int bad = 0;
    for (int a = 1; a < argc; ++a) {
        std::vector<uint8_t> d = Read(argv[a]);
        std::vector<std::pair<size_t, size_t>> chunks;
        Collect(d, 0, chunks);
        size_t models = 0, tris = 0, groups = 0, outside = 0, range = 0, failed = 0, outsideM = 0, nonIdentity = 0;
        for (auto& c : chunks) {
            MdpcPackage p;
            if (!p.Load(d, c.first, c.second)) {
                ++failed;
                printf("  chunk at 0x%zx: %s\n", c.first, p.Error().c_str());
                continue;
            }
            models += p.models.size();
            std::vector<uint32_t> t;
            for (const MdpcModel& m : p.models) {
                if (m.vertexBuffer >= p.vertexBuffers.size()) { ++range; continue; }
                const MdpcVertexBuffer& vb = p.vertexBuffers[m.vertexBuffer];
                float lo[3], hi[3];
                for (int k = 0; k < 3; ++k) {
                    lo[k] = hi[k] = m.box[0][k];
                    for (int n = 1; n < 8; ++n) {
                        lo[k] = m.box[n][k] < lo[k] ? m.box[n][k] : lo[k];
                        hi[k] = m.box[n][k] > hi[k] ? m.box[n][k] : hi[k];
                    }
                }
                for (const auto& lod : m.lods)
                    for (const MdpcSubmesh& sm : lod)
                        for (const MdpcGroup& g : sm.groups) {
                            ++groups;
                            t.clear();
                            p.Triangles(g, t);
                            if (t.empty() && g.primitives) ++range;
                            tris += t.size() / 3;
                            for (uint32_t v : t) {
                                if (v >= vb.count) { ++range; break; }
                                const float* q = vb.Position(v);
                                for (int k = 0; k < 3; ++k)
                                    if (q[k] < lo[k] - 0.05f || q[k] > hi[k] + 0.05f) { ++outside; k = 3; }
                                // the same in the model's space (submesh matrix when +0x4a is set)
                                const float* M = sm.matrix;
                                float r[3];
                                sm.Transform(q, r);
                                for (int k = 0; k < 3; ++k)
                                    if (r[k] < lo[k] - 0.05f || r[k] > hi[k] + 0.05f) { ++outsideM; k = 3; }
                                bool ident = true;
                                for (int k = 0; k < 16; ++k) if (M[k] != ((k % 5 == 0) ? 1.f : 0.f)) ident = false;
                                if (!ident) ++nonIdentity;
                            }
                        }
            }
        }
        printf("%s: %zu MDPC, %zu models, %zu groups, %zu triangles; failed %zu, out of range %zu, "
               "outside the box %zu vertices (%zu with the submesh matrix; %zu under a non-identity matrix)\n", argv[a],
               chunks.size(), models, groups, tris, failed, range, outside, outsideM, nonIdentity);
        if (failed || range) bad = 1;
    }
    return bad;
}
