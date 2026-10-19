#include "level.h"

#include <chrono>
#include <cstring>

namespace {

const char* const kCity[3] = { "Miami", "Nice", "Istanbul" };

}  // namespace

std::vector<std::string> RideFileList(int level, int city, bool night, const std::string& sky)
{
    if (city < 0 || city > 2) city = 0;
    const std::string c = kCity[city];
    char dam[64];
    snprintf(dam, sizeof(dam), "Missions\\mission%.2d.dam", level);  // 0x781ba4
    return {
        "Vehicles\\" + c + "\\CarGlobals" + c + ".vgt",
        "Vehicles\\" + c + ".vvv",
        sky,
        "sfx\\sfx.pmu",
        "OVERLAYS\\" + c + ".MAP",
        "OVERLAYS\\OVERLAYS.GFX",
        dam,
        "Guns\\Guns.Cpr",
        "Vehicles\\" + c + ".vvs",
        "Cities\\" + c + (night ? "_Night" : "_Day") + "_PC.d3c",
    };
}

const MdpcPackage* LevelLoader::FirstPackageOf(const std::string& file) const
{
    auto it = firstOf_.find(file);
    return it != firstOf_.end() && it->second < packages_.size() ? &packages_[it->second] : nullptr;
}

void LevelLoader::Start(const std::string& gameDir, const std::vector<std::string>& files)
{
    Wait();
    done_ = false;
    bytes_ = 0;
    seconds_ = 0;
    missing_.clear();
    report_.clear();
    packages_.clear();
    firstOf_.clear();
    city_ = CityPlacement();
    cityFirst_ = cityEnd_ = 0;
    worker_ = std::thread([this, gameDir, files] {
        auto t0 = std::chrono::steady_clock::now();
        for (const std::string& f : files) {
            std::vector<uint8_t> d = ReadFile(gameDir + "\\" + f);
            if (d.empty()) {
                missing_.push_back(f);
                continue;
            }
            bytes_ += d.size();
            if (f.size() > 4 && _stricmp(f.c_str() + f.size() - 4, ".d3c") == 0) LoadCity(gameDir, f, d);
            else DecodePackages(f, d);
        }
        seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        done_ = true;
    });
}

void LevelLoader::DecodePackages(const std::string& f, const std::vector<uint8_t>& d)
{
    auto chunks = FindMdpcChunks(d);
    size_t first = packages_.size(), models = 0, tris = 0, failed = 0;
    std::vector<uint32_t> t;
    for (const auto& c : chunks) {
        MdpcPackage p;
        if (!p.Load(d, c.first, c.second)) {
            ++failed;
            continue;
        }
        models += p.models.size();
        for (const MdpcModel& m : p.models)
            for (const MdpcSubmesh& sm : m.lods[0])
                for (const MdpcGroup& g : sm.groups) {
                    t.clear();
                    p.Triangles(g, t);
                    tris += t.size() / 3;
                }
        packages_.push_back(std::move(p));
    }
    if (packages_.size() > first) firstOf_[f] = first;
    if (!chunks.empty()) {
        char line[256];
        snprintf(line, sizeof(line), "%s: %zu model packages, %zu models, %zu triangles (level 0)%s", f.c_str(),
                 packages_.size() - first, models, tris, failed ? ", some not read" : "");
        report_.push_back(line);
    }
}

void LevelLoader::LoadCity(const std::string& gameDir, const std::string& f, const std::vector<uint8_t>& d)
{
    // The city loader (0x461ee0): with a PCSL chunk the regions' models are
    // not in the .d3c but in <path without extension>\SuperRegions.pcs
    // (0x7818f8, opened by 0x462ae0; \Interiors.pcs 0x7818e8 is opened the
    // same way and not read here). The instances are in the GEBI chunks.
    char line[256];
    size_t first = packages_.size();
    DecodePackages(f, d);
    if (!city_.Load(d)) {
        snprintf(line, sizeof(line), "%s: placement not read (%s)", f.c_str(), city_.Error().c_str());
        report_.push_back(line);
        return;
    }
    size_t off, size;
    if (FindChunk(d, 0, "PCSL", off, size)) {
        std::string pcs = f.substr(0, f.rfind('.')) + "\\SuperRegions.pcs";
        std::vector<uint8_t> p = ReadFile(gameDir + "\\" + pcs);
        if (p.empty()) missing_.push_back(pcs);
        bytes_ += p.size();
        DecodePackages(pcs, p);
    }
    // A model is package uid << 10 | index, looked up through the table
    // the packages register in by uid (0x57c058, 0x5cbdd0); here only the
    // city's own packages (other files reuse the small uids).
    size_t models = 0, resolved = 0;
    for (const CityInstance& in : city_.instances) {
        if (!in.HasModel()) continue;
        ++models;
        for (size_t k = first; k < packages_.size(); ++k)
            if (packages_[k].uid == in.PackageUid()) {
                if (in.ModelIndex() < packages_[k].models.size()) ++resolved;
                break;
            }
    }
    cityFirst_ = first;
    cityEnd_ = packages_.size();
    snprintf(line, sizeof(line), "%s: %zu instances on a %d x %d grid, %zu with a model, %zu of them found", f.c_str(),
             city_.instances.size(), city_.gridSide, city_.gridSide, models, resolved);
    report_.push_back(line);
}

