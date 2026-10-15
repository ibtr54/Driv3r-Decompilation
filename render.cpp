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

}  // namespace

void CameraMatrix(const float eye[3], const float at[3], float fovY, float aspect, float zNear, float zFar, float out[16])
{
    float z[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
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
    float ys = 1.0f / std::tan(fovY * 0.5f), xs = ys / aspect, q = zFar / (zFar - zNear);
    const float proj[16] = { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, q, 1, 0, 0, -q * zNear, 0 };
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
            continue;
        }
        Effect& f = fx_[e.id];
        f.fx = fx;
        // 0x5de7b0: "World" by semantic, g_fAlpha, g_fBrightness by name.
        f.world = fx->GetParameterBySemantic(nullptr, "World");
        f.alpha = fx->GetParameterByName(nullptr, "g_fAlpha");
        f.brightness = fx->GetParameterByName(nullptr, "g_fBrightness");
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

bool CityRenderer::Upload(const std::vector<MdpcPackage>& packages, size_t first, size_t end, const CityPlacement& city)
{
    if (!device_) { error_ = "no device"; return false; }
    std::map<uint32_t, int> byUid;  // the table 0x8b5310 (0x57c058)
    for (size_t k = first; k < end && k < packages.size(); ++k) {
        Package p;
        p.data = &packages[k];
        for (const MdpcVertexBuffer& vb : p.data->vertexBuffers) {
            IDirect3DVertexBuffer9* b = nullptr;
            void* ptr;
            if (SUCCEEDED(device_->CreateVertexBuffer(UINT(vb.data.size()), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &b, nullptr)) &&
                SUCCEEDED(b->Lock(0, 0, &ptr, 0))) {
                memcpy(ptr, vb.data.data(), vb.data.size());
                b->Unlock();
            }
            p.vbs.push_back(b);
        }
        if (!p.data->indices.empty()) {
            UINT bytes = UINT(p.data->indices.size() * 2);
            void* ptr;
            if (SUCCEEDED(device_->CreateIndexBuffer(bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &p.ib, nullptr)) &&
                SUCCEEDED(p.ib->Lock(0, 0, &ptr, 0))) {
                memcpy(ptr, p.data->indices.data(), bytes);
                p.ib->Unlock();
            }
        }
        byUid[p.data->uid] = int(packages_.size());
        packages_.push_back(std::move(p));
    }
    // The effect of every group of slot 0 (0x5d68e0, first substance).
    for (Package& p : packages_) {
        p.effectOf.resize(p.data->models.size());
        for (size_t m = 0; m < p.data->models.size(); ++m) {
            const MdpcModel& model = p.data->models[m];
            int id = RenderId(model);
            bool layout = (DeclIndex(model.vertexType) == 1 || DeclIndex(model.vertexType) == 2) &&
                          model.vertexBuffer < p.data->vertexBuffers.size() &&
                          p.data->vertexBuffers[model.vertexBuffer].stride == 48;
            for (const MdpcSubmesh& sm : model.lods[0]) {
                std::vector<int> ids;
                for (const MdpcGroup& g : sm.groups) {
                    int fx = id;
                    auto it = byUid.find(g.material >> 16);
                    if (g.material != 0xffff0000 && g.material != 0xcccccccc && it != byUid.end()) {
                        const MdpcPackage& owner = *packages_[it->second].data;
                        uint32_t mi = g.material & 0xffff;
                        if (mi < owner.materials.size() && !owner.materials[mi].empty()) {
                            const uint8_t* s = owner.materials[mi][0].bytes;
                            int index = (((s[4] & 0x7f) + id * 4) * 3 + s[5]) * 5 + s[6];
                            if (int e = SubstanceEffect(index)) fx = e;
                        }
                    }
                    if (!layout || fx <= 0 || fx >= 0x30 || !fx_[fx].fx) fx = -1;
                    ids.push_back(fx);
                }
                p.effectOf[m].push_back(std::move(ids));
            }
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
        for (const auto& sm : packages_[it->second].effectOf[inst.model])
            for (int e : sm) (e >= 0 ? groupsDrawn : groupsSkipped)++;
        instances_.push_back(inst);
    }
    return true;
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

    struct Item { int fx; const Instance* inst; const MdpcSubmesh* sm; const MdpcGroup* g; };
    std::vector<Item> items;
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
                int fx = p.effectOf[inst.model][s][g];
                if (fx >= 0) items.push_back({ fx, &inst, &m.lods[0][s], &m.lods[0][s].groups[g] });
            }
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.fx < b.fx; });

    device_->SetVertexDeclaration(decl_);
    for (size_t i = 0; i < items.size();) {
        Effect& e = fx_[items[i].fx];
        UINT passes = 0;
        // 0x5dfdb0: Begin(&passes, 1 = D3DXFX_DONOTSAVESTATE), pass 0.
        if (FAILED(e.fx->Begin(&passes, D3DXFX_DONOTSAVESTATE)) || FAILED(e.fx->BeginPass(0))) {
            while (i < items.size() && &fx_[items[i].fx] == &e) ++i;
            continue;
        }
        for (; i < items.size() && &fx_[items[i].fx] == &e; ++i) {
            const Item& it = items[i];
            const Package& p = packages_[it.inst->package];
            const MdpcModel& m = p.data->models[it.inst->model];
            // 0x5de4e0: transpose(world) x submesh matrix when +0x4a is set.
            float wt[16], world[16];
            Transpose(it.inst->in->world, wt);
            if (it.sm->hasMatrix) Mul(wt, it.sm->matrix, world);
            else memcpy(world, wt, sizeof(world));
            if (e.world) e.fx->SetMatrix(e.world, reinterpret_cast<const D3DXMATRIX*>(world));
            e.fx->CommitChanges();
            IDirect3DVertexBuffer9* vb = p.vbs[m.vertexBuffer];
            if (!vb || !p.ib) continue;
            device_->SetStreamSource(0, vb, 0, p.data->vertexBuffers[m.vertexBuffer].stride);
            device_->SetIndices(p.ib);
            // 0x5dfe20: the group's six DrawIndexedPrimitive values.
            const MdpcGroup& g = *it.g;
            device_->DrawIndexedPrimitive(D3DPRIMITIVETYPE(g.type), g.baseVertex, g.minIndex, g.numVertices, g.startIndex,
                                          g.primitives);
            ++frameDraws;
        }
        e.fx->EndPass();
        e.fx->End();
    }
}

void CityRenderer::OnLostDevice()
{
    if (shared_) shared_->OnLostDevice();
    for (Effect& e : fx_)
        if (e.fx) e.fx->OnLostDevice();
}

void CityRenderer::OnResetDevice()
{
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
    }
    packages_.clear();
    instances_.clear();
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
}
