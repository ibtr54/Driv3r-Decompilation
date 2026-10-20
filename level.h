// Gameplay start (state 0xB4), as far as it is implemented: the level's file
// list and its loading on a worker thread (state 0xB6, level loader 0x8ac338),
// the loading screen's graphics (OVERLAYS\LOADING.GFX) and a Direct3D 9 view
// for the level itself.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <functional>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "city.h"
#include "mdpc.h"
#include "mec.h"
#include "vehicle.h"

// The files the level loader queues (vtbl+8 0x44b360, in its order) for a
// Take A Ride level; city 0 Miami, 1 Nice, 2 Istanbul.
//   game setup vtbl+0x44  Vehicles\<City>\CarGlobals<City>.vgt
//              vtbl+0x34  Vehicles\<City>.vvv (0x464390; boat and articulated
//                         truck have their own files, not chosen here)
//   mood [0x8ac3d8] +0x50 the sky dome, Skies\sky_<name>.d3s (SKYDOME_NAME
//                         of the level's mood, mood.h)
//              sfx\sfx.pmu, OVERLAYS\<city>.MAP, OVERLAYS\OVERLAYS.GFX
//              vtbl+0x38  Missions\mission<NN>.dam
//              Guns\Guns.Cpr
// Read here as well (not from that queue):
//              vtbl+0x3c  Vehicles\<City>.vvs
//              vtbl+0x30  Cities\<City><_Day|_Night>_PC.d3c (0x463b70: _Night
//                         when the lighting vtbl+0x28 is 2, 4 or 6; a ride at
//                         night (time 3) turns 1/3/5 into 2/4/6), opened by
//                         the city object itself (0x461e80 -> 0x461ef5).
std::vector<std::string> RideFileList(int level, int city, bool night, const std::string& sky);

// Reads the files on a worker thread (the original loads them behind the
// loading screen, polled by state 0xB6 through vtbl+0x10). Only reading:
// the model packages (MDPC chunks) in them are decoded (mdpc.h), and for
// the .d3c the city's placement (city.h) and SuperRegions.pcs; the rest
// of the data is not used yet.
class LevelLoader
{
public:
    ~LevelLoader() { Wait(); }
    void Start(const std::string& gameDir, const std::vector<std::string>& files);
    bool Done() const { return done_; }
    void Wait() { if (worker_.joinable()) worker_.join(); }
    uint64_t Bytes() const { return bytes_; }
    double Seconds() const { return seconds_; }
    const std::vector<std::string>& Missing() const { return missing_; }
    // Per file with model packages: "<file>: N packages, M models, T
    // triangles" (failed packages named); filled when Done().
    const std::vector<std::string>& Report() const { return report_; }
    const std::vector<MdpcPackage>& Packages() const { return packages_; }
    const CityPlacement& City() const { return city_; }
    // The city's packages: Packages()[CityFirst(), CityEnd()).
    size_t CityFirst() const { return cityFirst_; }
    size_t CityEnd() const { return cityEnd_; }
    // The first package decoded from a file of the list (nullptr: none).
    const MdpcPackage* FirstPackageOf(const std::string& file) const;
    // The vehicles of the .vvs (vehicle.h) and the package of each (index
    // into Packages(), -1: its MDPC was not read).
    const std::vector<VehicleData>& Vehicles() const { return vehicles_; }
    const std::vector<int>& VehiclePackages() const { return vehiclePackages_; }

private:
    void DecodePackages(const std::string& f, const std::vector<uint8_t>& d);
    void LoadCity(const std::string& gameDir, const std::string& f, const std::vector<uint8_t>& d);
    std::thread worker_;
    std::atomic<bool> done_{ true };
    uint64_t bytes_ = 0;
    double seconds_ = 0;
    std::vector<std::string> missing_, report_;
    std::vector<MdpcPackage> packages_;
    std::map<std::string, size_t> firstOf_;  // file -> its first package
    CityPlacement city_;
    size_t cityFirst_ = 0, cityEnd_ = 0;
    std::vector<VehicleData> vehicles_;
    std::vector<int> vehiclePackages_;
};

// OVERLAYS\*.GFX: CHNK { UPST, MDPC }. UPST (as found in LOADING.GFX, not
// traced in the code): +0x10 u16 group, u16 count; then count x {u16 index,
// u16 group}; item id = group << 16 | index, and the entries are in the
// order of the MDPC materials. LOADING.GFX: 0x380000 the 256x128 DRIV3R
// logo, 0x380001..0x380010 the 16 frames of the 32x32 spinning hubcap.
class OverlayGfx
{
public:
    bool Load(const std::string& path);
    const Texture* Item(uint32_t id) const;

private:
    std::vector<Texture> materials_;
    std::map<uint32_t, size_t> items_;
};

// A windowed Direct3D 9 device on the game window, used while a level runs;
// the menus keep their own drawing. d3d9.dll is loaded at run time.
class D3DView
{
public:
    ~D3DView() { Close(); }
    bool Open(HWND hwnd);
    void Close();
    bool IsOpen() const { return device_ != nullptr; }
    // Clears the back buffer to the colour (and the depth buffer), calls
    // draw between BeginScene and EndScene, and presents it (vsync).
    bool Frame(D3DCOLOR clear, const std::function<void(IDirect3DDevice9*)>& draw = nullptr);
    IDirect3DDevice9* Device() const { return device_; }
    float Aspect() const;
    // The back buffer keeps alpha (A8R8G8B8, as the unpatched game asks: the
    // shadow redraw blends by the destination alpha); false: X8R8G8B8 (the
    // shadow patch, level.cpp kShadowPatch) or the desktop's format.
    bool HasAlpha() const { return alpha_; }
    // Called around a device reset (effects must release and recreate
    // their default-pool resources).
    std::function<void()> onLost, onReset;
    const std::string& Error() const { return error_; }

private:
    HMODULE dll_ = nullptr;
    IDirect3D9* d3d_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    D3DPRESENT_PARAMETERS pp_ = {};
    bool alpha_ = false;
    std::string error_;
};