bool OverlayGfx::Load(const std::string& path)
{
    materials_.clear();
    items_.clear();
    std::vector<uint8_t> d = ReadFile(path);
    size_t up, upSize, md, mdSize;
    if (!FindChunk(d, 0, "UPST", up, upSize) || !FindChunk(d, 0, "MDPC", md, mdSize) || upSize < 0x14) return false;
    materials_ = LoadMaterials(d, md, mdSize);
    uint16_t group, count;
    memcpy(&group, &d[up + 0x10], 2);
    memcpy(&count, &d[up + 0x12], 2);
    for (size_t k = 0; k < count && up + 0x14 + 4 * k + 4 <= up + upSize && k < materials_.size(); ++k) {
        uint16_t index, g;
        memcpy(&index, &d[up + 0x14 + 4 * k], 2);
        memcpy(&g, &d[up + 0x16 + 4 * k], 2);
        items_[uint32_t(g) << 16 | index] = k;
    }
    return !items_.empty();
}

const Texture* OverlayGfx::Item(uint32_t id) const
{
    auto it = items_.find(id);
    return it == items_.end() || materials_[it->second].px.empty() ? nullptr : &materials_[it->second];
}

bool D3DView::Open(HWND hwnd)
{
    Close();
    dll_ = LoadLibraryA("d3d9.dll");
    typedef IDirect3D9*(WINAPI * CreateFn)(UINT);
    CreateFn create = dll_ ? reinterpret_cast<CreateFn>(GetProcAddress(dll_, "Direct3DCreate9")) : nullptr;
    d3d_ = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d_) {
        error_ = "Direct3D 9 not available";
        Close();
        return false;
    }
    pp_ = {};
    pp_.Windowed = TRUE;
    pp_.SwapEffect = D3DSWAPEFFECT_DISCARD;
    // The game asks for A8R8G8B8 (traced: format 21): the shadow pass keeps
    // its strength in the destination alpha. Windowed that needs the
    // conversion to the desktop's format; without it the desktop's format.
    // Size from the client area.
    D3DDISPLAYMODE mode = {};
    alpha_ = SUCCEEDED(d3d_->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode)) &&
             SUCCEEDED(d3d_->CheckDeviceType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format, D3DFMT_A8R8G8B8, TRUE));
    pp_.BackBufferFormat = alpha_ ? D3DFMT_A8R8G8B8 : D3DFMT_UNKNOWN;
    pp_.hDeviceWindow = hwnd;
    pp_.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    pp_.EnableAutoDepthStencil = TRUE;
    pp_.AutoDepthStencilFormat = D3DFMT_D24S8;
    HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp_,
                                    &device_);
    if (FAILED(hr))
        hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp_,
                                &device_);
    if (FAILED(hr) && alpha_) {
        alpha_ = false;
        pp_.BackBufferFormat = D3DFMT_UNKNOWN;
        hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp_,
                                &device_);
    }
    if (FAILED(hr)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "CreateDevice failed (0x%08lx)", hr);
        error_ = msg;
        Close();
        return false;
    }
    return true;
}

void D3DView::Close()
{
    if (device_) device_->Release();
    if (d3d_) d3d_->Release();
    if (dll_) FreeLibrary(dll_);
    device_ = nullptr;
    d3d_ = nullptr;
    dll_ = nullptr;
}

bool D3DView::Frame(D3DCOLOR clear, const std::function<void(IDirect3DDevice9*)>& draw)
{
    if (!device_) return false;
    HRESULT hr = device_->TestCooperativeLevel();
    if (hr == D3DERR_DEVICELOST) return true;  // e.g. another window full screen: wait
    if (hr == D3DERR_DEVICENOTRESET) {
        if (onLost) onLost();
        if (FAILED(device_->Reset(&pp_))) return true;
        if (onReset) onReset();
    }
    // Target, depth and stencil (0), like the game's frame clear (flags 7).
    device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, clear, 1.0f, 0);
    if (SUCCEEDED(device_->BeginScene())) {
        if (draw) draw(device_);
        device_->EndScene();
    }
    device_->Present(nullptr, nullptr, nullptr, nullptr);
    return true;
}

float D3DView::Aspect() const
{
    RECT r = {};
    if (pp_.hDeviceWindow) GetClientRect(pp_.hDeviceWindow, &r);
    return r.bottom > r.top ? float(r.right - r.left) / float(r.bottom - r.top) : 4.0f / 3.0f;
}
