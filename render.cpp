#include "render.h"

#include <d3dx9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "mec.h"

namespace {

// 0x5d3140: the shared effect (0x5d30b0 / 0x5dc0e0), then one per id.
struct FxEntry { uint32_t va, size; int id; };
const FxEntry kFx[] = {
    { 0x71d038, 0x830, -1 },
    { 0x71d868, 0x20dc, 0x01 }, { 0x71f948, 0x244c, 0x02 }, { 0x721d98, 0x2464, 0x03 }, { 0x724200, 0x287c, 0x05 },
    { 0x726a80, 0x28a8, 0x06 }, { 0x729328, 0x3170, 0x09 }, { 0x72c498, 0x2a84, 0x07 }, { 0x72ef20, 0x1920, 0x04 },
    { 0x730840, 0x1900, 0x10 }, { 0x732140, 0x285c, 0x11 }, { 0x739038, 0x1840, 0x0a }, { 0x73a878, 0x19c4, 0x0b },
    { 0x73c240, 0x1840, 0x16 }, { 0x73da80, 0x1860, 0x0c }, { 0x73f2e0, 0x19d4, 0x0d }, { 0x740cb8, 0x1840, 0x15 },
    { 0x76d548, 0x2a84, 0x17 }, { 0x76ffd0, 0x2a84, 0x18 }, { 0x778188, 0x2ad8, 0x19 }, { 0x77ac60, 0x2ad8, 0x1a },
    { 0x762c30, 0x19d4, 0x0e }, { 0x764608, 0x18d0, 0x08 }, { 0x76a0a8, 0x16dc, 0x2b }, { 0x7424f8, 0x26ac, 0x1e },
    { 0x744ba8, 0x3a28, 0x1f }, { 0x7485d0, 0x3a20, 0x20 }, { 0x74bff0, 0x2c34, 0x21 }, { 0x74ec28, 0x2d44, 0x1d },
    { 0x751970, 0x2200, 0x22 }, { 0x753b70, 0x36f0, 0x23 }, { 0x757260, 0x3860, 0x24 }, { 0x75aac0, 0x388c, 0x25 },
    { 0x75e350, 0x2200, 0x26 }, { 0x765ed8, 0x227c, 0x27 }, { 0x768158, 0x1f4c, 0x28 }, { 0x7349a0, 0x18e4, 0x12 },
    { 0x736288, 0x16d4, 0x13 }, { 0x737960, 0x16d4, 0x14 }, { 0x772a58, 0x2b94, 0x1b }, { 0x7755f0, 0x2b94, 0x1c },
    { 0x761e50, 0x0ddc, 0x2c }, { 0x76b788, 0x0c00, 0x2d }, { 0x760550, 0x18fc, 0x2e }, { 0x76c388, 0x11bc, 0x2f },
    { 0x77d738, 0x1368, 0x29 }, { 0x77eaa0, 0x1378, 0x2a },
};

// 0x5d3140 clears the 0xb40-entry table 0x8accc8 and sets these entries;
// 0x5d68e0 reads entry (((s[4] & 0x7f) + id * 4) * 3 + s[5]) * 5 + s[6] for a
// substance s and render id id, and keeps id when the entry is 0.
const struct { int index, effect; } kSubstanceEffect[] = {
    { 75, 0x01 }, { 80, 0x02 }, { 95, 0x03 }, { 81, 0x05 }, { 96, 0x06 }, { 99, 0x09 }, { 1413, 0x18 },
    { 1653, 0x1c }, { 620, 0x0b }, { 735, 0x0c }, { 740, 0x0c }, { 755, 0x0c }, { 1533, 0x1a }, { 1815, 0x1e },
    { 1840, 0x20 }, { 1825, 0x1f }, { 2055, 0x22 }, { 2056, 0x23 }, { 2061, 0x24 }, { 2076, 0x25 },
};

int SubstanceEffect(int index)
{
    for (const auto& e : kSubstanceEffect)
        if (e.index == index) return e.effect;
    return 0;
}

// 0x5cc3d0: the render id of a model (model +0x20). With flag 2 (+0x1c) a
// model with any substance in slots 0-3 gets 0x17, then 2 -> 0x1b, 4 -> 0x19,
// 7 -> 0x10 (the substances are changed too; not done here, no city model
// has flag 2).
int RenderId(const MdpcModel& m)
{
    int id = 1;
    switch (m.vertexType) {
    case 0: id = 0; break;
    case 1: id = 1; break;
    case 2: id = 0x12; break;
    case 4: id = 0xc; break;
    case 5: id = 0x1e; break;
    case 6: id = 0x22; break;
    case 7: id = 0x10; break;
    default: break;
    }
    if (m.flags & 2) {
        for (int lod = 0; lod < 4; ++lod)
            if (!m.lods[lod].empty()) id = 0x17;
        if (m.vertexType == 2) id = 0x1b;
        else if (m.vertexType == 4) id = 0x19;
        else if (m.vertexType == 7) id = 0x10;
    }
    return id;
}

// 0x5ca590: vertex type -> declaration index (table 0x8acc70).
int DeclIndex(int vertexType)
{
    switch (vertexType) {
    case 2: case 3: case 4: return 2;
    case 5: return 5;
    case 6: return 6;
    case 8: return 7;
    default: return 1;
    }
}

typedef HRESULT(WINAPI* CreateEffectFn)(LPDIRECT3DDEVICE9, LPCVOID, UINT, const D3DXMACRO*, LPD3DXINCLUDE, DWORD,
                                        LPD3DXEFFECTPOOL, LPD3DXEFFECT*, LPD3DXBUFFER*);
typedef HRESULT(WINAPI* CreatePoolFn)(LPD3DXEFFECTPOOL*);

// a x b, row-major 4 x 4.
void Mul(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

void Transpose(const float* a, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = a[r * 4 + c];
}

// Every texture parameter of an effect gets the stand-in texture.
void SetTextures(ID3DXEffect* fx, IDirect3DTexture9* tex)
{
    D3DXEFFECT_DESC d;
    if (FAILED(fx->GetDesc(&d))) return;
    for (UINT p = 0; p < d.Parameters; ++p) {
        D3DXHANDLE h = fx->GetParameter(nullptr, p);
        D3DXPARAMETER_DESC pd;
        if (SUCCEEDED(fx->GetParameterDesc(h, &pd)) && pd.Type >= D3DXPT_TEXTURE && pd.Type <= D3DXPT_TEXTURECUBE)
            fx->SetTexture(h, tex);
    }
}

// A texture from a PCMP entry's DDS file, as stored: DXT1 / DXT3 / DXT5 or
// 32-bit (A8R8G8B8 / X8R8G8B8), all its mip levels. How the game creates
// its textures was not traced.
IDirect3DTexture9* CreateDds(IDirect3DDevice9* device, const std::vector<uint8_t>& dds)
{
    if (dds.size() < 128) return nullptr;
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, &dds[o], 4); return v; };
    UINT height = u32(12), width = u32(16), levels = std::max<UINT>(1, u32(28));
    uint32_t pfFlags = u32(80), fourcc = u32(84), bits = u32(88), amask = u32(104);
    D3DFORMAT fmt;
    UINT block = 0;  // bytes per 4 x 4 block, 0: 4 bytes per pixel
    if (pfFlags & 4) {  // DDPF_FOURCC
        if (fourcc == MAKEFOURCC('D', 'X', 'T', '1')) { fmt = D3DFMT_DXT1; block = 8; }
        else if (fourcc == MAKEFOURCC('D', 'X', 'T', '3')) { fmt = D3DFMT_DXT3; block = 16; }
        else if (fourcc == MAKEFOURCC('D', 'X', 'T', '5')) { fmt = D3DFMT_DXT5; block = 16; }
        else return nullptr;
    } else if (bits == 32 && u32(92) == 0xff0000 && u32(96) == 0xff00 && u32(100) == 0xff) {
        fmt = amask ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
    } else {
        return nullptr;
    }
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(device->CreateTexture(width, height, levels, 0, fmt, D3DPOOL_MANAGED, &tex, nullptr))) return nullptr;
    size_t off = 128;
    for (UINT l = 0; l < levels; ++l) {
        UINT w = std::max<UINT>(1, width >> l), h = std::max<UINT>(1, height >> l);
        UINT rows = block ? (h + 3) / 4 : h, rowBytes = block ? (w + 3) / 4 * block : w * 4;
        if (off + size_t(rows) * rowBytes > dds.size()) break;
        D3DLOCKED_RECT lr;
        if (FAILED(tex->LockRect(l, &lr, nullptr, 0))) break;
        for (UINT r = 0; r < rows; ++r)
            memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(r) * lr.Pitch, &dds[off + size_t(r) * rowBytes], rowBytes);
        tex->UnlockRect(l);
        off += size_t(rows) * rowBytes;
    }
    return tex;
}

