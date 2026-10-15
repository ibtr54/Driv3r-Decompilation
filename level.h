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
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "mec.h"

// The files the level loader queues (vtbl+8 0x44b360, in its order) for a
// Take A Ride level; city 0 Miami, 1 Nice, 2 Istanbul.
//   game setup vtbl+0x44  Vehicles\<City>\CarGlobals<City>.vgt
//              vtbl+0x34  Vehicles\<City>.vvv (0x464390; boat and articulated
//                         truck have their own files, not chosen here)
//              vtbl+0x30  Cities\<City><_Day|_Night>_PC.d3c (0x463b70: _Night
//                         when the lighting vtbl+0x28 is 2, 4 or 6; a ride at
//                         night (time 3) turns 1/3/5 into 2/4/6)
//              sfx\sfx.pmu, OVERLAYS\<city>.MAP, OVERLAYS\OVERLAYS.GFX
//              vtbl+0x38  Missions\mission<NN>.dam
//              Guns\Guns.Cpr
//              vtbl+0x3c  Vehicles\<City>.vvs
// The city file is the one the loader's +0x50 entry stands for (0x8ac3d8;
// that object was not traced, the path is from the setup's getter).
std::vector<std::string> RideFileList(int level, int city, bool night);

// Reads the files on a worker thread (the original loads them behind the
// loading screen, polled by state 0xB6 through vtbl+0x10). Only reading:
// nothing is decoded yet.
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

private:
    std::thread worker_;
    std::atomic<bool> done_{ true };
    uint64_t bytes_ = 0;
    double seconds_ = 0;
    std::vector<std::string> missing_;
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
    // Clears the back buffer to the colour and presents it (vsync).
    bool Frame(D3DCOLOR clear);
    const std::string& Error() const { return error_; }

private:
    HMODULE dll_ = nullptr;
    IDirect3D9* d3d_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    D3DPRESENT_PARAMETERS pp_ = {};
    std::string error_;
};
