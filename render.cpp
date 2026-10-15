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

// The traced g_fExtrusion of every shadow volume draw (c9 = (20, 0, 0, 0)).
// 0x5defb0 sets it from the drawn object's +0x70, which the copy made by
// 0x5d2b90 (0x70 bytes) does not write; where the 20 comes from was not
// found.
const float kExtrusion = 20.0f;

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

}  // namespace

void CameraMatrix(const float eye[3], const float at[3], float fovY, float aspect, float zNear, float zFar, float out[16])
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
    float ys = 1.0f / std::tan(fovY * 0.5f), xs = ys / aspect, q = zFar / (zNear - zFar);
    const float proj[16] = { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, q, -1, 0, 0, q * zNear, 0 };
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
        // 0x5d5790: the first valid technique.
        D3DXHANDLE tech = nullptr;
        if (SUCCEEDED(fx->FindNextValidTechnique(nullptr, &tech)) && tech) fx->SetTechnique(tech);
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
        if (f.extrusion) {
            const D3DXVECTOR4 v(kExtrusion, 0, 0, 0);
            fx->SetVector(f.extrusion, &v);
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

// The effect and textures of every group of slot 0 (0x5d68e0, 0x5e0970;
// first substance); byUid: package uid -> index into packages_.
void CityRenderer::ResolveGroups(Package& p, const std::map<uint32_t, int>& byUid)
{
    p.groupOf.resize(p.data->models.size());
    for (size_t m = 0; m < p.data->models.size(); ++m) {
        const MdpcModel& model = p.data->models[m];
        int id = RenderId(model);
        bool layout = (DeclIndex(model.vertexType) == 1 || DeclIndex(model.vertexType) == 2) &&
                      model.vertexBuffer < p.data->vertexBuffers.size() &&
                      p.data->vertexBuffers[model.vertexBuffer].stride == 48;
        for (const MdpcSubmesh& sm : model.lods[0]) {
            std::vector<Package::Group> groups;
            for (const MdpcGroup& g : sm.groups) {
                Package::Group gr;
                gr.fx = id;
                // No substance: the binder's request keeps type 8 (0x5e0900).
                uint8_t s[8] = { 8 };
                auto it = byUid.find(g.material >> 16);
                if (g.material != 0xffff0000 && g.material != 0xcccccccc && it != byUid.end()) {
                    const Package& owner = packages_[it->second];
                    uint32_t mi = g.material & 0xffff;
                    if (mi < owner.data->materials.size() && !owner.data->materials[mi].empty()) {
                        const MdpcSubstance& sub = owner.data->materials[mi][0];
                        memcpy(s, sub.bytes, 8);
                        if (!(model.flags & 2)) ModelSubstance(s);
                        int index = (((s[4] & 0x7f) + id * 4) * 3 + s[5]) * 5 + s[6];
                        if (int e = SubstanceEffect(index)) gr.fx = e;
                        // Texture references are entries of the material's own package.
                        if (!sub.textures.empty() && size_t(sub.textures[0]) < owner.textures.size())
                            gr.diffuse = owner.textures[sub.textures[0]];
                        if ((s[6] == 1 || s[6] == 4) && sub.textures.size() >= 2 &&
                            size_t(sub.textures[1]) < owner.textures.size())
                            gr.normal = owner.textures[sub.textures[1]];
                    }
                }
                if (!layout || gr.fx <= 0 || gr.fx >= 0x30 || !fx_[gr.fx].fx) gr.fx = -1;
                gr.state = StateFor(s);
                gr.type = int8_t(s[0]);
                gr.late = LateType(gr.type);
                if (!gr.late && DrawnInMode(gr.type, 7)) gr.redraw = StateFor(s, EffectiveMode(7, gr.type));
                groups.push_back(gr);
            }
            p.groupOf[m].push_back(std::move(groups));
        }
    }
    // 0x5ca020: slot 5 gets the mask 0x20 when it has submeshes, else slot 6
    // (the files have 0 in both).
    p.shadowSlot.assign(p.data->models.size(), -1);
    for (size_t m = 0; m < p.data->models.size(); ++m) {
        const MdpcModel& model = p.data->models[m];
        bool layout = (DeclIndex(model.vertexType) == 1 || DeclIndex(model.vertexType) == 2) &&
                      model.vertexBuffer < p.data->vertexBuffers.size() &&
                      p.data->vertexBuffers[model.vertexBuffer].stride == 48;
        if (!layout || !fx_[0x2b].fx) continue;
        if (!model.lods[5].empty()) p.shadowSlot[m] = 5;
        else if (!model.lods[6].empty()) p.shadowSlot[m] = 6;
    }
}

bool CityRenderer::Upload(const std::vector<MdpcPackage>& packages, size_t first, size_t end, const CityPlacement& city,
                          const MdpcPackage* sky)
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
    // The sky's materials are its own (uid 0x34; city packages may use the
    // same small uid).
    skyInfo.clear();
    if (sky && !sky->models.empty()) {
        skyPackage_ = int(packages_.size());
        AddPackage(*sky);
        ResolveGroups(packages_.back(), { { sky->uid, skyPackage_ } });
        for (const auto& sm : packages_.back().groupOf[0])
            for (const Package::Group& g : sm) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%sgroup: effect %d, preset 0x%02x", skyInfo.empty() ? "" : "; ", g.fx,
                         unsigned(states_[g.state].base));
                skyInfo += buf;
            }
    }
    for (const CityInstance& in : city.instances) {
        if (!in.HasModel()) continue;
        auto it = byUid.find(in.PackageUid());
        if (it == byUid.end() || in.ModelIndex() >= packages_[it->second].data->models.size()) continue;
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
        for (const auto& sm : packages_[it->second].groupOf[inst.model])
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
// 0x1f, 0x20, 0x21 the ambient colour with w 0.35; 0x23 zero; 9 and 0xb the
// light state's +0x140 (not decoded: their default is kept); others
// unchanged.
void CityRenderer::SetEnvSpec(const float colour[4])
{
    for (int id = 0; id < 0x30; ++id) {
        Effect& e = fx_[id];
        if (!e.fx || !e.envSpec) continue;
        D3DXVECTOR4 v(0, 0, 0, 0);
        if (id == 2 || id == 3 || id == 5 || id == 6 || id == 0x24 || id == 0x25)
            v = D3DXVECTOR4(colour[0], colour[1], colour[2], 0);
        else if (id == 0x1d || id == 0x1f || id == 0x20 || id == 0x21)
            v = D3DXVECTOR4(mood_.ambient[0], mood_.ambient[1], mood_.ambient[2], 0.35f);
        else if (id != 0x23)
            continue;
        e.fx->SetVector(e.envSpec, &v);
    }
}

// One group with its effect already begun: World, textures, CommitChanges,
// its render states and the DrawIndexedPrimitive (0x5de4e0, 0x5e0970,
// 0x5d4440, 0x5dfe20). world: row-vector object matrix.
void CityRenderer::DrawGroup(const Package& p, const MdpcModel& m, const MdpcGroup& g, const Package::Group& info,
                             int state, const float world[16], int& applied)
{
    Effect& e = fx_[info.fx];
    if (e.world) e.fx->SetMatrix(e.world, reinterpret_cast<const D3DXMATRIX*>(world));
    if (e.diffuse) e.fx->SetTexture(e.diffuse, info.diffuse ? info.diffuse : white_);
    if (e.normalMap) e.fx->SetTexture(e.normalMap, info.normal ? info.normal : white_);
    e.fx->CommitChanges();
    if (state != applied) {
        ApplyState(states_[state]);
        applied = state;
    }
    IDirect3DVertexBuffer9* vb = p.vbs[m.vertexBuffer];
    if (!vb || !p.ib) return;
    device_->SetStreamSource(0, vb, 0, p.data->vertexBuffers[m.vertexBuffer].stride);
    device_->SetIndices(p.ib);
    // 0x5dfe20: the group's six DrawIndexedPrimitive values.
    device_->DrawIndexedPrimitive(D3DPRIMITIVETYPE(g.type), g.baseVertex, g.minIndex, g.numVertices, g.startIndex,
                                  g.primitives);
    ++frameDraws;
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
    for (size_t s = 0; s < m.lods[0].size() && s < p.groupOf[0].size(); ++s) {
        const MdpcSubmesh& sm = m.lods[0][s];
        if (sm.hasMatrix) Mul(wt, sm.matrix, world);
        else memcpy(world, wt, sizeof(world));
        for (size_t g = 0; g < sm.groups.size(); ++g) {
            const Package::Group& info = p.groupOf[0][s][g];
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

void CityRenderer::Draw(const float viewProj[16], const float eye[3], float range)
{
    frameInstances = frameDraws = 0;
    if (!device_ || !shared_) return;
    float t[16];
    Transpose(viewProj, t);  // 0x5dc4b0
    shared_->SetMatrix(viewProj_, reinterpret_cast<const D3DXMATRIX*>(t));
    const D3DXVECTOR4 cam(eye[0], eye[1], eye[2], 1.0f);
    if (camPos_) shared_->SetVector(camPos_, &cam);
    device_->SetVertexDeclaration(decl_);
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

    // The scene's lists (0x5d2ca0): 0 = items, 1 = volumes, 2 = late.
    struct Item { int fx; const Instance* inst; const MdpcSubmesh* sm; const MdpcGroup* g; const Package::Group* info; };
    std::vector<Item> items, late;
    std::vector<const Instance*> volumes;
    frameShadowDraws = frameRedraws = 0;
    for (const Instance& inst : instances_) {
        float d[3] = { inst.centre[0] - eye[0], inst.centre[1] - eye[1], inst.centre[2] - eye[2] };
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > range * range) continue;
        // Behind the camera (clip w of the box centre, with some margin).
        const float* c = inst.centre;
        float w = c[0] * viewProj[3] + c[1] * viewProj[7] + c[2] * viewProj[11] + viewProj[15];
        if (w < -60.0f) continue;
        ++frameInstances;
        const Package& p = packages_[inst.package];
        const MdpcModel& m = p.data->models[inst.model];
        for (size_t s = 0; s < m.lods[0].size(); ++s)
            for (size_t g = 0; g < m.lods[0][s].groups.size(); ++g) {
                const Package::Group& info = p.groupOf[inst.model][s][g];
                if (info.fx >= 0)
                    (info.late ? late : items).push_back({ info.fx, &inst, &m.lods[0][s], &m.lods[0][s].groups[g], &info });
            }
        // 0x5d2990 -> 0x5d2b90: a shadow volume per object while shadows are on.
        if (shadows_ && p.shadowSlot[inst.model] >= 0) volumes.push_back(&inst);
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
    auto worldOf = [](const Instance& inst, const MdpcSubmesh& sm, float world[16]) {
        float wt[16];
        Transpose(inst.in->world, wt);
        if (sm.hasMatrix) Mul(wt, sm.matrix, world);
        else memcpy(world, wt, 16 * sizeof(float));
    };
    int applied = -1;
    // 0x5dbfc0 / 0x5e05b0: one list in one mode; per effect Begin(&passes, 1 =
    // D3DXFX_DONOTSAVESTATE) and pass 0 (0x5dfdb0); the groups the mode does
    // not draw are left out (0x5e06f0).
    auto drawList = [&](const std::vector<Item>& list, bool redraw) {
        for (size_t i = 0; i < list.size();) {
            Effect& e = fx_[list[i].fx];
            size_t end = i;
            bool any = false;
            for (; end < list.size() && &fx_[list[end].fx] == &e; ++end)
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
                const Package& p = packages_[it.inst->package];
                float world[16];
                worldOf(*it.inst, *it.sm, world);
                DrawGroup(p, p.data->models[it.inst->model], *it.g, *it.info, state, world, applied);
                if (redraw) ++frameRedraws;
            }
            e.fx->EndPass();
            e.fx->End();
        }
    };

    drawList(items, false);  // list 0, mode 2
    if (!volumes.empty()) {
        // Modes 4 and 5: every volume twice, effect 0x2b with the request's
        // type 8 (the state sets of Upload).
        Effect& e = fx_[0x2b];
        Package::Group volume;
        volume.fx = 0x2b;
        for (int pass = 0; pass < 2; ++pass) {
            UINT passes = 0;
            if (FAILED(e.fx->Begin(&passes, D3DXFX_DONOTSAVESTATE)) || FAILED(e.fx->BeginPass(0))) break;
            for (const Instance* inst : volumes) {
                const Package& p = packages_[inst->package];
                const MdpcModel& m = p.data->models[inst->model];
                for (const MdpcSubmesh& sm : m.lods[p.shadowSlot[inst->model]]) {
                    float world[16];
                    worldOf(*inst, sm, world);
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
        SetEnvSpec(kZero);
        drawList(items, true);
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