// Render-state presets. A pair is (D3DRENDERSTATETYPE or D3DSAMPLERSTATETYPE,
// value).
struct StatePair { uint16_t state; uint32_t value; };
struct PresetPair { uint8_t preset; uint16_t state; uint32_t value; };
// 0x5d4b90 (called at the end of 0x5d9500): the device defaults, render states.
const StatePair kDefaultRS[] = {
    { 7, 1 }, { 168, 15 }, { 23, 4 }, { 14, 1 }, { 8, 3 }, { 9, 2 }, { 15, 1 }, { 16, 1 },
    { 19, 5 }, { 20, 6 }, { 22, 2 }, { 24, 1 }, { 25, 5 }, { 26, 1 }, { 27, 1 }, { 28, 0 },
    { 29, 0 }, { 35, 0 }, { 36, 0 }, { 37, 0 }, { 38, 0 }, { 48, 0 }, { 52, 0 }, { 53, 1 },
    { 54, 1 }, { 55, 1 }, { 56, 8 }, { 57, 0 }, { 58, 0xffffffff }, { 59, 0xffffffff }, { 60, 0xffffffff }, { 136, 1 },
    { 137, 0 }, { 139, 0 }, { 140, 0 }, { 141, 0 }, { 142, 0 }, { 143, 0 }, { 145, 1 }, { 146, 2 },
    { 147, 0 }, { 148, 0 }, { 151, 1 }, { 152, 0 }, { 154, 0 }, { 155, 0 }, { 156, 0 }, { 157, 0 },
    { 158, 0 }, { 159, 0 }, { 160, 0 }, { 162, 0xffffffff }, { 163, 0 }, { 165, 1 }, { 166, 0 }, { 167, 0 },
    { 170, 0 }, { 171, 1 }, { 174, 0 }, { 184, 0 }, { 185, 0 }, { 186, 1 }, { 187, 1 }, { 188, 1 },
    { 189, 8 }, { 190, 15 }, { 191, 15 }, { 192, 15 }, { 193, 0xffffffff }, { 194, 0 }, { 195, 0 }, { 206, 0 },
    { 207, 2 }, { 208, 1 }, { 209, 1 },
};
// ... and sampler states (stages 0..3): ADDRESSU / V / W = WRAP.
const StatePair kDefaultSampler[] = { { 1, 1 }, { 2, 1 }, { 3, 1 } };
// 0x5d9500: per preset its render states (0x5d4860) and sampler states
// (0x5d4790); the texture-stage lists (0x5d4800) are left out, the
// effects' pixel shaders do not use them.
const PresetPair kPresetRS[] = {
    { 0x01, 23, 4 }, { 0x01, 27, 0 }, { 0x01, 15, 1 }, { 0x01, 22, 2 }, { 0x01, 28, 1 }, { 0x01, 35, 0 },
    { 0x0a, 15, 1 }, { 0x0a, 27, 0 }, { 0x0a, 28, 1 }, { 0x0a, 29, 1 },
    { 0x0b, 15, 1 }, { 0x0b, 27, 1 }, { 0x0b, 28, 1 }, { 0x0b, 29, 1 },
    { 0x03, 15, 0 }, { 0x03, 27, 0 }, { 0x03, 52, 1 }, { 0x03, 28, 1 }, { 0x03, 57, 1 }, { 0x03, 56, 4 },
    { 0x03, 55, 1 }, { 0x03, 7, 1 }, { 0x03, 23, 4 }, { 0x03, 14, 1 },
    { 0x02, 23, 4 }, { 0x02, 27, 0 }, { 0x02, 15, 0 }, { 0x02, 22, 2 }, { 0x02, 24, 2 },
    { 0x04, 23, 4 }, { 0x04, 14, 0 }, { 0x04, 27, 0 }, { 0x04, 15, 1 }, { 0x04, 22, 2 }, { 0x04, 28, 0 },
    { 0x0c, 23, 4 }, { 0x0c, 28, 1 }, { 0x0c, 27, 1 }, { 0x0c, 29, 1 }, { 0x0c, 15, 1 }, { 0x0c, 25, 5 },
    { 0x0c, 24, 2 },
    { 0x09, 14, 0 }, { 0x09, 27, 1 }, { 0x09, 15, 1 }, { 0x09, 25, 5 }, { 0x09, 24, 2 },
    { 0x05, 23, 4 }, { 0x05, 28, 1 }, { 0x05, 27, 1 }, { 0x05, 15, 1 }, { 0x05, 25, 5 }, { 0x05, 24, 254 },
    { 0x06, 14, 0 }, { 0x06, 23, 4 }, { 0x06, 28, 1 }, { 0x06, 27, 1 }, { 0x06, 15, 1 }, { 0x06, 25, 4 },
    { 0x06, 24, 254 },
    { 0x07, 14, 1 }, { 0x07, 27, 1 }, { 0x07, 15, 1 }, { 0x07, 25, 5 }, { 0x07, 24, 128 },
    { 0x08, 14, 0 }, { 0x08, 27, 1 }, { 0x08, 15, 1 }, { 0x08, 25, 8 }, { 0x08, 22, 1 }, { 0x08, 24, 128 },
    { 0x13, 27, 1 }, { 0x13, 15, 1 },
    { 0x14, 23, 4 }, { 0x14, 27, 1 }, { 0x14, 15, 1 }, { 0x14, 25, 8 },
    { 0x15, 23, 4 }, { 0x15, 28, 0 }, { 0x15, 15, 1 },
    { 0x16, 23, 4 }, { 0x16, 27, 1 }, { 0x16, 15, 0 }, { 0x16, 25, 5 }, { 0x16, 24, 1 }, { 0x16, 171, 1 },
    { 0x16, 19, 5 }, { 0x16, 20, 2 },
    { 0x0e, 23, 4 }, { 0x0e, 14, 0 }, { 0x0e, 27, 1 }, { 0x0e, 15, 1 }, { 0x0e, 25, 5 }, { 0x0e, 24, 1 },
    { 0x0f, 23, 4 }, { 0x0f, 14, 0 }, { 0x0f, 27, 1 }, { 0x0f, 15, 1 }, { 0x0f, 25, 5 }, { 0x0f, 24, 1 },
    { 0x10, 23, 4 }, { 0x10, 28, 1 }, { 0x10, 27, 1 }, { 0x10, 15, 1 }, { 0x10, 25, 5 }, { 0x10, 24, 1 },
    { 0x10, 171, 1 }, { 0x10, 19, 7 }, { 0x10, 20, 2 },
    { 0x0d, 23, 2 }, { 0x0d, 14, 0 }, { 0x0d, 27, 1 }, { 0x0d, 15, 1 }, { 0x0d, 25, 5 }, { 0x0d, 22, 2 },
    { 0x0d, 24, 1 },
    { 0x18, 28, 0 },
    { 0x1a, 28, 0 },
    { 0x19, 28, 0 }, { 0x19, 23, 8 }, { 0x19, 27, 1 }, { 0x19, 15, 1 }, { 0x19, 14, 0 },
    { 0x17, 7, 0 }, { 0x17, 14, 0 }, { 0x17, 23, 8 }, { 0x17, 28, 0 }, { 0x17, 23, 4 }, { 0x17, 27, 1 },
    { 0x17, 15, 1 },
    { 0x11, 28, 0 }, { 0x11, 52, 1 }, { 0x11, 15, 0 }, { 0x11, 25, 8 }, { 0x11, 14, 0 }, { 0x11, 52, 1 },
    { 0x11, 23, 5 }, { 0x11, 9, 1 }, { 0x11, 56, 8 }, { 0x11, 54, 1 }, { 0x11, 53, 1 }, { 0x11, 57, 1 },
    { 0x11, 58, 0xffffffff }, { 0x11, 59, 0xffffffff }, { 0x11, 168, 0 }, { 0x11, 55, 8 }, { 0x11, 22, 2 },
    { 0x12, 28, 0 }, { 0x12, 52, 1 }, { 0x12, 15, 0 }, { 0x12, 27, 0 }, { 0x12, 25, 8 }, { 0x12, 14, 0 },
    { 0x12, 52, 1 }, { 0x12, 23, 5 }, { 0x12, 9, 1 }, { 0x12, 56, 8 }, { 0x12, 54, 1 }, { 0x12, 53, 1 },
    { 0x12, 57, 1 }, { 0x12, 58, 0xffffffff }, { 0x12, 59, 0xffffffff }, { 0x12, 168, 0 }, { 0x12, 55, 7 }, { 0x12, 22, 3 },
    { 0x20, 15, 0 }, { 0x20, 27, 0 }, { 0x20, 25, 8 }, { 0x20, 28, 0 }, { 0x20, 7, 1 }, { 0x20, 23, 8 },
    { 0x20, 14, 0 }, { 0x20, 19, 2 }, { 0x20, 20, 1 }, { 0x20, 168, 8 },
    { 0x1f, 15, 0 }, { 0x1f, 27, 0 }, { 0x1f, 25, 8 }, { 0x1f, 52, 1 }, { 0x1f, 28, 0 }, { 0x1f, 57, 1 },
    { 0x1f, 56, 4 }, { 0x1f, 55, 1 }, { 0x1f, 7, 1 }, { 0x1f, 23, 8 }, { 0x1f, 14, 0 }, { 0x1f, 19, 2 },
    { 0x1f, 20, 1 }, { 0x1f, 168, 8 },
    { 0x1c, 27, 1 }, { 0x1c, 52, 1 }, { 0x1c, 57, 1 }, { 0x1c, 56, 4 }, { 0x1c, 55, 1 }, { 0x1c, 7, 1 },
    { 0x1c, 23, 4 }, { 0x1c, 14, 1 }, { 0x1c, 168, 7 }, { 0x1c, 171, 1 }, { 0x1c, 19, 7 }, { 0x1c, 20, 8 },
    { 0x1b, 52, 1 }, { 0x1b, 29, 1 }, { 0x1b, 57, 1 }, { 0x1b, 56, 4 }, { 0x1b, 55, 1 }, { 0x1b, 168, 7 },
    { 0x1b, 171, 1 }, { 0x1b, 19, 5 }, { 0x1b, 20, 6 },
    { 0x1e, 168, 0 },
    { 0x1d, 23, 4 }, { 0x1d, 14, 0 }, { 0x1d, 27, 1 }, { 0x1d, 15, 1 }, { 0x1d, 25, 5 },
    { 0x26, 23, 4 }, { 0x26, 14, 0 }, { 0x26, 28, 0 }, { 0x26, 27, 0 }, { 0x26, 15, 0 }, { 0x26, 168, 0 },
    { 0x27, 27, 1 }, { 0x27, 15, 0 }, { 0x27, 23, 4 }, { 0x27, 14, 0 }, { 0x27, 28, 0 }, { 0x27, 57, 1 },
    { 0x27, 55, 3 }, { 0x27, 56, 8 }, { 0x27, 53, 1 }, { 0x27, 52, 1 }, { 0x27, 168, 8 }, { 0x27, 19, 5 },
    { 0x27, 20, 2 },
    { 0x28, 171, 1 }, { 0x28, 19, 2 }, { 0x28, 20, 2 }, { 0x28, 27, 1 }, { 0x28, 14, 0 }, { 0x28, 168, 8 },
    { 0x2a, 22, 1 },
    { 0x2b, 171, 1 }, { 0x2b, 19, 5 }, { 0x2b, 20, 2 },
    { 0x2c, 27, 0 }, { 0x2c, 15, 0 },
    { 0x2d, 171, 3 }, { 0x2d, 19, 5 }, { 0x2d, 20, 2 },
    { 0x30, 14, 0 },
    { 0x31, 14, 0 },
    { 0x23, 15, 0 }, { 0x23, 27, 0 }, { 0x23, 28, 0 }, { 0x23, 7, 0 },
    { 0x25, 15, 0 }, { 0x25, 27, 0 }, { 0x25, 28, 0 }, { 0x25, 171, 1 }, { 0x25, 19, 2 }, { 0x25, 20, 2 },
    { 0x25, 7, 0 },
    { 0x21, 15, 1 }, { 0x21, 27, 1 }, { 0x21, 28, 0 }, { 0x21, 7, 0 },
    { 0x22, 15, 1 }, { 0x22, 27, 1 }, { 0x22, 28, 0 }, { 0x22, 171, 1 }, { 0x22, 19, 5 }, { 0x22, 20, 2 },
    { 0x22, 7, 0 },
    { 0x24, 15, 0 }, { 0x24, 27, 0 }, { 0x24, 28, 0 }, { 0x24, 7, 0 },
};
const PresetPair kPresetSampler[] = {
    { 0x18, 1, 3 }, { 0x18, 3, 3 }, { 0x18, 2, 3 },
    { 0x17, 1, 3 }, { 0x17, 3, 3 }, { 0x17, 2, 3 },
    { 0x2e, 1, 3 },
    { 0x2f, 2, 3 }, { 0x2f, 3, 3 },
};

// 0x5cb9d0 (jump table 0x5cbaec / 0x5cba84): substance type -> preset;
// above 0x35: 1.
int TypePreset(int type)
{
    static const uint8_t kPreset[0x36] = {
        0x04, 0x1e, 0x20, 0x1f, 0x27, 0x21, 0x02, 0x0c, 0x01, 0x1e, 0x0e, 0x0a, 0x0a, 0x1e, 0x0f, 0x10, 0x0c, 0x0c,
        0x0c, 0x09, 0x07, 0x04, 0x05, 0x09, 0x08, 0x15, 0x26, 0x11, 0x08, 0x0c, 0x13, 0x0d, 0x0d, 0x17, 0x19, 0x19,
        0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x18, 0x17, 0x17, 0x17, 0x17, 0x17, 0x1a, 0x17,
    };
    return type >= 0 && type < 0x36 ? kPreset[type] : 1;
}

// 0x5d8340: the presets added for byte 1 (b1) and the flags of a substance,
// as bits (n: preset 0x2a + n). The check of the render context [0x8afab0]
// (8 / 9 keep 0x2c out) is left out: it was 0 in every traced draw.
unsigned ExtraPresets(int b1, int flags)
{
    unsigned m = 0;
    if ((flags & 0x10) || b1 == 1 || b1 == 4) m |= 1u << 0;   // 0x2a cull none
    bool keep = false;
    if (flags & 8) { m |= 1u << 1; keep = true; }               // 0x2b additive
    if (flags & 0x80) { m |= 1u << 3; keep = true; }            // 0x2d BLENDOP 3
    if (((flags & 4) || b1 == 2 || b1 == 5) && !keep) m |= 1u << 2;  // 0x2c no blend / test
    if (flags & 0x40) m |= 1u << 4;                             // 0x2e clamp U
    if (flags & 0x20) m |= 1u << 5;                             // 0x2f clamp V, W
    if (flags & 1) m |= 1u << 6;                                // 0x30 no z write
    if (flags & 2) m |= 1u << 7;                                // 0x31 no z write
    return m;
}

// 0x5cdaa0: the request's mode -> its preset (0: none), applied by 0x5d4440
// after the type and extra presets unless the type is 2..4.
int ModePreset(int mode)
{
    switch (mode) {
    case 3: case 4: return 0x11;
    case 5: return 0x12;
    case 6: return 0x1b;
    case 7: return 0x1c;
    case 8: return 0x28;
    case 9: return 0x06;
    case 10: return 0x14;
    case 11: return 0x16;
    default: return 0;
    }
}

// 0x5d4330 (commit of the request): in mode 7 the types whose byte in the
// table 0x5d4428 (types 11..24) is 0 use mode 6; in mode 6 the types 6 and 8
// use mode 7.
int EffectiveMode(int mode, int type)
{
    static const uint8_t kKeep7[14] = { 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0 };
    if (mode == 7 && type >= 11 && type <= 24 && !kKeep7[type - 11]) return 6;
    if (mode == 6 && (type == 6 || type == 8)) return 7;
    return mode;
}

// 0x5e06f0 (asked per group by 0x5e05b0): whether a group whose substance
// has this type is drawn in the request's mode. Modes 6 / 7: types 2..24
// whose byte in the table 0x5e0758 is 0; other modes: every type but 0..5.
// (Its range check against 0x8df140 / 0x7e5f04 is 0..100: always true.)
bool DrawnInMode(int type, int mode)
{
    static const uint8_t kSkip67[23] = { 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0 };
    if (mode == 6 || mode == 7) return type >= 2 && type <= 24 && !kSkip67[type - 2];
    return type < 0 || type > 5;
}

// 0x5d2ae8: groups whose substance type is 0x11, 0x1d or 0x1e go to the
// scene's list 2 (drawn last, after the shadows), the others to list 0.
bool LateType(int type) { return type == 0x11 || type == 0x1d || type == 0x1e; }

// g_fExtrusion of every shadow volume draw: effect 0x2b's per-object call
// (vtbl 0x78e4b8 +0x20 = 0x5dedf0) sets it with SetFloat 20.0 (0x5def3c);
// the trace has c9 = (20, 0, 0, 0). (0x5defb0, which reads the object's
// +0x70, is +0x20 of the next vtable 0x78e510.)
const float kExtrusion = 20.0f;

// A vehicle part's model id (object +0x60) as the shadow volume sees it:
// package uid 0x39 (the 0xe400 branch of 0x5dedf0; which uid the vehicle
// packages get at run time was taken from that trace, not from the loader).
const uint16_t kVehicleModelId = 0xe400;

// 0x5dedf0: the volume's World. d = camera position (camera +0x70) - the
// object's position (+0x40, row 3); the position moves by -0.1 x d
// normalized (0x4025d0, length of x, y, z), or by -0.005 x d for models
// whose id (+0x60) & 0xfc00 is 0xe400 (package uid 0x39; none placed in
// the cities; the vehicles: the original's car volumes at start point 0 are
// moved by -0.005 x d, e.g. (0, -0.01385, -0.027) for d = (0, 2.77, 5.34)),
// i.e. away from the camera; then transposed. It replaces the
// World of 0x5de4e0 (slot +0x24, called first by 0x5e05b0), so the submesh
// matrix is not used (no volume submesh in the cities has one).
void VolumeWorld(uint16_t model, const float* matrix, const float eye[3], float world[16])
{
    float object[16];
    memcpy(object, matrix, sizeof(object));
    float d[3] = { eye[0] - object[12], eye[1] - object[13], eye[2] - object[14] };
    float s = -0.005f;
    if ((model & 0xfc00) != 0xe400) {
        float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        s = n > 0 ? -0.1f / n : 0.0f;
    }
    for (int k = 0; k < 3; ++k) object[12 + k] += s * d[k];
    Transpose(object, world);
}

// 0x5cc3d0, for a model without flag 2 (+0x1c; none in the cities): each
// substance of its slots 0..3 of type 0x19 becomes 8; one of type 0xf gets
// byte 1 = (flags == 0x10) when byte 1 is 2, flag 4 cleared and byte 6 = 4.
// (A substance of type 6 sets the model's flags to 0xffff00bb; not used
// here.) Runs after the conversion and before the effect (0x5d68e0).
void ModelSubstance(uint8_t s[8])
{
    if (s[0] == 0x19) {
        s[0] = 8;
    } else if (s[0] == 0xf) {
        if (s[1] == 2) s[1] = s[2] == 0x10;
        s[2] &= 0xfb;
        s[6] = 4;
    }
}

// The models the renderer can draw: declaration index 1 / 2 with 48-byte
// vertices, or 5 (the vehicles) with 60.
bool Drawable(const MdpcModel& m, const MdpcPackage& data)
{
    if (m.vertexBuffer >= data.vertexBuffers.size()) return false;
    uint32_t stride = data.vertexBuffers[m.vertexBuffer].stride;
    int decl = DeclIndex(m.vertexType);
    return ((decl == 1 || decl == 2) && stride == 48) || (decl == 5 && stride == 60);
}

}  // namespace

