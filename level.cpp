#include "level.h"

#include <chrono>
#include <cstring>

namespace {

const char* const kCity[3] = { "Miami", "Nice", "Istanbul" };

}  // namespace

std::vector<std::string> RideFileList(int level, int city, bool night)
{
    if (city < 0 || city > 2) city = 0;
    const std::string c = kCity[city];
    char dam[64];
    snprintf(dam, sizeof(dam), "Missions\\mission%.2d.dam", level);  // 0x781ba4
    return {
        "Vehicles\\" + c + "\\CarGlobals" + c + ".vgt",
        "Vehicles\\" + c + ".vvv",
        "Cities\\" + c + (night ? "_Night" : "_Day") + "_PC.d3c",
        "sfx\\sfx.pmu",
        "OVERLAYS\\" + c + ".MAP",
        "OVERLAYS\\OVERLAYS.GFX",
        dam,
        "Guns\\Guns.Cpr",
        "Vehicles\\" + c + ".vvs",
    };
}

void LevelLoader::Start(const std::string& gameDir, const std::vector<std::string>& files)
{
    Wait();
    done_ = false;
    bytes_ = 0;
    seconds_ = 0;
    missing_.clear();
    worker_ = std::thread([this, gameDir, files] {
        auto t0 = std::chrono::steady_clock::now();
        std::vector<char> buf(1 << 20);
        for (const std::string& f : files) {
            HANDLE h = CreateFileA((gameDir + "\\" + f).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                missing_.push_back(f);
                continue;
            }
            DWORD got = 0;
            while (::ReadFile(h, buf.data(), DWORD(buf.size()), &got, nullptr) && got > 0) bytes_ += got;
            CloseHandle(h);
        }
        seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        done_ = true;
    });
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
    pp_.BackBufferFormat = D3DFMT_UNKNOWN;  // the desktop's format; size from the client area
    pp_.hDeviceWindow = hwnd;
    pp_.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp_,
                                    &device_);
    if (FAILED(hr))
        hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp_,
                                &device_);
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

bool D3DView::Frame(D3DCOLOR clear)
{
    if (!device_) return false;
    HRESULT hr = device_->TestCooperativeLevel();
    if (hr == D3DERR_DEVICELOST) return true;  // e.g. another window full screen: wait
    if (hr == D3DERR_DEVICENOTRESET && FAILED(device_->Reset(&pp_))) return true;
    device_->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0);
    if (SUCCEEDED(device_->BeginScene())) device_->EndScene();
    device_->Present(nullptr, nullptr, nullptr, nullptr);
    return true;
}