void CameraMatrix(const float eye[3], const float at[3], float fov, float aspect, float out[16])
{
    // z points from the target back to the eye (right-handed).
    float z[3] = { eye[0] - at[0], eye[1] - at[1], eye[2] - at[2] };
    float n = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (float& v : z) v /= n;
    float x[3] = { z[2], 0, -z[0] };  // up (0, 1, 0) x z
    n = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    for (float& v : x) v /= n;
    float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    const float view[16] = {
        x[0], y[0], z[0], 0,
        x[1], y[1], z[1], 0,
        x[2], y[2], z[2], 0,
        -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]), -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]),
        -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]), 1,
    };
    // 0x57ce50 (camera +0x80): t = tan(+0x180 x 0.5); x scale 1 / t, y scale
    // 1 / tan(atan(t / +0x184)) (the fpatan / fptan pair, x 2 x 0.5);
    // fixed z row 0xbf800106 / 0xbe800106 (near 0.25, far about 8013) and
    // w = -z.
    float t = std::tan(fov * 0.5f), xs = 1.0f / t, ys = 1.0f / std::tan(std::atan(t / aspect));
    uint32_t qBits = 0xbf800106, qnBits = 0xbe800106;
    float q, qn;
    memcpy(&q, &qBits, 4);
    memcpy(&qn, &qnBits, 4);
    const float proj[16] = { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, q, -1, 0, 0, qn, 0 };
    Mul(view, proj, out);
}

bool CityRenderer::Init(IDirect3DDevice9* device, const std::string& exePath)
{
    Release();
    device_ = device;
    dll_ = LoadLibraryA("d3dx9_43.dll");
    auto createEffect = dll_ ? reinterpret_cast<CreateEffectFn>(GetProcAddress(dll_, "D3DXCreateEffect")) : nullptr;
    auto createPool = dll_ ? reinterpret_cast<CreatePoolFn>(GetProcAddress(dll_, "D3DXCreateEffectPool")) : nullptr;
    if (!createEffect || !createPool) { error_ = "d3dx9_43.dll not found"; return false; }
    std::vector<uint8_t> exe = ReadFile(exePath);
    if (exe.empty()) { error_ = "cannot read " + exePath; return false; }
    if (FAILED(device_->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &white_, nullptr))) {
        error_ = "texture";
        return false;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(white_->LockRect(0, &lr, nullptr, 0))) {
        *static_cast<uint32_t*>(lr.pBits) = 0xffffffff;
        white_->UnlockRect(0);
    }
    // Declaration index 1 / 2 (0x5d0680): position, normal, texcoord, tangent.
    const D3DVERTEXELEMENT9 elems[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0 },
        D3DDECL_END(),
    };
    if (FAILED(device_->CreateVertexDeclaration(elems, &decl_))) { error_ = "vertex declaration"; return false; }
    // Declaration index 5 (0x5d099b, handle 0x8acc84; the vehicles' vertex
    // type 5): position, normal, texcoord, position 1, normal 1 (the damaged
    // shape, blended by g_DamageFactor), tangent float1; 60 bytes.
    const D3DVERTEXELEMENT9 elems5[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 32, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 1 },
        { 0, 44, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 1 },
        { 0, 56, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0 },
        D3DDECL_END(),
    };
    if (FAILED(device_->CreateVertexDeclaration(elems5, &decl5_))) { error_ = "vertex declaration 5"; return false; }
    if (FAILED(createPool(&pool_))) { error_ = "effect pool"; return false; }
    // The data sections are mapped at file offset + 0x400000.
    for (const FxEntry& e : kFx) {
        uint32_t off = e.va - 0x400000;
        if (off + e.size > exe.size()) continue;
        ID3DXEffect* fx = nullptr;
        if (FAILED(createEffect(device_, &exe[off], e.size, nullptr, nullptr, 0x100, pool_, &fx, nullptr))) {
            char id[16];
            snprintf(id, sizeof(id), " 0x%x", unsigned(e.id));
            failedEffects += id;
            continue;
        }
        // 0x5d5790: the first valid technique (FindNextValidTechnique(NULL)).
        // With the game's own (statically linked, older) D3DX that is never
        // t_2x on this machine: asked in the running original (proxy:
        // GetCurrentTechnique of every effect in [0x8deff4] +0x2c..), all
        // effects run t_11, or t_14 / t_13 where they have them. D3DX 9.43
        // accepts t_2x, so t_2x is skipped here.
        D3DXHANDLE tech = nullptr, first = nullptr;
        while (SUCCEEDED(fx->FindNextValidTechnique(tech, &tech)) && tech) {
            if (!first) first = tech;
            D3DXTECHNIQUE_DESC desc;
            if (SUCCEEDED(fx->GetTechniqueDesc(tech, &desc)) && desc.Name && strcmp(desc.Name, "t_2x") != 0) break;
        }
        if (!tech) tech = first;
        if (tech) fx->SetTechnique(tech);
        SetTextures(fx, white_);
        if (e.id < 0) {
            shared_ = fx;
            // 0x5dc9a0: ViewProjection by semantic, the rest by name.
            viewProj_ = fx->GetParameterBySemantic(nullptr, "ViewProjection");
            camPos_ = fx->GetParameterByName(nullptr, "s_CamPos");
            ambient_ = fx->GetParameterByName(nullptr, "s_LightAmbientColour");
            lightDir_ = fx->GetParameterByName(nullptr, "s_LightDirectionalDirection");
            lightColour_ = fx->GetParameterByName(nullptr, "s_LightDirectionalColour");
            fogParams_ = fx->GetParameterByName(nullptr, "s_FogParams");
            envTex_ = fx->GetParameterByName(nullptr, "s_TextureEnvironment");  // 0x5dca79
            worldToEnv_ = fx->GetParameterByName(nullptr, "s_WorldToEnvMap");   // shared +0x2c
            shineTex_ = fx->GetParameterByName(nullptr, "s_TextureShine");
            continue;
        }
        Effect& f = fx_[e.id];
        f.fx = fx;
        // 0x5de7b0: "World" by semantic, g_fAlpha, g_fBrightness by name.
        f.world = fx->GetParameterBySemantic(nullptr, "World");
        f.alpha = fx->GetParameterByName(nullptr, "g_fAlpha");
        f.brightness = fx->GetParameterByName(nullptr, "g_fBrightness");
        f.diffuse = fx->GetParameterByName(nullptr, "g_TextureDiffuse");
        f.normalMap = fx->GetParameterByName(nullptr, "g_TextureNormalMap");
        f.envSpec = fx->GetParameterByName(nullptr, "g_envSpecColourAndLerpFactor");
        f.extrusion = e.id == 0x2b ? fx->GetParameterByName(nullptr, "g_fExtrusion") : nullptr;
        // 0x5e1080 (every effect's texture binder) and 0x5df370 (the vehicle
        // effects' class, vtbl 0x78e510).
        f.clean = fx->GetParameterByName(nullptr, "g_TextureClean");
        f.damageTex = fx->GetParameterByName(nullptr, "g_TextureDamage");
        f.mask = fx->GetParameterByName(nullptr, "g_TextureColourednessMask");
        f.colourWeights = fx->GetParameterByName(nullptr, "g_colourWeightsVec");
        f.damageFactor = fx->GetParameterByName(nullptr, "g_DamageFactor");
        if (f.extrusion) fx->SetFloat(f.extrusion, kExtrusion);
        if (e.id == 7 || e.id == 9) {  // class 0x5d3da0, handles 0x5deaa0
            f.lerp = fx->GetParameterByName(nullptr, "g_TextureLerp");
            f.diffuse2[0] = fx->GetParameterByName(nullptr, "g_TextureDiffuse1");
            f.diffuse2[1] = fx->GetParameterByName(nullptr, "g_TextureDiffuse2");
            f.normal2[0] = fx->GetParameterByName(nullptr, "g_TextureNormalMap1");
            f.normal2[1] = fx->GetParameterByName(nullptr, "g_TextureNormalMap2");
        }
        // 0x5de330: g_fBrightness 0x3f26a6a7; g_fAlpha comes from the drawn
        // object (+0x64, not traced): 1 here.
        uint32_t bits = 0x3f26a6a7;
        float brightness;
        memcpy(&brightness, &bits, 4);
        if (f.brightness) fx->SetFloat(f.brightness, brightness);
        if (f.alpha) fx->SetFloat(f.alpha, 1.0f);
        ++effects;
    }
    if (!shared_) { error_ = "shared effect not created"; return false; }
    return true;
}

void CityRenderer::AddPackage(const MdpcPackage& data)
{
    Package p;
    p.data = &data;
    for (const MdpcVertexBuffer& vb : data.vertexBuffers) {
        IDirect3DVertexBuffer9* b = nullptr;
        void* ptr;
        if (SUCCEEDED(device_->CreateVertexBuffer(UINT(vb.data.size()), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &b, nullptr)) &&
            SUCCEEDED(b->Lock(0, 0, &ptr, 0))) {
            memcpy(ptr, vb.data.data(), vb.data.size());
            b->Unlock();
        }
        p.vbs.push_back(b);
    }
    if (!data.indices.empty()) {
        UINT bytes = UINT(data.indices.size() * 2);
        void* ptr;
        if (SUCCEEDED(device_->CreateIndexBuffer(bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &p.ib, nullptr)) &&
            SUCCEEDED(p.ib->Lock(0, 0, &ptr, 0))) {
            memcpy(ptr, data.indices.data(), bytes);
            p.ib->Unlock();
        }
    }
    for (const MdpcTexture& t : data.textures) {
        IDirect3DTexture9* tex = CreateDds(device_, t.dds);
        (tex ? textures : texturesSkipped)++;
        p.textures.push_back(tex);
    }
    packages_.push_back(std::move(p));
}

// A group's effect, textures and states from its material (package uid << 16
// | index) and the render id (0x5d68e0, 0x5e0970, first substance);
// modelSubstance: the model's substance changes (model flag 2 clear).
CityRenderer::Package::Group CityRenderer::GroupFor(uint32_t material, int id, bool modelSubstance, bool layout,
                                                   const std::map<uint32_t, int>& byUid, int self, bool vehicle)
{
    Package::Group gr;
    gr.fx = id;
    gr.vehicle = vehicle;
    // No substance: the binder's request keeps type 8 (0x5e0900).
    uint8_t s[8] = { 8 };
    // Package 0xfffd: the model's own (0x5cbea0; the vehicles' materials).
    auto it = byUid.find(material >> 16);
    int ownerIndex = (material >> 16) == 0xfffd ? self : it != byUid.end() ? it->second : -1;
    if (material != 0xffff0000 && material != 0xcccccccc && ownerIndex >= 0) {
        const Package& owner = packages_[size_t(ownerIndex)];
        uint32_t mi = material & 0xffff;
        if (mi < owner.data->materials.size() && !owner.data->materials[mi].empty()) {
            const MdpcSubstance& sub = owner.data->materials[mi][0];
            memcpy(s, sub.bytes, 8);
            if (modelSubstance) ModelSubstance(s);
            int index = (((s[4] & 0x7f) + id * 4) * 3 + s[5]) * 5 + s[6];
            if (int e = SubstanceEffect(index)) gr.fx = e;
            // Texture references are entries of the material's own package.
            if (!sub.textures.empty() && size_t(sub.textures[0]) < owner.textures.size())
                gr.diffuse = owner.textures[sub.textures[0]];
            gr.normalBound = s[6] == 1 || s[6] == 4;
            if (gr.normalBound && sub.textures.size() >= 2 && size_t(sub.textures[1]) < owner.textures.size())
                gr.normal = owner.textures[sub.textures[1]];
            if (vehicle) {
                // 0x5e0b20 by the substance's +0x15: texture n is entry n, or
                // entry 0 past the count (0x5d8830); nullptr where the game
                // binds the global texture 0xf00d0004.
                gr.colourMode = sub.colourMode;
                gr.textureCount = uint8_t(sub.textures.size());
                auto tex = [&](size_t n) {
                    if (sub.textures.empty()) return static_cast<IDirect3DTexture9*>(nullptr);
                    size_t t = size_t(sub.textures[n < sub.textures.size() ? n : 0]);
                    return t < owner.textures.size() ? owner.textures[t] : nullptr;
                };
                switch (sub.colourMode) {
                case 0x10:  // 0x5e0b40
                    gr.clean = tex(0), gr.mask = tex(1), gr.damage = tex(2);
                    break;
                case 4:  // 0x5e0bab
                    gr.clean = tex(0), gr.mask = tex(1), gr.damage = tex(1);
                    break;
                case 8:  // 0x5e0c3a
                    gr.clean = tex(0), gr.damage = tex(1);
                    break;
                case 0:  // 0x5e0ce3: only with one texture
                    if (sub.textures.size() == 1) gr.clean = tex(0);
                    break;
                default:  // 0x5e0db3
                    gr.clean = tex(0), gr.mask = tex(1), gr.damage = tex(2);
                    break;
                }
            }
            // Several substances: their textures (0x5d92b0, 0x5de890).
            if (owner.data->materials[mi].size() > 1 && mi < owner.data->materialFlags.size()) {
                auto key = std::make_pair(it->second, mi);
                auto found = animOf_.find(key);
                if (found == animOf_.end()) {
                    Anim a;
                    a.animated = (owner.data->materialFlags[mi] & 1) != 0;
                    a.speed = owner.data->materialSpeed[mi];
                    for (const MdpcSubstance& f : owner.data->materials[mi]) {
                        auto tex = [&](size_t k) {
                            return k < f.textures.size() && size_t(f.textures[k]) < owner.textures.size()
                                       ? owner.textures[f.textures[k]]
                                       : nullptr;
                        };
                        a.tex0.push_back(tex(0));
                        a.tex1.push_back(tex(1));
                    }
                    found = animOf_.emplace(key, int(anims_.size())).first;
                    anims_.push_back(std::move(a));
                }
                gr.anim = found->second;
            }
        }
    }
    if (!layout || gr.fx <= 0 || gr.fx >= 0x30 || !fx_[gr.fx].fx) gr.fx = -1;
    gr.state = StateFor(s);
    gr.type = int8_t(s[0]);
    gr.late = LateType(gr.type);
    if (!gr.late && DrawnInMode(gr.type, 7)) gr.redraw = StateFor(s, EffectiveMode(7, gr.type));
    return gr;
}

// The groups of every model's slots 0..4; byUid: package uid -> index into
// packages_; self: the package itself (uid 0xfffd); vehicle: kind 3.
void CityRenderer::ResolveGroups(Package& p, const std::map<uint32_t, int>& byUid, int self, bool vehicle)
{
    p.groupOf.resize(p.data->models.size());
    for (size_t m = 0; m < p.data->models.size(); ++m) {
        const MdpcModel& model = p.data->models[m];
        int id = RenderId(model);
        bool layout = Drawable(model, *p.data);
        p.groupOf[m].resize(5);
        for (int lod = 0; lod < 5; ++lod)
        for (const MdpcSubmesh& sm : model.lods[lod]) {
            std::vector<Package::Group> groups;
            for (const MdpcGroup& g : sm.groups)
                groups.push_back(GroupFor(g.material, id, !(model.flags & 2), layout, byUid, self, vehicle));
            p.groupOf[m][lod].push_back(std::move(groups));
        }
    }
    // 0x5ca020: slot 5 gets the mask 0x20 when it has submeshes, else slot 6
    // (the files have 0 in both).
    p.shadowSlot.assign(p.data->models.size(), -1);
    for (size_t m = 0; m < p.data->models.size(); ++m) {
        const MdpcModel& model = p.data->models[m];
        if (!Drawable(model, *p.data) || !fx_[0x2b].fx) continue;
        if (!model.lods[5].empty()) p.shadowSlot[m] = 5;
        else if (!model.lods[6].empty()) p.shadowSlot[m] = 6;
    }
}

bool CityRenderer::Upload(const std::vector<MdpcPackage>& packages, size_t first, size_t end, const CityPlacement& city,
                          const MdpcPackage* sky, const MdpcPackage* sfx, const VehicleDraw* vehicle)
{
    if (!device_) { error_ = "no device"; return false; }
    std::map<uint32_t, int> byUid;  // the table 0x8b5310 (0x57c058)
    std::map<int, size_t> perPreset, perEffect;
    for (size_t k = first; k < end && k < packages.size(); ++k) {
        byUid[packages[k].uid] = int(packages_.size());
        AddPackage(packages[k]);
    }
    for (Package& p : packages_) ResolveGroups(p, byUid);
    // Shadow volumes: the request keeps type 8, byte 1 and flags 0 (every
    // traced mode 4 / 5 draw), modes 4 and 5.
    static const uint8_t kVolume[8] = { 8 };
    shadowState_[0] = StateFor(kVolume, 4);
    shadowState_[1] = StateFor(kVolume, 5);
    // The screen quads of the shadow pass: types 2 (preset 0x20) and 3 (0x1f),
    // requests of mode 7 (no mode preset for types 2..4).
    static const uint8_t kQuad[2][8] = { { 2 }, { 3 } };
    alphaState_[0] = StateFor(kQuad[0], 7);
    alphaState_[1] = StateFor(kQuad[1], 7);
    // The sky's materials are its own (uid 0x34; city packages may use the
    // same small uid).
    skyInfo.clear();
    if (sky && !sky->models.empty()) {
        skyPackage_ = int(packages_.size());
        AddPackage(*sky);
        ResolveGroups(packages_.back(), { { sky->uid, skyPackage_ } });
        // 0x57bff0: when the package with uid 0x34 (the sky) loads, the
        // environment cube [0x8afabc] is made from its material 1 and the
        // shine cube [0x8afab8] from material 0; with the ride's time of day
        // 3 (setup vtbl+0x64) the environment is material 0 and no shine.
        if (sky->uid == 0x34) {
            bool night = timeOfDay_ == 3;
            cubes_[1] = CubeFrom(packages_.back(), night ? 0 : 1);
            if (!night) cubes_[0] = CubeFrom(packages_.back(), 0);
        }
        for (const auto& sm : packages_.back().groupOf[0][0])
            for (const Package::Group& g : sm) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%sgroup: effect %d, preset 0x%02x", skyInfo.empty() ? "" : "; ", g.fx,
                         unsigned(states_[g.state].base));
                skyInfo += buf;
            }
    }
    city_ = &city;
    instanceOf_.assign(city.instances.size(), -1);
    cells = city.cells.size();
    lodScale = 1.0f;  // [0x7a1a68] starts at 1.0
    for (const CityInstance& in : city.instances) {
        if (!in.HasModel()) continue;
        auto it = byUid.find(in.PackageUid());
        if (it == byUid.end() || in.ModelIndex() >= packages_[it->second].data->models.size()) continue;
        instanceOf_[size_t(&in - city.instances.data())] = int(instances_.size());
        Instance inst;
        inst.in = &in;
        inst.package = it->second;
        inst.model = in.ModelIndex();
        const MdpcModel& m = packages_[it->second].data->models[inst.model];
        float c[3] = {};
        if (m.box[0][0] < 1e30f && m.box[0][0] > -1e30f)
            for (int k = 0; k < 8; ++k)
                for (int j = 0; j < 3; ++j) c[j] += m.box[k][j] / 8.0f;
        in.Transform(c, inst.centre);
        for (const auto& slot : packages_[it->second].groupOf[inst.model])
        for (const auto& sm : slot)
            for (const Package::Group& g : sm) {
                (g.fx >= 0 ? groupsDrawn : groupsSkipped)++;
                if (g.fx < 0) continue;
                if (!g.diffuse) ++groupsUntextured;
                ++perPreset[states_[g.state].base];
                ++perEffect[g.fx];
                if (g.late) ++lateGroups;
                if (!DrawnInMode(g.type, 2)) ++hiddenGroups;
            }
        if (packages_[it->second].shadowSlot[inst.model] >= 0) ++shadowInstances;
        instances_.push_back(inst);
    }
    // The sea's tiles (GWTR): models of the city's packages, submitted by
    // the water object (0x451030), not by a quadtree.
    waterTiles_.clear();
    for (const CityInstance& in : city.water.tiles) {
        auto it = byUid.find(in.PackageUid());
        if (!in.HasModel() || it == byUid.end() || in.ModelIndex() >= packages_[it->second].data->models.size()) continue;
        Instance inst;
        inst.in = &in;
        inst.package = it->second;
        inst.model = in.ModelIndex();
        inst.substance = uint32_t(mood_.SeaIndex());  // 0x451030: object +0x50 = water +0x780
        memcpy(inst.centre, in.position, sizeof(inst.centre));
        for (const auto& slot : packages_[it->second].groupOf[inst.model])
            for (const auto& sm : slot)
                for (const Package::Group& g : sm) {
                    (g.fx >= 0 ? groupsDrawn : groupsSkipped)++;
                    if (g.fx < 0) continue;
                    ++perPreset[states_[g.state].base];
                    ++perEffect[g.fx];
                    ++waterGroups;
                }
        waterTiles_.push_back(int(instances_.size()));
        instances_.push_back(inst);
    }
    // The shore waves (0x454bb0): min(lines, 8) slots, each of two strips of
    // up to 6 n + 2 vertices of the longest line, in a buffer object of the
    // manager [0x8ac240] (vtbl 0x78bddc, object vtbl 0x78bd1c) whose material
    // is the water's reference 0 ([0x8ac384] = the water object, vtbl+0x28 =
    // 0x450fd0; Miami 0x41f: type 0x0e, a 512 x 128 DXT3 texture). Drawn as
    // kind 0xa (0x57b490 -> 0x5cfaa0) with render id 1: effect 1. Checked in
    // a trace at boat start point 1: two strips of 60 triangles at vertices 0
    // and 544 (slots 0 and 8 of 68), type 0x0e, mode 2 only, World identity,
    // the 512 texture, no z writes, SRCALPHA / INVSRCALPHA, no fog, and the
    // t_11 constants of effect 1 (s_CamPos in c9, g_fAlpha 0.65098 in c13).
    shoreLines = 0;
    size_t maxN = 0;
    for (const ShoreLine& l : city.water.shore)
        if (l.Count() >= 2) {
            ++shoreLines;
            maxN = std::max(maxN, l.Count());
        }
    shoreSlots_ = std::min<size_t>(city.water.shore.size(), 8);
    shoreMaxVerts_ = maxN * 6 + 2;
    shoreInfo.clear();
    if (shoreLines && shoreSlots_ && !city.water.textures.empty()) {
        shoreGroup_ = GroupFor(city.water.textures[0], 1, false, true, byUid);
        if (shoreGroup_.fx >= 0 &&
            SUCCEEDED(device_->CreateVertexBuffer(UINT(2 * shoreSlots_ * shoreMaxVerts_ * 48), D3DUSAGE_WRITEONLY, 0,
                                                  D3DPOOL_MANAGED, &shoreVb_, nullptr))) {
            shoreCache_.assign(shoreSlots_, ShoreSlot());
            shoreVerts_.assign(2 * shoreSlots_, 0);
            char buf[128];
            snprintf(buf, sizeof(buf), "material 0x%x, effect %d, preset 0x%02x, %zu slots of %zu vertices",
                     unsigned(city.water.textures[0]), shoreGroup_.fx, unsigned(states_[shoreGroup_.state].base),
                     shoreSlots_, shoreMaxVerts_);
            shoreInfo = buf;
        }
    }
    // The far quad (0x450d90, once there are tiles): the manager's create
    // (0x57b180) with 1 strip of 4 vertices and the water's reference 6
    // (0x450fd0: 0xffff0000 past the texture count; Miami 0x424, type 9),
    // drawn as kind 0xa like the shore: render id 1.
    farInfo.clear();
    if (!city.water.tiles.empty()) {
        uint32_t ref = city.water.textures.size() > 6 ? city.water.textures[6] : 0xffff0000u;
        farGroup_ = GroupFor(ref, 1, false, true, byUid);
        if (farGroup_.fx >= 0 &&
            SUCCEEDED(device_->CreateVertexBuffer(4 * 48, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &farVb_, nullptr))) {
            char buf[96];
            snprintf(buf, sizeof(buf), "material 0x%x, effect %d, preset 0x%02x", unsigned(ref), farGroup_.fx,
                     unsigned(states_[farGroup_.state].base));
            farInfo = buf;
        }
    }
    // The tree shadows (0x5613f0, not for a night ride): three buffer
    // objects of the manager with sfx.pmu's materials 0x1f0016, 0x1f0016 and
    // 0x1f0015 (0x800, 0x800, 0x100 vertices), drawn as kind 0xa: render id 1.
    // The device buffer holds the triangle list: 1.5 x the written vertices.
    treeInfo.clear();
    trees_.Reset();
    if (sfx && sfx->uid == 0x1f && timeOfDay_ != 3) {
        int sfxPackage = int(packages_.size());
        AddPackage(*sfx);
        const std::map<uint32_t, int> sfxUid = { { sfx->uid, sfxPackage } };
        static const uint32_t kMaterial[3] = { 0x1f0016, 0x1f0016, 0x1f0015 };
        static const UINT kWritten[3] = { 0x800, 0x800, 0x100 };
        for (int b = 0; b < 3; ++b) {
            treeGroup_[b] = GroupFor(kMaterial[b], 1, false, true, sfxUid);
            if (treeGroup_[b].fx < 0 ||
                FAILED(device_->CreateVertexBuffer(kWritten[b] * 3 / 2 * 48, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED,
                                                   &treeVb_[b], nullptr)))
                treeVb_[b] = nullptr;
        }
        if (treeVb_[0] && treeVb_[2]) {
            char buf[160];
            snprintf(buf, sizeof(buf), "materials 0x1f0016 / 0x1f0015, effect %d / %d, type %d / %d, preset 0x%02x / 0x%02x",
                     treeGroup_[0].fx, treeGroup_[2].fx, int(treeGroup_[0].type), int(treeGroup_[2].type),
                     unsigned(states_[treeGroup_[0].state].base), unsigned(states_[treeGroup_[2].state].base));
            treeInfo = buf;
        }
    }
    // The player's vehicle: its package and the shared vehicle materials
    // (CarGlobals, uid 0x1d), its groups bound as draw kind 3 (0x5e0b20);
    // each part an object of its own with the vehicle's World.
    vehicleParts_.clear();
    vehicleWorlds_.clear();
    vehicleInfo.clear();
    if (vehicle && vehicle->package) {
        std::map<uint32_t, int> carUid;
        if (vehicle->globals) {
            carUid[vehicle->globals->uid] = int(packages_.size());
            AddPackage(*vehicle->globals);
        }
        int car = int(packages_.size());
        AddPackage(*vehicle->package);
        ResolveGroups(packages_.back(), carUid, car, true);
        memcpy(vehicleColour_, vehicle->colour, sizeof(vehicleColour_));
        vehicleDamage_ = vehicle->damage;
        std::map<int, size_t> perFx;
        vehicleWorlds_.reserve(vehicle->parts.size());
        for (const VehicleDraw::Part& part : vehicle->parts) {
            if (part.model < 0 || size_t(part.model) >= vehicle->package->models.size()) continue;
            vehicleWorlds_.emplace_back(part.world, part.world + 16);
            Instance inst;
            inst.package = car;
            inst.model = uint32_t(part.model);
            inst.world = vehicleWorlds_.back().data();
            memcpy(inst.centre, part.world + 12, sizeof(inst.centre));
            for (const auto& sm : packages_[size_t(car)].groupOf[inst.model][0])
                for (const Package::Group& g : sm) ++perFx[g.fx];
            vehicleParts_.push_back(int(instances_.size()));
            instances_.push_back(inst);
        }
        for (const auto& e : perFx) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%seffect %d x%zu", vehicleInfo.empty() ? "" : ", ", e.first, e.second);
            vehicleInfo += buf;
        }
    }
    // Instances no quadtree reaches are not drawn (as in the game).
    std::vector<bool> reached(city.instances.size(), false);
    for (const CityCell& c : city.cells)
        for (uint32_t r : c.index)
            if (r != 0xffffffffu) reached[c.firstInstance + r] = true;
    unindexed = 0;
    for (size_t i = 0; i < reached.size(); ++i)
        if (instanceOf_[i] >= 0 && !reached[i]) ++unindexed;
    stateSets = states_.size();
    presetsUsed.clear();
    for (const auto& e : perPreset) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s0x%02x x%zu", presetsUsed.empty() ? "" : ", ", unsigned(e.first), e.second);
        presetsUsed += buf;
    }
    effectsUsed.clear();
    for (const auto& e : perEffect) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s0x%02x x%zu", effectsUsed.empty() ? "" : ", ", unsigned(e.first), e.second);
        effectsUsed += buf;
    }
    return true;
}

int CityRenderer::StateFor(const uint8_t s[8], int mode)
{
    // The request reads bytes 0..2 sign-extended (0x5e0900).
    const int type = int8_t(s[0]);
    int base = TypePreset(type);
    unsigned extras = ExtraPresets(int8_t(s[1]), int8_t(s[2]));
    if (type >= 2 && type <= 4) mode = 2;  // 0x5d4440: no mode preset for these types
    for (size_t k = 0; k < states_.size(); ++k)
        if (states_[k].base == base && states_[k].extras == extras && states_[k].mode == mode) return int(k);
    // Defaults, the base preset over them (presets below 0x2a first restore
    // the defaults, 0x5d50f0), then the extra presets in 0x5d8340's order,
    // then the mode's preset (0x5cdaa0).
    std::map<DWORD, DWORD> rs, sampler;
    for (const StatePair& p : kDefaultRS) rs[p.state] = p.value;
    for (const StatePair& p : kDefaultSampler) sampler[p.state] = p.value;
    // Filters: MIN / MAG LINEAR, MIP POINT in every traced draw (where the
    // game sets them was not found).
    sampler[D3DSAMP_MINFILTER] = D3DTEXF_LINEAR;
    sampler[D3DSAMP_MAGFILTER] = D3DTEXF_LINEAR;
    sampler[D3DSAMP_MIPFILTER] = D3DTEXF_POINT;
    auto apply = [&](int preset) {
        for (const PresetPair& p : kPresetRS)
            if (p.preset == preset) rs[p.state] = p.value;
        for (const PresetPair& p : kPresetSampler)
            if (p.preset == preset) sampler[p.state] = p.value;
    };
    apply(base);
    static const int kOrder[] = { 0, 1, 3, 2, 4, 5, 6, 7 };  // 0x2a, 0x2b, 0x2d, 0x2c, 0x2e, 0x2f, 0x30, 0x31
    for (int n : kOrder)
        if (extras & (1u << n)) apply(0x2a + n);
    if (int preset = ModePreset(mode)) apply(preset);
    StateSet set;
    set.base = base;
    set.extras = extras;
    set.mode = mode;
    set.rs.assign(rs.begin(), rs.end());
    set.sampler.assign(sampler.begin(), sampler.end());
    set.blend = rs[D3DRS_ALPHABLENDENABLE] != 0;
    states_.push_back(std::move(set));
    return int(states_.size() - 1);
}

void CityRenderer::ApplyState(const StateSet& set)
{
    for (const auto& p : set.rs) {
        if (p.first >= 256 || (rsValid_[p.first] && rsCache_[p.first] == p.second)) continue;
        device_->SetRenderState(D3DRENDERSTATETYPE(p.first), p.second);
        rsCache_[p.first] = p.second;
        rsValid_[p.first] = true;
    }
    for (DWORD stage = 0; stage < 4; ++stage)
        for (const auto& p : set.sampler) {
            if (p.first >= 16 || (samplerValid_[stage][p.first] && samplerCache_[stage][p.first] == p.second)) continue;
            device_->SetSamplerState(stage, D3DSAMPLERSTATETYPE(p.first), p.second);
            samplerCache_[stage][p.first] = p.second;
            samplerValid_[stage][p.first] = true;
        }
}

// The shared light (the effects share these parameters through the pool).
void CityRenderer::SetLight(const float ambient[4], const float colour[4], const float direction[4])
{
    if (ambient_) shared_->SetVector(ambient_, reinterpret_cast<const D3DXVECTOR4*>(ambient));
    if (lightColour_) shared_->SetVector(lightColour_, reinterpret_cast<const D3DXVECTOR4*>(colour));
    if (lightDir_) shared_->SetVector(lightDir_, reinterpret_cast<const D3DXVECTOR4*>(direction));
}

// 0x5e0f20 (per group, from the texture binder): g_envSpecColourAndLerpFactor
// by effect id from the light state (0x8d6c9c + 0x100, copied by 0x5e1130):
// ids 2, 3, 5, 6, 0x24, 0x25 the directional colour (colour) with w 0; 0x1d,
// 0x1f, 0x20, 0x21 the light state's +0x100 with w 0.35 (0.35 alone in modes
// 6 / 7): the mood's CAR_SPECULAR_COLOUR (+0x1d0; the original's car draws
// have ps c0 = (0.62, 0.58, 0.54, 0.35) with Mood33); 0x23 zero; 9 and 0xb
// the light state's +0x140 with w 0: the mood's SHINY_WATER_TINT (the value
// of the original's water draws, ps c2 = (1, 0.9, 0.73, 0) with Mood33;
// where the mood copies it to +0x140 is not traced); others unchanged.
void CityRenderer::SetEnvSpec(const float colour[4], bool redraw)
{
    for (int id = 0; id < 0x30; ++id) {
        Effect& e = fx_[id];
        if (!e.fx || !e.envSpec) continue;
        D3DXVECTOR4 v(0, 0, 0, 0);
        if (id == 2 || id == 3 || id == 5 || id == 6 || id == 0x24 || id == 0x25)
            v = D3DXVECTOR4(colour[0], colour[1], colour[2], 0);
        else if (id == 0x1d || id == 0x1f || id == 0x20 || id == 0x21)
            v = redraw ? D3DXVECTOR4(0, 0, 0, 0.35f)
                       : D3DXVECTOR4(mood_.carSpecular[0], mood_.carSpecular[1], mood_.carSpecular[2], 0.35f);
        else if (id == 9 || id == 0xb)
            v = D3DXVECTOR4(mood_.shinyWaterTint[0], mood_.shinyWaterTint[1], mood_.shinyWaterTint[2], 0);
        else if (id != 0x23)
            continue;
        e.fx->SetVector(e.envSpec, &v);
    }
}

// One group with its effect already begun: World, textures, CommitChanges,
// its render states and the DrawIndexedPrimitive (0x5de4e0, 0x5e0970,
// 0x5d4440, 0x5dfe20). world: row-vector object matrix.
void CityRenderer::DrawGroup(const Package& p, const MdpcModel& m, const MdpcGroup& g, const Package::Group& info,
                             int state, const float world[16], int& applied, float alpha, uint32_t substance)
{
    BindGroup(info, state, world, applied, alpha, substance);
    IDirect3DVertexBuffer9* vb = p.vbs[m.vertexBuffer];
    if (!vb || !p.ib) return;
    SetDecl(DeclIndex(m.vertexType) == 5 ? decl5_ : decl_);
    device_->SetStreamSource(0, vb, 0, p.data->vertexBuffers[m.vertexBuffer].stride);
    device_->SetIndices(p.ib);
    // 0x5dfe20: the group's six DrawIndexedPrimitive values.
    device_->DrawIndexedPrimitive(D3DPRIMITIVETYPE(g.type), g.baseVertex, g.minIndex, g.numVertices, g.startIndex,
                                  g.primitives);
    ++frameDraws;
}

// World, alpha, textures, CommitChanges and the render states of a group
// whose effect is begun.
void CityRenderer::BindGroup(const Package::Group& info, int state, const float world[16], int& applied, float alpha,
                             uint32_t substance)
{
    Effect& e = fx_[info.fx];
    if (e.world) e.fx->SetMatrix(e.world, reinterpret_cast<const D3DXMATRIX*>(world));
    if (e.alpha) e.fx->SetFloat(e.alpha, alpha);  // object +0x64 (0x5de330)
    IDirect3DTexture9* diffuse = info.diffuse;
    IDirect3DTexture9* normal = info.normal;
    if (info.anim >= 0) {
        const Anim& a = anims_[size_t(info.anim)];
        uint32_t n = uint32_t(a.tex0.size());
        // 0x5d92b0: the substance drawn is the object's index (0 when it is
        // not below the count), for an animated material int(time x speed)
        // mod count (fistp with truncation); its first / second texture.
        uint32_t frame = substance < n ? substance : 0;
        if (a.animated) frame = uint32_t(int64_t(time_ * a.speed)) % n;
        diffuse = a.tex0[frame];
        normal = info.normalBound ? a.tex1[frame] : nullptr;
        // 0x5de890 (effects 7 and 9, animated materials only): t = speed x
        // 0.15 x time, frames int(t) and int(t) + 1 mod count, g_TextureLerp
        // = t - int(t); Diffuse1 / 2 = their first texture, NormalMap1 / 2
        // their second (each set only when the substance has it).
        if (e.lerp && a.animated) {
            float t = a.speed * 0.15f * time_;
            uint32_t f = uint32_t(int64_t(t));
            uint32_t k[2] = { f % n, (f + 1) % n };
            e.fx->SetFloat(e.lerp, t - float(f));
            for (int j = 0; j < 2; ++j) {
                if (e.diffuse2[j] && a.tex0[k[j]]) e.fx->SetTexture(e.diffuse2[j], a.tex0[k[j]]);
                if (e.normal2[j] && a.tex1[k[j]]) e.fx->SetTexture(e.normal2[j], a.tex1[k[j]]);
            }
        }
    }
    if (e.diffuse) e.fx->SetTexture(e.diffuse, diffuse ? diffuse : white_);
    if (e.normalMap) e.fx->SetTexture(e.normalMap, normal ? normal : white_);
    if (info.vehicle) {
        if (e.clean) e.fx->SetTexture(e.clean, info.clean ? info.clean : white_);
        if (e.damageTex) e.fx->SetTexture(e.damageTex, info.damage ? info.damage : white_);
        if (e.mask) e.fx->SetTexture(e.mask, info.mask ? info.mask : white_);
        // 0x5defb0: g_DamageFactor = (damage, damage, 0, 0) (object +0x70).
        if (e.damageFactor) {
            const D3DXVECTOR4 d(vehicleDamage_, vehicleDamage_, 0, 0);
            e.fx->SetVector(e.damageFactor, &d);
        }
        // 0x5df200 by the substance's +0x15: 0 (one texture only) (1, 1, 1,
        // 0); 4 (colour, 0); 8 (0.5, 0.5, 0.5, damage); 0x10 (colour,
        // damage), the colour being the object's +0x50; others unchanged.
        if (e.colourWeights) {
            const float* c = vehicleColour_;
            D3DXVECTOR4 w;
            bool set = true;
            switch (info.colourMode) {
            case 0: w = D3DXVECTOR4(1, 1, 1, 0); set = info.textureCount == 1; break;
            case 4: w = D3DXVECTOR4(c[0], c[1], c[2], 0); break;
            case 8: w = D3DXVECTOR4(0.5f, 0.5f, 0.5f, vehicleDamage_); break;
            case 0x10: w = D3DXVECTOR4(c[0], c[1], c[2], vehicleDamage_); break;
            default: set = false; break;
            }
            if (set) e.fx->SetVector(e.colourWeights, &w);
        }
    }
    e.fx->CommitChanges();
    if (state != applied) {
        ApplyState(states_[state]);
        applied = state;
    }
}

// 0x454820 (line record, vertex out, a, b, alpha): one shore line as a
// triangle strip of 6 n + 2 vertices. Per point i the ends A (x1, y, z1)
// and B (x2, y, z2), uv (a + u, fmod(b x 0.05, 1) + v) and + 1 in v for B,
// colour SHORE_WAVE_COLOUR with alpha 0; between them (0x454660, the
// position and uv lerped by t^2 = 1/9 and 4/9) the rows L1 / L2 with alpha
// SHORE_WAVE_INTENSITY x alpha x 0.9921, or 0 on the first and last point.
// The strip: A / L1 from the last point to the first, a repeat, L1 / L2
// from the first to the last, a repeat, L2 / B from the last to the first.
void CityRenderer::BuildShoreStrip(const ShoreLine& line, char* out, float a, float b, float alpha) const
{
    struct Vertex { float pos[3], normal[3], uv[2], colour[4]; };
    static_assert(sizeof(Vertex) == 48, "shore vertex");
    Vertex* v = reinterpret_cast<Vertex*>(out);
    const size_t n = line.Count();
    const float y = city_->water.height;  // [0x8ac384] = the water object, vtbl+0x24 = 0x450fc0 (traced: -2.5)
    const float voff = std::fmod(b * 0.05f, 1.0f);
    const float inner = mood_.shoreWave[3] * alpha * 0.9921f;
    auto make = [&](const float* p, int end) {
        Vertex r = {};
        r.pos[0] = p[end * 2];
        r.pos[1] = y;
        r.pos[2] = p[end * 2 + 1];
        r.normal[1] = 1.0f;
        r.uv[0] = a + p[4];
        r.uv[1] = voff + p[5] + (end ? 1.0f : 0.0f);
        memcpy(r.colour, mood_.shoreWave, 3 * sizeof(float));
        r.colour[3] = 0.0f;
        return r;
    };
    auto lerp = [](const Vertex& p, const Vertex& q, float t, float al) {
        Vertex r = p;
        for (int k = 0; k < 3; ++k) r.pos[k] = (q.pos[k] - p.pos[k]) * t + p.pos[k];
        for (int k = 0; k < 2; ++k) r.uv[k] = (q.uv[k] - p.uv[k]) * t + p.uv[k];
        r.colour[3] = al;
        return r;
    };
    for (size_t i = 0; i < n; ++i) {
        const float* p = &line.points[i * 6];
        Vertex A = make(p, 0), B = make(p, 1);
        float al = (i == 0 || i + 1 == n) ? 0.0f : inner;
        Vertex L1 = lerp(A, B, 1.0f / 9.0f, al), L2 = lerp(A, B, 4.0f / 9.0f, al);
        v[2 * n - 2 - 2 * i] = A;
        v[2 * n - 1 - 2 * i] = L1;
        v[2 * n + 1 + 2 * i] = L1;
        v[2 * n + 2 + 2 * i] = L2;
        v[6 * n - 2 * i] = L2;
        v[6 * n + 1 - 2 * i] = B;
    }
    v[2 * n] = v[2 * n - 1];
    v[4 * n + 1] = v[4 * n];
}

// 0x454cf0: the lines whose centre (at the water height) is within 300
// units of the camera after subtracting their radius (0x78edd0; the game
// also drops lines whose box is outside the view, 0x57a410 / 0x579fa0 /
// 0x57a150, not reproduced), the nearest min(lines, 8) of them into the
// slot cache (a slot that gets another line is rebuilt). 0x455840: a slot is
// rebuilt when its last build is older than 0.09 / 0.06 / 0.03 / 0 s for a
// distance above 150 / 75 / 25 / below, with alpha 1, fading to 0 from 275
// to 300 (x 0.04); strip k = (a 0, b time), strip k + slots = (a fmod(time
// x 0.0005 + 0.333, 1), b time x 0.78578). The game's clock here is the
// update counter / 120 ([0x8ac434] +4 x 0x78e8d4).
void CityRenderer::UpdateShore(const float eye[3])
{
    shoreDrawn_ = 0;
    if (!shoreVb_ || !city_) return;
    const CityWater& w = city_->water;
    struct Cand { int line; float dist; };
    std::vector<Cand> cand;
    for (size_t i = 0; i < w.shore.size(); ++i) {
        const ShoreLine& l = w.shore[i];
        if (l.Count() < 2) continue;  // 0x4546f0 keeps nothing for them
        float d[3] = { eye[0] - l.centre[0], eye[1] - w.height, eye[2] - l.centre[1] };
        float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - l.radius;
        if (dist <= 300.0f) cand.push_back({ int(i), dist });
    }
    shoreDrawn_ = std::min(cand.size(), shoreSlots_);
    for (size_t k = 0; k < shoreDrawn_; ++k) {
        size_t best = 0;
        for (size_t j = 1; j < cand.size(); ++j)
            if (cand[best].dist > cand[j].dist) best = j;
        ShoreSlot& s = shoreCache_[k];
        if (s.line != cand[best].line) {
            s.line = cand[best].line;
            s.valid = false;  // last = -FLT_MAX
        }
        s.dist = cand[best].dist;
        cand[best].dist = 3.402823466e38f;
    }
    for (size_t k = 0; k < shoreDrawn_; ++k) {
        ShoreSlot& s = shoreCache_[k];
        float interval = s.dist > 150.0f ? 0.09f : s.dist > 75.0f ? 0.06f : s.dist > 25.0f ? 0.03f : 0.0f;
        if (s.valid && !(s.last < time_ - interval)) continue;
        float alpha = s.dist > 275.0f ? 1.0f - (s.dist - 275.0f) * 0.04f : 1.0f;
        const ShoreLine& l = w.shore[size_t(s.line)];
        UINT verts = UINT(l.Count() * 6 + 2), bytes = UINT(shoreMaxVerts_ * 48);
        for (int strip = 0; strip < 2; ++strip) {
            size_t slot = k + size_t(strip) * shoreSlots_;
            void* ptr = nullptr;
            if (FAILED(shoreVb_->Lock(UINT(slot) * bytes, bytes, &ptr, 0))) continue;
            if (strip == 0) BuildShoreStrip(l, static_cast<char*>(ptr), 0.0f, time_, alpha);
            else BuildShoreStrip(l, static_cast<char*>(ptr), std::fmod(time_ * 0.0005f + 0.333f, 1.0f), time_ * 0.78578f, alpha);
            shoreVb_->Unlock();
            shoreVerts_[slot] = verts;
        }
        s.last = time_;
        s.valid = true;
    }
}

// 0x454cf0 draws per chosen slot k strip k and strip k + slots through the
// buffer object (vtbl+0x10) with an identity matrix (the vertices are in
// world space). g_fAlpha is the traced 0.65098 (the value of g_fBrightness;
// where kind 0xa gets it was not followed). Effect 1 does not read the
// vertex colour, so its alpha (0 at the edges) does not show.
void CityRenderer::DrawShore(bool redraw, int& applied)
{
    const Package::Group& info = shoreGroup_;
    if (!shoreVb_ || !shoreDrawn_ || info.fx < 0) return;    int state = redraw ? info.redraw : info.state;
    if (state < 0 || (!redraw && !DrawnInMode(info.type, 2))) return;
    Effect& e = fx_[info.fx];
    UINT passes = 0;
    if (FAILED(e.fx->Begin(&passes, D3DXFX_DONOTSAVESTATE)) || FAILED(e.fx->BeginPass(0))) return;
    static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    uint32_t bits = 0x3f26a6a7;
    float alpha;
    memcpy(&alpha, &bits, 4);
    BindGroup(info, state, kIdentity, applied, alpha, 0);
    device_->SetStreamSource(0, shoreVb_, 0, 48);
    for (size_t k = 0; k < shoreDrawn_; ++k)
        for (int strip = 0; strip < 2; ++strip) {
            size_t slot = k + size_t(strip) * shoreSlots_;
            if (shoreVerts_[slot] < 3) continue;
            device_->DrawPrimitive(D3DPT_TRIANGLESTRIP, UINT(slot * shoreMaxVerts_), shoreVerts_[slot] - 2);
            ++frameDraws;
        }
    e.fx->EndPass();
    e.fx->End();
    if (!redraw) frameShore = shoreDrawn_;
}

// 0x451030 (camera matrix, SEA_COLOUR index, water height), part 1: the
// camera's row 2 (back) flattened (y = 0) and normalized (0x417ea0) is f;
// 0x41ea80 gives the World rows f x up = (-f.z, 0, f.x), (0, 1, 0), f and
// the position eye - 4000 f with y = 0 (0x78f048 = -4000). In it the strip
// (-4800, top), (4800, top), (-4800, bottom), (4800, bottom), z 0, normal
// (0, 1, 0), uv (0, 0), (1, 0), (0, 1), (1, 1), colour (1, 1, 1, 0) where
// declaration 1 has the tangent; top = eye.y x (1/6750) x 2750 + 2
// (0x78f044, 0x78f040, 0x78e8bc), bottom = height - 2: a wall from below
// the sea up to the camera's horizon. Type 9 -> preset 0x1e: colour writes
// off, so it only writes depth (at 4000 units the sea behind it is less
// than a pixel high). Drawn through the buffer object's vtbl+0x10 (0x57b490:
// kind 0xa) before the shore; its lock (vtbl+8, 0x57b430) is the static
// 0x7ec220, copied to the device buffer at the unlock (0x57b440). Checked
// in the traces (boat, start point 3): type 9, mode 2 only, a 2-triangle
// strip, World (-1, 0, 0), (0, 1, 0), (0, 0, 1), (cam.x, 0, cam.z - 4000)
// for a camera looking along -z, ZENABLE 1, ZFUNC 4, ZWRITE 1,
// COLORWRITEENABLE 0, SRCALPHA / INVSRCALPHA, fog off, alpha test, CULL 2,
// g_fAlpha 0.65098; the vertices as built here (top 1.58915 for s_CamPos.y
// -1.00845, bottom -4.5).
void CityRenderer::DrawFar(const float eye[3], const float back[3], int& applied)
{
    frameFar = false;
    const Package::Group& info = farGroup_;
    if (!farVb_ || !city_ || info.fx < 0 || info.state < 0 || !DrawnInMode(info.type, 2)) return;    float f[3] = { back[0], 0.0f, back[2] };
    float len = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    if (len <= 0.0f) return;
    f[0] /= len;
    f[2] /= len;
    const float object[16] = {
        -f[2], 0, f[0], 0,
        0, 1, 0, 0,
        f[0], 0, f[2], 0,
        f[0] * -4000.0f + eye[0], 0, f[2] * -4000.0f + eye[2], 1,
    };
    float world[16];
    Transpose(object, world);  // as for every object (0x5de4e0)
    uint32_t bits = 0x391b5837;  // 0x78f044
    float k;
    memcpy(&k, &bits, 4);
    const float top = eye[1] * k * 2750.0f + 2.0f, bottom = city_->water.height - 2.0f;
    struct Vertex { float pos[3], normal[3], uv[2], colour[4]; };
    static_assert(sizeof(Vertex) == 48, "far vertex");
    const Vertex quad[4] = {
        { { -4800.0f, top, 0 }, { 0, 1, 0 }, { 0, 0 }, { 1, 1, 1, 0 } },
        { { 4800.0f, top, 0 }, { 0, 1, 0 }, { 1, 0 }, { 1, 1, 1, 0 } },
        { { -4800.0f, bottom, 0 }, { 0, 1, 0 }, { 0, 1 }, { 1, 1, 1, 0 } },
        { { 4800.0f, bottom, 0 }, { 0, 1, 0 }, { 1, 1 }, { 1, 1, 1, 0 } },
    };
    void* ptr = nullptr;
    if (FAILED(farVb_->Lock(0, sizeof(quad), &ptr, 0))) return;
    memcpy(ptr, quad, sizeof(quad));
    farVb_->Unlock();
    Effect& e = fx_[info.fx];
    UINT passes = 0;
    if (FAILED(e.fx->Begin(&passes, D3DXFX_DONOTSAVESTATE)) || FAILED(e.fx->BeginPass(0))) return;
    uint32_t alphaBits = 0x3f26a6a7;  // g_fAlpha as traced (see DrawShore)
    float alpha;
    memcpy(&alpha, &alphaBits, 4);
    BindGroup(info, info.state, world, applied, alpha, 0);
    device_->SetStreamSource(0, farVb_, 0, 48);
    device_->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    ++frameDraws;
    e.fx->EndPass();
    e.fx->End();
    frameFar = true;
}

// 0x5616f0 with the camera's x / z as the point (the game passes the
// player's); the ground probe is 0x4e9da0 over the city's GRTD chunks.
void CityRenderer::UpdateTreeShadows(const float eye[3])
{
    if (!treeVb_[0] || !city_) return;
    const float point[2] = { eye[0], eye[2] };
    const CityGround& ground = city_->ground;
    auto probe = [&](float x, float y, float z, float& h, float n[3]) { return ground.Probe(x, y, z, h, n); };
    if (!trees_.Update(*city_, point, mood_.sun, probe)) return;
    for (int b = 0; b < 3; ++b) {
        if (!treeVb_[b] || trees_.list[b].empty()) continue;
        void* ptr = nullptr;
        UINT bytes = UINT(trees_.list[b].size() * sizeof(TreeShadowVertex));
        if (SUCCEEDED(treeVb_[b]->Lock(0, bytes, &ptr, 0))) {
            memcpy(ptr, trees_.list[b].data(), bytes);
            treeVb_[b]->Unlock();
        }
    }
    char buf[192];
    snprintf(buf, sizeof(buf),
             "at (%.1f, %.1f): %d trees found, %d without ground; buffers %d / %d / %d trees, %d / %d / %d triangles "
             "drawn",
             point[0], point[1], trees_.found, trees_.noGround, trees_.trees[0], trees_.trees[1], trees_.trees[2],
             trees_.triangles[0], trees_.triangles[1], trees_.triangles[2]);
    treeBuild = buf;
}

// The shadow pass's two screen quads (traced at the start of mode 7, from the
// buffer object path 0x5dfe4d, owner not decoded): six vertices each of
// 24 bytes, clip-space corners (+-1, +-1, 0), a D3DCOLOR and uv; type 2
// (preset 0x20: z ALWAYS, colour writes alpha only, no stencil test) with
// colour 0x00ffffff, then type 3 (0x1f: the same where stencil >= 1) with
// 0xffffffff. So the destination alpha is 0, and 1 in the volumes' shadow;
// the tree shadows add to it and the mode 7 redraw blends the shadowed
// colour by it (DESTALPHA / INVDESTALPHA). Drawn here without an effect
// (pre-transformed, alpha from the vertex colour: the same values written).
void CityRenderer::DrawShadowAlpha(int& applied)
{
    D3DVIEWPORT9 vp = {};
    device_->GetViewport(&vp);
    struct Q { float x, y, z, rhw; D3DCOLOR c; };
    const float x0 = float(vp.X) - 0.5f, y0 = float(vp.Y) - 0.5f, x1 = x0 + float(vp.Width), y1 = y0 + float(vp.Height);
    device_->SetVertexShader(nullptr);
    device_->SetPixelShader(nullptr);
    device_->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device_->SetTexture(0, nullptr);
    device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    for (int k = 0; k < 2; ++k) {
        if (alphaState_[k] < 0) continue;
        ApplyState(states_[alphaState_[k]]);
        applied = alphaState_[k];
        // Cull mode 1 (none) in the trace; the preset leaves the default.
        device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        rsValid_[D3DRS_CULLMODE] = false;
        D3DCOLOR c = k == 0 ? 0x00ffffff : 0xffffffff;
        const Q quad[4] = { { x0, y0, 0, 1, c }, { x1, y0, 0, 1, c }, { x0, y1, 0, 1, c }, { x1, y1, 0, 1, c } };
        device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Q));
        ++frameDraws;
    }
    device_->SetVertexDeclaration(decl_);
    currentDecl_ = decl_;
}

// 0x5616c0: each buffer object with vertices draws its list (D3DPT_TRIANGLELIST,
// World identity) with its group's states (type 4 -> preset 0x27: stencil
// REPLACE 1, colour writes alpha only, SRCALPHA / ONE). The game draws them
// as mode 7 requests, before the list 0 redraw (the trace's order).
void CityRenderer::DrawTreeShadows(int& applied)
{
    frameTreeTriangles = 0;
    static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    for (int b = 0; b < 3; ++b) {
        const Package::Group& info = treeGroup_[b];
        int n = trees_.triangles[b];
        if (!treeVb_[b] || n <= 0 || info.fx < 0 || !DrawnInMode(info.type, 7)) continue;
        n = std::min(n, int(trees_.list[b].size() / 3));
        Effect& e = fx_[info.fx];
        UINT passes = 0;
        if (FAILED(e.fx->Begin(&passes, D3DXFX_DONOTSAVESTATE)) || FAILED(e.fx->BeginPass(0))) continue;
        BindGroup(info, info.state, kIdentity, applied, 1.0f, 0);
        device_->SetStreamSource(0, treeVb_[b], 0, sizeof(TreeShadowVertex));
        device_->DrawPrimitive(D3DPT_TRIANGLELIST, 0, UINT(n));
        ++frameDraws;
        frameTreeTriangles += n;
        e.fx->EndPass();
        e.fx->End();
    }
}

// 0x5cdb20: a cube texture from a material's six substances: edge and
// format of the first one's texture, one level, managed; face i (+x, -x,
// +y, -y, +z, -z) = substance i's first texture, copied by
// D3DXLoadSurfaceFromSurface with D3DX_FILTER_NONE.
IDirect3DCubeTexture9* CityRenderer::CubeFrom(const Package& p, size_t material)
{
    typedef HRESULT(WINAPI * LoadSurfaceFn)(LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*, LPDIRECT3DSURFACE9,
                                            const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);
    auto load = dll_ ? reinterpret_cast<LoadSurfaceFn>(GetProcAddress(dll_, "D3DXLoadSurfaceFromSurface")) : nullptr;
    if (!load || material >= p.data->materials.size()) return nullptr;
    const std::vector<MdpcSubstance>& subs = p.data->materials[material];
    IDirect3DTexture9* faces[6] = {};
    for (size_t i = 0; i < 6; ++i) {
        if (i >= subs.size() || subs[i].textures.empty() || size_t(subs[i].textures[0]) >= p.textures.size()) return nullptr;
        faces[i] = p.textures[subs[i].textures[0]];
        if (!faces[i]) return nullptr;
    }
    D3DSURFACE_DESC desc;
    IDirect3DCubeTexture9* cube = nullptr;
    if (FAILED(faces[0]->GetLevelDesc(0, &desc)) ||
        FAILED(device_->CreateCubeTexture(desc.Width, 1, 0, desc.Format, D3DPOOL_MANAGED, &cube, nullptr)))
        return nullptr;
    for (int i = 0; i < 6; ++i) {
        IDirect3DSurface9 *src = nullptr, *dst = nullptr;
        if (SUCCEEDED(faces[i]->GetSurfaceLevel(0, &src)) &&
            SUCCEEDED(cube->GetCubeMapSurface(D3DCUBEMAP_FACES(i), 0, &dst)))
            load(dst, nullptr, nullptr, src, nullptr, nullptr, D3DX_FILTER_NONE, 0);
        if (src) src->Release();
        if (dst) dst->Release();
    }
    return cube;
}

// 0x539cb0: the sky dome is an object with the mood's sky matrix (+0x10),
// row 3 = the camera position with y = -5 (0xc0a00000), model 0xd000
// (package uid 0x34, model 0) and the colour SKY_COLOUR (+0x160). Drawn
// as kind 0x10001, 0x5de330 gives it s_LightAmbientColour = that colour,
// s_LightDirectionalDirection (0, -1, 0, 0) and s_LightDirectionalColour
// (0, 0, 0, 1) (the values the trace shows for its draw).
void CityRenderer::DrawSky(const float eye[3])
{
    if (skyPackage_ < 0) return;
    const Package& p = packages_[skyPackage_];
    const MdpcModel& m = p.data->models[0];
    static const float kDown[4] = { 0, -1, 0, 0 }, kBlack[4] = { 0, 0, 0, 1 };
    SetLight(mood_.skyColour, kBlack, kDown);
    float object[16];
    memcpy(object, mood_.skyMatrix, sizeof(object));
    object[12] = eye[0];
    object[13] = -5.0f;
    object[14] = eye[2];
    object[15] = 1.0f;
    float wt[16], world[16];
    Transpose(object, wt);
    int applied = -1;
    for (size_t s = 0; s < m.lods[0].size() && s < p.groupOf[0][0].size(); ++s) {
        const MdpcSubmesh& sm = m.lods[0][s];
        if (sm.hasMatrix) Mul(wt, sm.matrix, world);
        else memcpy(world, wt, sizeof(world));
        for (size_t g = 0; g < sm.groups.size(); ++g) {
            const Package::Group& info = p.groupOf[0][0][s][g];
            if (info.fx < 0) continue;
            Effect& e = fx_[info.fx];
            UINT passes = 0;
            if (FAILED(e.fx->Begin(&passes, D3DXFX_DONOTSAVESTATE)) || FAILED(e.fx->BeginPass(0))) continue;
            DrawGroup(p, m, sm.groups[g], info, info.state, world, applied);
            e.fx->EndPass();
            e.fx->End();
        }
    }
}

// 0x4ddc50 (normal cells, mode 1) and 0x4dde20 (the GESR, mode 3): the
// slot's +0x40.. distances per quadtree level by the profile's draw
// distance (+0xc88), and at +0x80.. six detail entries (mask, ratio limit,
// fade width). The masks carry u = 0x20, or 0x40 when the city is a night
// one (setup vtbl+0x60 == 2; 0x464230: vtbl+0x28 = 2, 4 or 6). Slot masks
// of the models: 0x1e / 0x1f, 0x1c, 0x18, 0x10 for slots 0..3 (each adds
// detail), 0x01 for slot 4 (the far stand-in), 0x20 the shadow volume.
// (0x4ddf00, mode 2, is for the cell at [0x8ac44c]+0xcbc, set by the
// handler 0x45aef0; none in a ride.)
void CityRenderer::SetDrawDistance(int setting, bool night)
{
    uint32_t u = night ? 0x40 : 0x20;
    LodTable& t = tables_[0];
    t = LodTable();
    static const float kDist[3][16] = {
        { 500, 62.5f, 62.5f, 62.5f, 50, 37.5f, 25, 21.25f, 21.25f, 21.25f, 21.25f, 21.25f, 12.5f, 12.5f, 12.5f, 12.5f },
        { 2000, 250, 250, 250, 200, 150, 100, 85, 85, 85, 85, 85, 50, 50, 50, 50 },
        { 4000, 500, 500, 500, 400, 300, 200, 170, 170, 170, 170, 170, 170, 170, 170, 170 },
    };
    if (setting >= 0 && setting <= 2) memcpy(t.dist, kDist[setting], sizeof(t.dist));
    const uint32_t mask[6] = { u | 0x10, u | 8, u | 4, u | 2, 1, 0 };
    const float limit[6] = { 0.12f, 0.18f, 0.36f, 0.65f, 1.2f, 1.5f };
    const float width[6] = { 0.04f, 0.06f, 0.12f, 0.08f, 0.08f, 0 };
    memcpy(t.mask, mask, sizeof(mask));
    memcpy(t.limit, limit, sizeof(limit));
    memcpy(t.width, width, sizeof(width));
    LodTable& g = tables_[1];
    g = LodTable();
    const uint32_t gmask[6] = { 0, 0, 0, u | 1, 1, 0 };
    const float glimit[6] = { 0, 0, 0.75f, 1.5f, 1e6f, 1e6f };
    const float gwidth[6] = { 0, 0, 0.15f, 0, 0, 0 };
    memcpy(g.mask, gmask, sizeof(gmask));
    memcpy(g.limit, glimit, sizeof(glimit));
    memcpy(g.width, gwidth, sizeof(gwidth));
}

// The camera for one frame (0x4e7e30): position, the horizontal backward
// axis f (camera matrix row 2 with y = 0, normalized; 0x41b630), the two
// side planes normalize(+-r - k f) (r = f x up, 0x407640; k = tan(fov / 2)
// x sqrt(1 + 1 / aspect^2) / |f|, the half angle of the view's corners),
// the distance factor [0x8b3998] = 1.04 / fov and the shadow vector
// (mood +0x190, y = 0: [0x8b399c]).
struct CityRenderer::View
{
    float eye[3], f[3], p1[3], p2[3], shadow[3];
    float distScale, lodScale;
    bool sides;  // false when the view is nearly vertical (|f| <= 0.05)
};

// 0x4dff20: the detail entry of a ratio; within the fade width below its
// limit the next entry's mask is added with 0x80 and the alpha falls with
// the ratio (entries 3 / 4 add 0x200). Mode 3 entry 2: alpha above 0.5
// pushes the stand-in 2.5 units away and fades it in (2 - 2 alpha), else
// it is opaque and pushed by alpha x 5. The push is not reset otherwise
// (the caller's value carries over).
void CityRenderer::PickLod(const LodTable& t, int mode, float ratio, Lod& lod) const
{
    int i = 0;
    while (i < 5 && !(ratio < t.limit[i])) ++i;
    lod.mask = t.mask[i];
    lod.alpha = 1.0f;
    if (i < 5 && t.limit[i] - t.width[i] < ratio) {
        lod.mask |= t.mask[i + 1] | 0x80;
        lod.alpha = (t.limit[i] - ratio) / t.width[i];
        if (i == 3 || i == 4) lod.mask |= 0x200;
        if (mode == 3 && i == 2) {
            if (lod.alpha > 0.5f) {
                lod.offset = 2.5f;
                lod.alpha = 2.0f - (lod.alpha + lod.alpha);
                return;
            }
            lod.offset = lod.alpha * 5.0f;
            lod.mask &= ~0x80u;
            lod.alpha = 1.0f;
        }
    }
}

// 0x4dfe10: pos is the node centre minus the camera (y = 0), radius its
// size x 1.2071 (or x 1 for the shadow retest), dist its level's distance
// x [0x8b3998]. Mode 3: |pos| / radius. Otherwise 0 when the camera is
// within the radius, -1 when the centre is behind the camera or the node
// is beyond a side plane, -1 beyond 1.2 x (limit +0xb4) the distance x
// [0x7a1a68], else (|pos| - radius) / (distance x [0x7a1a68]).
float CityRenderer::NodeRatio(const View& v, const LodTable& t, int mode, const float pos[3], float radius,
                              float dist) const
{
    float d2 = pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2];
    if (mode == 3) return std::sqrt(d2) / radius;
    if (radius * radius > d2) return 0.0f;
    if (-(v.f[0] * pos[0] + v.f[1] * pos[1] + v.f[2] * pos[2]) < 0.0f) return -1.0f;
    if (v.sides) {
        if (v.p1[0] * pos[0] + v.p1[1] * pos[1] + v.p1[2] * pos[2] < -radius) return -1.0f;
        if (v.p2[0] * pos[0] + v.p2[1] * pos[1] + v.p2[2] * pos[2] < -radius) return -1.0f;
    }
    float e = std::sqrt(d2) - radius, s = v.lodScale * dist;
    if (!(e <= s * t.limit[4])) return -1.0f;
    return e / s;
}

// 0x4dfce0: mode 3 nodes wider than 480 get mask 2 with alpha 0 (drawn as
// nothing); otherwise the node test with radius size x 1.2071 picks the
// detail; failing that, the same node moved by the shadow vector with
// radius size passes with mask 0x20 (only its shadow volumes).
bool CityRenderer::NodeVisible(const View& v, const CityCell& cell, int mode, const CityNode& n, float size,
                               Lod& lod) const
{
    const LodTable& t = tables_[mode == 3 ? 1 : 0];
    if (mode == 3 && size > 480.0f) {
        lod.mask = 2;
        lod.alpha = 0.0f;
        return true;
    }
    float pos[3] = { (float(n.x) + 0.5f) * size + cell.origin[0] - v.eye[0], 0,
                     (float(n.z) + 0.5f) * size + cell.origin[1] - v.eye[2] };
    float dist = v.distScale * t.dist[n.level < 16 ? n.level : 15];
    float r = NodeRatio(v, t, mode, pos, size * 1.2071f, dist);
    if (r >= -1e-5f) {
        PickLod(t, mode, r, lod);
        return true;
    }
    for (int k = 0; k < 3; ++k) pos[k] += v.shadow[k];
    if (NodeRatio(v, t, mode, pos, size, dist) >= -1e-5f) {
        lod.mask = 0x20;
        lod.alpha = 0.0f;
        return true;
    }
    return false;
}

// 0x4dec70: a visible node submits its instances (their three bit masks at
// slot +0x18 / +0x20 / +0x28 are cleared at load, 0x4ddae0) with the
// node's detail; in mode 3 nodes narrower than 480 pick each instance's
// detail again from |instance - camera| / node size. Then its children.
void CityRenderer::WalkNode(const View& v, const CityCell& cell, int mode, const CityNode& n,
                            std::vector<Submit>& out) const
{
    float size = cell.size / float(1u << (n.level & 31));
    Lod lod;
    lod.alpha = 1.0f;
    if (!NodeVisible(v, cell, mode, n, size, lod)) return;
    for (uint32_t k = n.first; k < uint32_t(n.first) + n.count; ++k) {
        uint32_t r = cell.index[k];
        if (r == 0xffffffffu) continue;
        int idx = instanceOf_[cell.firstInstance + r];
        if (idx < 0) continue;
        if (mode == 3 && size < 480.0f) {
            const float* p = instances_[size_t(idx)].in->position;
            float d[3] = { p[0] - v.eye[0], p[1] - v.eye[1], p[2] - v.eye[2] };
            PickLod(tables_[1], mode, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / size, lod);
        }
        out.push_back({ idx, lod });
    }
    for (uint16_t c : n.child)
        if (c) WalkNode(v, cell, mode, cell.nodes[c], out);
}

void CityRenderer::Draw(const float viewProj[16], const float eye[3], const float back[3], float fov, float aspect)
{
    frameInstances = frameDraws = 0;
    if (!device_ || !shared_) return;
    float t[16];
    Transpose(viewProj, t);  // 0x5dc4b0
    shared_->SetMatrix(viewProj_, reinterpret_cast<const D3DXMATRIX*>(t));
    const D3DXVECTOR4 cam(eye[0], eye[1], eye[2], 1.0f);
    if (camPos_) shared_->SetVector(camPos_, &cam);
    device_->SetVertexDeclaration(decl_);
    currentDecl_ = decl_;
    // 0x5dc35a: s_TextureEnvironment / s_TextureShine = the cubes when there
    // are (else the texture the caller passes; here the white one).
    if (envTex_) shared_->SetTexture(envTex_, cubes_[1] ? static_cast<IDirect3DBaseTexture9*>(cubes_[1]) : white_);
    if (shineTex_) shared_->SetTexture(shineTex_, cubes_[0] ? static_cast<IDirect3DBaseTexture9*>(cubes_[0]) : white_);
    DrawSky(eye);
    // The mood (0x5dc1e0): s_FogParams and FOGCOLOR (0x5cefb0), the light
    // (0x5dc3f0): ambient (w 1), directional colour (w 1) and direction
    // normalized with w 0 (0x417ea0).
    float fog[4], dir[4] = { mood_.direction[0], mood_.direction[1], mood_.direction[2], 0 };
    mood_.FogParams(fog);
    if (fogParams_) shared_->SetVector(fogParams_, reinterpret_cast<const D3DXVECTOR4*>(fog));
    float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len > 0)
        for (float& v : dir) v /= len;
    SetLight(mood_.ambient, mood_.directional, dir);
    device_->SetRenderState(D3DRS_FOGCOLOR, mood_.FogColour());
    SetEnvSpec(mood_.directional);
    // 0x5dc566: s_WorldToEnvMap = transpose(D3DXMatrixLookAtRH(eye = -light
    // direction, at = 0, up = (0, 1, 0))) (the t_11 shaders' c8..c10).
    if (worldToEnv_) {
        float z[3] = { -dir[0], -dir[1], -dir[2] };  // normalize(eye - at)
        float x[3] = { z[2], 0, -z[0] };               // up x z
        float n = std::sqrt(x[0] * x[0] + x[2] * x[2]);
        if (n > 0)
            for (float& c : x) c /= n;
        float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
        auto dot = [&](const float* a) { return a[0] * z[0] + a[1] * z[1] + a[2] * z[2]; };  // . eye (= z)
        const float m[16] = { x[0], y[0], z[0], 0,  x[1], y[1], z[1], 0,  x[2], y[2], z[2], 0,
                              -dot(x), -dot(y), -dot(z), 1 };
        float mt[16];
        Transpose(m, mt);
        shared_->SetMatrix(worldToEnv_, reinterpret_cast<const D3DXMATRIX*>(mt));
    }

    frameTreeTriangles = 0;
    UpdateTreeShadows(eye);
    // The cells (0x4e7e30): every cell's quadtree from its root, the GESR
    // in mode 3, the others in mode 1.
    View v;
    memcpy(v.eye, eye, sizeof(v.eye));
    float fh = std::sqrt(back[0] * back[0] + back[2] * back[2]);
    v.sides = fh > 0.05f;
    v.f[0] = v.sides ? back[0] / fh : 0;
    v.f[1] = 0;
    v.f[2] = v.sides ? back[2] / fh : 0;
    float k = std::tan(fov * 0.5f) * std::sqrt(1.0f / (aspect * aspect) + 1.0f) / (fh > 0 ? fh : 1);
    float r[3] = { -v.f[2], 0, v.f[0] };  // f x up, in the ground plane
    for (int s = 0; s < 2; ++s) {
        float* pl = s ? v.p2 : v.p1;
        float sign = s ? 1.0f : -1.0f;
        float q[3] = { sign * r[0] - k * v.f[0], 0, sign * r[2] - k * v.f[2] };
        float n = std::sqrt(q[0] * q[0] + q[2] * q[2]);
        for (int j = 0; j < 3; ++j) pl[j] = n > 0 ? q[j] / n : 0;
    }
    v.distScale = 1.04f / fov;
    v.lodScale = lodScale;
    v.shadow[0] = mood_.shadow[0];
    v.shadow[1] = 0;
    v.shadow[2] = mood_.shadow[2];
    std::vector<Submit> submitted;
    if (city_)
        for (const CityCell& cell : city_->cells)
            if (!cell.nodes.empty()) WalkNode(v, cell, cell.column == -2 && cell.row == -2 ? 3 : 1, cell.nodes[0], submitted);
    // The sea (0x451030): every tile whose point is within 6000 units of the
    // camera (squared distance below 36e6, 0x78f03c), as an object with an
    // identity rotation, +0x60 = 0x5f and alpha 1 through 0x57de30 (its
    // cull 0x57dbd0 is not reproduced).
    frameWater = 0;
    size_t cityObjects = submitted.size();  // [0x8b3918] counts only these (0x4ddb60)
    for (int idx : waterTiles_) {
        const float* p = instances_[size_t(idx)].in->position;
        float d[3] = { p[0] - eye[0], p[1] - eye[1], p[2] - eye[2] };
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] >= 36e6f) continue;
        Lod lod;
        lod.mask = 0x5f;
        submitted.push_back({ idx, lod });
        ++frameWater;
    }
    // The player's vehicle: every part with mask 0x22 (slot 0, whose mask
    // 0x1e has bit 2, and the shadow volume; the vehicle's own detail choice
    // was not traced: the traced start frames draw slot 0).
    for (int idx : vehicleParts_) {
        Lod lod;
        lod.mask = 0x22;
        submitted.push_back({ idx, lod });
    }

    // The scene's lists (0x5d2ca0): 0 = items, 1 = volumes, 2 = late. Each
    // submitted object draws the slots 0..4 whose mask shares a bit with
    // its mask (0x5dcc50); with an alpha other than 1 the slot whose lowest
    // bit is the object's highest one fades (alpha 0: left out) and any
    // other slot sets the object's alpha back to 1, so the alpha used is
    // the one left at the end (the draw reads +0x64 later). Slots 5 / 6
    // (mask 0x20) are not drawn here: their groups have no material
    // (0x5ca020); the shadow volume is the object's copy (0x5d2b90).
    struct Object { const Instance* inst; float world[16]; float alpha; };
    struct Item { int fx; size_t object; const MdpcSubmesh* sm; const MdpcGroup* g; const Package::Group* info; };
    std::vector<Object> objects;
    objects.reserve(submitted.size());
    std::vector<Item> items, late;
    std::vector<size_t> volumes;
    frameShadowDraws = frameRedraws = frameShadowOnly = 0;
    for (size_t& c : frameSlots) c = 0;
    for (const Submit& s : submitted) {
        const Instance& inst = instances_[size_t(s.instance)];
        const Package& p = packages_[inst.package];
        const MdpcModel& m = p.data->models[inst.model];
        Object o;
        o.inst = &inst;
        memcpy(o.world, inst.world ? inst.world : inst.in->world, sizeof(o.world));
        if (s.lod.offset != 0.0f) {  // 0x4def90: away from the camera
            float d[3] = { o.world[12] - eye[0], o.world[13] - eye[1], o.world[14] - eye[2] };
            float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (n > 0)
                for (int j = 0; j < 3; ++j) o.world[12 + j] += d[j] * s.lod.offset / n;
        }
        float alpha = s.lod.alpha;
        uint32_t high = 0;
        if (alpha != 1.0f) {
            high = s.lod.mask & 0x1f;
            while (high & (high - 1)) high &= high - 1;
        }
        bool slot[5] = {};
        for (int l = 0; l < 7; ++l) {
            uint32_t sm = m.lodMask[l];
            if (!(s.lod.mask & sm)) continue;
            if (!(high & (sm & (0u - sm)))) alpha = 1.0f;
            else if (alpha == 0.0f) continue;
            if (l < 5) slot[l] = true;
        }
        o.alpha = alpha;
        size_t oi = objects.size();
        objects.push_back(o);
        ++frameInstances;
        if (s.lod.mask == 0x20) ++frameShadowOnly;
        for (int l = 0; l < 5; ++l) {
            if (!slot[l]) continue;
            ++frameSlots[l];
            for (size_t sm = 0; sm < m.lods[l].size(); ++sm)
                for (size_t g = 0; g < m.lods[l][sm].groups.size(); ++g) {
                    const Package::Group& info = p.groupOf[inst.model][l][sm][g];
                    if (info.fx >= 0)
                        (info.late ? late : items).push_back({ info.fx, oi, &m.lods[l][sm], &m.lods[l][sm].groups[g], &info });
                }
        }
        // 0x5d2990 -> 0x5d2b90: a shadow volume per submitted object while
        // shadows are on.
        if (shadows_ && p.shadowSlot[inst.model] >= 0) volumes.push_back(oi);
    }
    // 0x4e7d90: the detail scale follows the number of objects: (150 - n) /
    // 3000 when that is more than 0.01 either way, kept within 0.8..1.
    float step = (150.0f - float(cityObjects)) * (1.0f / 3000.0f);
    if (std::fabs(step) > 0.01f) {
        lodScale += step;
        if (lodScale < 0.8f) lodScale = 0.8f;
        else if (lodScale > 1.0f) lodScale = 1.0f;
    }
    // Blended groups after the others (stand-in, see render.h), then by
    // effect, render states and texture.
    auto order = [this](const Item& a, const Item& b) {
        bool ba = states_[a.info->state].blend, bb = states_[b.info->state].blend;
        if (ba != bb) return bb;
        if (a.fx != b.fx) return a.fx < b.fx;
        if (a.info->state != b.info->state) return a.info->state < b.info->state;
        return a.info->diffuse < b.info->diffuse;
    };
    std::stable_sort(items.begin(), items.end(), order);
    std::stable_sort(late.begin(), late.end(), order);

    // 0x5de4e0: World = transpose(object matrix) x submesh matrix when +0x4a is set.
    auto worldOf = [](const float* object, const MdpcSubmesh& sm, float world[16]) {
        float wt[16];
        Transpose(object, wt);
        if (sm.hasMatrix) Mul(wt, sm.matrix, world);
        else memcpy(world, wt, 16 * sizeof(float));
    };
    int applied = -1;
    // 0x5dbfc0 / 0x5e05b0: one list in one mode; per effect Begin(&passes, 1 =
    // D3DXFX_DONOTSAVESTATE) and pass 0 (0x5dfdb0); the groups the mode does
    // not draw are left out (0x5e06f0).
    auto drawRange = [&](const std::vector<Item>& list, size_t from, size_t to, bool redraw) {
        for (size_t i = from; i < to;) {
            Effect& e = fx_[list[i].fx];
            size_t end = i;
            bool any = false;
            for (; end < to && &fx_[list[end].fx] == &e; ++end)
                any |= redraw ? list[end].info->redraw >= 0 : DrawnInMode(list[end].info->type, 2);
            UINT passes = 0;
            if (!any || FAILED(e.fx->Begin(&passes, D3DXFX_DONOTSAVESTATE)) || FAILED(e.fx->BeginPass(0))) {
                i = end;
                continue;
            }
            for (; i < end; ++i) {
                const Item& it = list[i];
                int state = redraw ? it.info->redraw : it.info->state;
                if (redraw ? state < 0 : !DrawnInMode(it.info->type, 2)) continue;
                const Object& o = objects[it.object];
                const Package& p = packages_[o.inst->package];
                float world[16];
                // A vehicle part: its World alone (the traced draws have it as
                // g_ObjectToWorld with the vertices as stored).
                if (o.inst->world) Transpose(o.world, world);
                else worldOf(o.world, *it.sm, world);
                DrawGroup(p, p.data->models[o.inst->model], *it.g, *it.info, state, world, applied, o.alpha,
                          o.inst->substance);
                if (redraw) ++frameRedraws;
            }
            e.fx->EndPass();
            e.fx->End();
        }
    };
    auto drawList = [&](const std::vector<Item>& list, bool redraw) { drawRange(list, 0, list.size(), redraw); };

    // List 0 in mode 2. The far quad (type 9) and the shore strips (type
    // 0x0e) come between the opaque and the blended groups: in the traces the
    // far quad comes first, then the shore, then the sea tiles' type 0x0f
    // groups (stand-in for the scene's sort order, which was not decoded).
    UpdateShore(eye);
    frameShore = 0;
    size_t firstBlended = 0;
    while (firstBlended < items.size() && !states_[items[firstBlended].info->state].blend) ++firstBlended;
    drawRange(items, 0, firstBlended, false);
    SetDecl(decl_);  // the buffer objects' vertices have declaration 1
    DrawFar(eye, back, applied);
    DrawShore(false, applied);
    drawRange(items, firstBlended, items.size(), false);
    if (!volumes.empty()) {
        // Modes 4 and 5: every volume twice, effect 0x2b with the request's
        // type 8 (the state sets of Upload).
        Effect& e = fx_[0x2b];
        Package::Group volume;
        volume.fx = 0x2b;
        for (int pass = 0; pass < 2; ++pass) {
            UINT passes = 0;
            if (FAILED(e.fx->Begin(&passes, D3DXFX_DONOTSAVESTATE)) || FAILED(e.fx->BeginPass(0))) break;
            for (size_t oi : volumes) {
                const Instance* inst = objects[oi].inst;
                const Package& p = packages_[inst->package];
                const MdpcModel& m = p.data->models[inst->model];
                float world[16];
                VolumeWorld(inst->in ? inst->in->model : kVehicleModelId, objects[oi].world, eye, world);
                for (const MdpcSubmesh& sm : m.lods[p.shadowSlot[inst->model]]) {
                    for (const MdpcGroup& g : sm.groups) {
                        DrawGroup(p, m, g, volume, shadowState_[pass], world, applied);
                        ++frameShadowDraws;
                    }
                }
            }
            e.fx->EndPass();
            e.fx->End();
        }
        // 0x5d2e2a: directional colour (0, 0, 0, 0), the light uploaded again
        // (0x5dc1e0), list 0 in mode 7 / 6, then the colour back.
        static const float kZero[4] = {};
        SetLight(mood_.ambient, kZero, dir);
        SetEnvSpec(kZero, true);
        DrawShadowAlpha(applied);
        SetDecl(decl_);
        DrawTreeShadows(applied);
        drawList(items, true);  // (the shore strips are not redrawn: mode 2 only in the trace)
        SetLight(mood_.ambient, mood_.directional, dir);
        SetEnvSpec(mood_.directional);
    }
    drawList(late, false);  // list 2, mode 2
}

void CityRenderer::OnLostDevice()
{
    if (shared_) shared_->OnLostDevice();
    for (Effect& e : fx_)
        if (e.fx) e.fx->OnLostDevice();
}

void CityRenderer::OnResetDevice()
{
    // A reset puts every state back to Direct3D's defaults.
    memset(rsValid_, 0, sizeof(rsValid_));
    memset(samplerValid_, 0, sizeof(samplerValid_));
    if (shared_) shared_->OnResetDevice();
    for (Effect& e : fx_)
        if (e.fx) e.fx->OnResetDevice();
}

void CityRenderer::Release()
{
    for (Package& p : packages_) {
        for (IDirect3DVertexBuffer9* b : p.vbs)
            if (b) b->Release();
        if (p.ib) p.ib->Release();
        for (IDirect3DTexture9* t : p.textures)
            if (t) t->Release();
    }
    packages_.clear();
    instances_.clear();
    waterTiles_.clear();
    if (shoreVb_) shoreVb_->Release();
    shoreVb_ = nullptr;
    if (farVb_) farVb_->Release();
    farVb_ = nullptr;
    farInfo.clear();
    for (IDirect3DVertexBuffer9*& b : treeVb_) {
        if (b) b->Release();
        b = nullptr;
    }
    treeInfo.clear();
    treeBuild.clear();
    shoreSlots_ = shoreMaxVerts_ = shoreDrawn_ = 0;
    shoreCache_.clear();
    shoreVerts_.clear();
    shoreLines = 0;
    shoreInfo.clear();
    anims_.clear();
    animOf_.clear();
    for (IDirect3DCubeTexture9*& c : cubes_) {
        if (c) c->Release();
        c = nullptr;
    }
    waterGroups = 0;
    states_.clear();
    skyPackage_ = -1;
    skyInfo.clear();
    memset(rsValid_, 0, sizeof(rsValid_));
    memset(samplerValid_, 0, sizeof(samplerValid_));
    for (Effect& e : fx_) {
        if (e.fx) e.fx->Release();
        e = Effect();
    }
    if (shared_) shared_->Release();
    if (pool_) pool_->Release();
    if (decl_) decl_->Release();
    if (decl5_) decl5_->Release();
    decl5_ = nullptr;
    currentDecl_ = nullptr;
    vehicleParts_.clear();
    vehicleWorlds_.clear();
    vehicleInfo.clear();
    if (white_) white_->Release();
    shared_ = nullptr;
    pool_ = nullptr;
    decl_ = nullptr;
    white_ = nullptr;
    if (dll_) FreeLibrary(dll_);
    dll_ = nullptr;
    device_ = nullptr;
    effects = 0;
    failedEffects.clear();
    groupsDrawn = groupsSkipped = 0;
    shadowInstances = lateGroups = hiddenGroups = 0;
    shadowState_[0] = shadowState_[1] = -1;
    textures = texturesSkipped = groupsUntextured = 0;
}
