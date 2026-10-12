// Driv3r (PC, 2005) boot sequence + front-end menu, re-implemented from
// Driv3r.exe (sha256 355ad3d5...0f02) analysed with Ghidra.
//
// Covered: WinMain window setup, boot sequencer (state 0xA9), intro FMVs
// (0xAA), language select (0xAB), "Press ENTER" title (0xAC) and the
// front-end menu. Gameplay is NOT implemented: where the original calls
// FUN_00557d70 -> state 0xB3 (load game), this program closes itself.
//
// Menus are drawn from the game's own .MEC layout files and FONT.BNK (see
// mec.h). Uses the game's binkw32.dll for the .XMV files (they are Bink
// 'BIKi' videos), so it must be built as a 32-bit (x86) executable.
// Addresses in comments refer to the original Driv3r.exe.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <d3d9.h>
#include <dsound.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "mec.h"

#pragma comment(lib, "winmm.lib")  // waveOut (menu sounds, music)
#pragma comment(lib, "dinput8.lib")  // joystick (DirectInput8Create, c_dfDIJoystick2)
#pragma comment(lib, "dxguid.lib")

// ============================================================== logging ====
static FILE* g_log = nullptr;

static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

// ================================================================= Bink ====
// Only the imports Driv3r.exe itself uses (IAT 0x6e025c..0x6e0280).
typedef void* HBINK;
struct BinkHeader { uint32_t Width, Height, Frames, FrameNum; };  // leading BINK fields
static const uint32_t BINKSURFACE32   = 3;
static const uint32_t BINKCOPYALL     = 0x80000000u;  // used on the first frame (0x5994a2)
static const uint32_t BINKSNDTRACK    = 0x00004000u;  // second BinkOpen (0x599616)
static const size_t   BINK_NUMTRACKS  = 0xF0;         // read at 0x5995f6

struct BinkApi
{
    HMODULE dll = nullptr;
    HBINK (__stdcall* Open)(const char*, uint32_t) = nullptr;
    void  (__stdcall* Close)(HBINK) = nullptr;
    int   (__stdcall* Wait)(HBINK) = nullptr;
    int   (__stdcall* DoFrame)(HBINK) = nullptr;
    void  (__stdcall* NextFrame)(HBINK) = nullptr;
    int   (__stdcall* CopyToBuffer)(HBINK, void*, int, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
    void  (__stdcall* SetSoundTrack)(uint32_t, uint32_t*) = nullptr;
    void  (__stdcall* SetVolume)(HBINK, uint32_t, int32_t) = nullptr;
    int   (__stdcall* SetSoundSystem)(void*, uint32_t) = nullptr;
    void* (__stdcall* OpenDirectSound)(uint32_t) = nullptr;

    template <class T> bool Get(T& fn, const char* name)
    {
        fn = reinterpret_cast<T>(GetProcAddress(dll, name));
        if (!fn) Log("binkw32.dll: missing export %s", name);
        return fn != nullptr;
    }

    bool Load(const std::string& gameDir)
    {
        dll = LoadLibraryA((gameDir + "\\binkw32.dll").c_str());
        if (!dll) return false;
        bool ok = Get(Open, "_BinkOpen@8") & Get(Close, "_BinkClose@4") &
                  Get(Wait, "_BinkWait@4") & Get(DoFrame, "_BinkDoFrame@4") &
                  Get(NextFrame, "_BinkNextFrame@4") &
                  Get(CopyToBuffer, "_BinkCopyToBuffer@28") &
                  Get(SetSoundTrack, "_BinkSetSoundTrack@8") &
                  Get(SetVolume, "_BinkSetVolume@12") &
                  Get(SetSoundSystem, "_BinkSetSoundSystem@8") &
                  Get(OpenDirectSound, "_BinkOpenDirectSound@4");
        if (ok) SetSoundSystem(reinterpret_cast<void*>(OpenDirectSound), 0);
        return ok;
    }
};
static BinkApi g_bink;

// Video sound. The original's player (created by 0x599890) calls
// BinkSetSoundSystem(BinkOpenDirectSound, sound system 0x8ac2b0 +0x75c):
// Bink plays through the game's own DirectSound object, opened on the
// chosen driver (0x5976b0). Here that object is made by ApplySound and
// handed to Bink before every BinkOpen; "No Sound" opens the video with
// no sound track. Bink keeps using the object it got while any video is
// open (the menu background loops), so a replaced object is released only
// once no video is open; released at once, the next BinkOpen hung.
struct VideoSound
{
    IDirectSound8* ds = nullptr;  // nullptr: Bink opens the default device itself
    bool mute = false;
    int openVideos = 0;
    std::vector<IDirectSound8*> retired;

    void Retire()
    {
        if (ds) retired.push_back(ds);
        ds = nullptr;
        ReleaseRetired();
    }
    void ReleaseRetired()
    {
        if (openVideos > 0) return;
        for (IDirectSound8* r : retired) r->Release();
        retired.clear();
    }
};
static VideoSound g_videoSound;
static float VideoGain();  // after the settings

// Movie player: mirrors Driv3r's Bink wrapper (open @ 0x599590, per-frame
// update @ 0x599440).
class Movie
{
public:
    ~Movie() { Close(); }

    bool Open(const std::string& path, uint32_t langTrack, bool loop)
    {
        Close();
        // Open once to count audio tracks, fall back to track 0 if the
        // language track is missing, then reopen with that track.
        g_bink.SetSoundSystem(reinterpret_cast<void*>(g_bink.OpenDirectSound),
                              uint32_t(reinterpret_cast<uintptr_t>(g_videoSound.ds)));
        HBINK probe = g_bink.Open(path.c_str(), 0);
        if (!probe) { Log("Movie: cannot open %s", path.c_str()); return false; }
        uint32_t tracks = *reinterpret_cast<uint32_t*>(static_cast<char*>(probe) + BINK_NUMTRACKS);
        uint32_t track = tracks > langTrack ? langTrack : 0;
        g_bink.Close(probe);
        g_bink.SetSoundTrack(g_videoSound.mute ? 0 : 1, &track);  // 0 tracks: no sound
        h_ = g_bink.Open(path.c_str(), BINKSNDTRACK);
        if (!h_) h_ = g_bink.Open(path.c_str(), 0);
        if (!h_) return false;
        ++g_videoSound.openVideos;
        // 0x59962b: volume = category 3 (overall) x category 0 (effects)
        // x 32768, truncated, on the track index; set once per video. A
        // category reads 0 with no sound device (0x596e50).
        g_bink.SetVolume(h_, track, int32_t(VideoGain() * 32768.0f));

        const BinkHeader* hdr = static_cast<const BinkHeader*>(h_);
        w_ = hdr->Width;
        hgt_ = hdr->Height;
        px_.assign(size_t(w_) * hgt_, 0);
        loop_ = loop;
        done_ = false;
        first_ = true;
        path_ = path;
        Log("Movie: %s %ux%u, %u frames, audio track %u/%u%s%s", path.c_str(), w_, hgt_,
            hdr->Frames, track, tracks, loop ? " (loop)" : "", g_videoSound.mute ? ", no sound" : "");
        return true;
    }

    void Close()
    {
        if (h_) {
            g_bink.Close(h_);
            --g_videoSound.openVideos;
            g_videoSound.ReleaseRetired();
        }
        h_ = nullptr;
        path_.clear();
    }

    // Returns true when a new frame was decoded.
    bool Update()
    {
        if (!h_ || done_) return false;
        if (g_bink.Wait(h_)) return false;
        g_bink.DoFrame(h_);
        g_bink.CopyToBuffer(h_, px_.data(), int(w_ * 4), hgt_, 0, 0,
                            BINKSURFACE32 | (first_ ? BINKCOPYALL : 0));
        first_ = false;
        const BinkHeader* hdr = static_cast<const BinkHeader*>(h_);
        if (hdr->FrameNum == hdr->Frames && !loop_) { done_ = true; return true; }
        g_bink.NextFrame(h_);  // wraps to frame 1 at the end when looping
        return true;
    }

    // Stretched into (x,y,w,h), or letterboxed into it when 'fit' is set.
    void Draw(HDC dc, int x, int y, int w, int h, bool fit) const
    {
        if (!h_ || px_.empty()) return;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = LONG(w_);
        bi.bmiHeader.biHeight = -LONG(hgt_);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        if (fit) {
            double s = std::min(double(w) / w_, double(h) / hgt_);
            int fw = int(w_ * s), fh = int(hgt_ * s);
            x += (w - fw) / 2;
            y += (h - fh) / 2;
            w = fw;
            h = fh;
        }
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, x, y, w, h, 0, 0, w_, hgt_, px_.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        GdiFlush();  // the canvas is drawn into the same DIB right after
    }

    bool IsOpen() const { return h_ != nullptr; }
    bool Done() const { return done_; }
    // Frame being shown (1-based, Bink FrameNum); 0 when closed.
    uint32_t Frame() const { return h_ ? static_cast<const BinkHeader*>(h_)->FrameNum : 0; }
    const std::string& Path() const { return path_; }

private:
    HBINK h_ = nullptr;
    uint32_t w_ = 0, hgt_ = 0;
    bool loop_ = false, done_ = false, first_ = false;
    std::string path_;
    std::vector<uint32_t> px_;
};

// ============================================================ game data ====
static bool FileExists(const std::string& p)
{
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Languages offered on the EU page of GUI\LANGUAGE (event handler 0x556770).
struct Language { const char* folder; uint32_t id; const char* event; const char* stat; };
static const Language kLanguages[] = {
    { "English", 0, "lang_eng", "english" }, { "French", 1, "lang_fre", "french" },
    { "German", 2, "lang_ger", "german" },   { "Italian", 4, "lang_ita", "italian" },
    { "Spanish", 3, "lang_spa", "spanish" },
};
static const int kLanguageCount = int(sizeof(kLanguages) / sizeof(kLanguages[0]));

static int LanguageIndex(uint32_t id)
{
    for (int i = 0; i < kLanguageCount; ++i)
        if (kLanguages[i].id == id) return i;
    return -1;
}

using TextTable = std::map<int, std::wstring>;

// Parses the UTF-16 locale files: <ID>00001</ID><TEXT>...</TEXT>
static TextTable LoadTxt(const std::string& path)
{
    TextTable t;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { Log("Text: cannot open %s", path.c_str()); return t; }
    std::wstring s;
    wchar_t c;
    while (fread(&c, sizeof(c), 1, f) == 1) s += c;
    fclose(f);
    size_t p = 0;
    while ((p = s.find(L"<ID>", p)) != std::wstring::npos) {
        int id = _wtoi(s.c_str() + p + 4);
        size_t a = s.find(L"<TEXT>", p), b = s.find(L"</TEXT>", a);
        if (a == std::wstring::npos || b == std::wstring::npos) break;
        t[id] = s.substr(a + 6, b - a - 6);
        p = b;
    }
    return t;
}

static bool StartsWithI(const std::string& s, const char* prefix)
{
    return _strnicmp(s.c_str(), prefix, strlen(prefix)) == 0;
}

// ============================================================ key map =======
// Driv3r keeps the key map in one global table (0x7a4e50: 50 records of 0x34
// bytes, used through the map object 0x8ac590, vtbl 0x789a20). The settings
// object keeps a copy at +0x208 ("keymap_changed", 0x5495f5), so it is saved
// in ProfileData. The defaults are the table at 0x7a4218, filled by the static
// initialiser 0x6dc680 and copied back by "keymap_reset" (0x54a321).
struct KeyBind  // one direction of an action
{
    int32_t key;      // keyboard: DirectInput code (DIK_*), 0x4000 = none
    int32_t altAxis;  // 1: "alt" is a joystick axis index (dir = sign)
    int32_t alt;      // 0x1000.. joystick POV / button, 0x2000.. mouse button, 0x4000 none
    uint8_t dir, pad[3];  // pad keeps the game's bytes as they are
};
struct KeyRecord
{
    int32_t group;  // 0 misc, 1 camera, 2 vehicle, 3 character (keymap_start1..4)
    int32_t id;     // game control id (not used by the menu)
    int32_t axis;   // 1: two rows, one per direction (bind[0], bind[1])
    KeyBind bind[2];
    int32_t text[2];  // row names in Text\controls.txt (-1: no second row)
};
static_assert(sizeof(KeyRecord) == 0x34, "key map record size");
static const int kKeyRecords = 50;
static const KeyRecord kDefaultKeyMap[kKeyRecords] = {
    { 0, 0x21, 0, { { 0x39, 0, 0x4000, 0 }, { 0x39, 0, 0x4000, 0 } }, { 0, -1 } },
    { 0, 0x20, 0, { { 0x52, 0, 0x1008, 0 }, { 0x52, 0, 0x1008, 0 } }, { 0x41, -1 } },
    { 0, 0x14, 0, { { 0x2e, 0, 0x4000, 0 }, { 0x2e, 0, 0x4000, 0 } }, { 0x27, -1 } },
    { 0, 0xa, 0, { { 0x47, 1, 3, 0 }, { 0x47, 1, 3, 0 } }, { 2, -1 } },
    { 0, 0xb, 0, { { 0x4f, 1, 3, 1 }, { 0x4f, 1, 3, 1 } }, { 3, -1 } },
    { 0, 0xc, 0, { { 0x4b, 1, 0, 0 }, { 0x4b, 1, 0, 0 } }, { 4, -1 } },
    { 0, 0xd, 0, { { 0x4d, 1, 0, 1 }, { 0x4d, 1, 0, 1 } }, { 5, -1 } },
    { 0, 0xe, 0, { { 0x48, 1, 1, 1 }, { 0x48, 1, 1, 1 } }, { 6, -1 } },
    { 0, 0xf, 0, { { 0x50, 1, 1, 0 }, { 0x50, 1, 1, 0 } }, { 7, -1 } },
    { 0, 0x10, 0, { { 0x49, 1, 2, 0 }, { 0x49, 1, 2, 0 } }, { 8, -1 } },
    { 0, 0x11, 0, { { 0x51, 1, 2, 1 }, { 0x51, 1, 2, 1 } }, { 9, -1 } },
    { 0, 9, 0, { { 0x52, 0, 0x1006, 0 }, { 0x52, 0, 0x1006, 0 } }, { 0xa, -1 } },
    { 0, 0x12, 0, { { 0x4c, 0, 0x1007, 0 }, { 0x4c, 0, 0x1007, 0 } }, { 0xb, -1 } },
    { 1, 0x19, 1, { { 0x1f, 1, 1, 0 }, { 0x11, 1, 1, 1 } }, { 0xc, 0xd } },
    { 1, 0x1a, 1, { { 0x1e, 1, 0, 0 }, { 0x20, 1, 0, 1 } }, { 0xe, 0xf } },
    { 1, 0x1b, 1, { { 0xc9, 1, 5, 0 }, { 0xd1, 1, 5, 1 } }, { 0x10, 0x11 } },
    { 1, 0x1c, 1, { { 0x4b, 1, 2, 0 }, { 0x4d, 1, 2, 1 } }, { 0x12, 0x13 } },
    { 1, 0x1d, 1, { { 0x48, 1, 4, 0 }, { 0x50, 1, 4, 1 } }, { 0x14, 0x15 } },
    { 1, 0x18, 0, { { 0x1d, 0, 0x4000, 0 }, { 0x1d, 0, 0x4000, 0 } }, { 0x16, -1 } },
    { 2, 0x1e, 1, { { 0x4000, 1, 0, 0 }, { 0x4000, 1, 0, 1 } }, { 0x19, 0x1a } },
    { 2, 0x3c, 0, { { 0x2c, 0, 0x1002, 0 }, { 0x2c, 0, 0x1002, 0 } }, { 0x1b, -1 } },
    { 2, 0x3d, 0, { { 0x2d, 0, 0x1003, 0 }, { 0x2d, 0, 0x1003, 0 } }, { 0x1c, -1 } },
    { 2, 0x1f, 1, { { 0x4000, 1, 1, 0 }, { 0x4000, 1, 1, 1 } }, { 0x1d, 0x1e } },
    { 2, 0x3e, 0, { { 0x21, 0, 0x1000, 0 }, { 0x21, 0, 0x1000, 0 } }, { 0x1f, -1 } },
    { 2, 0x3f, 0, { { 0x2f, 0, 0x1001, 0 }, { 0x2f, 0, 0x1001, 0 } }, { 0x20, -1 } },
    { 2, 0x22, 0, { { 0x28, 0, 0x1004, 0 }, { 0x28, 0, 0x1004, 0 } }, { 0x21, -1 } },
    { 2, 0x23, 0, { { 0x2b, 0, 0x1006, 0 }, { 0x2b, 0, 0x1006, 0 } }, { 0x22, -1 } },
    { 2, 0x24, 0, { { 0x39, 0, 0x1007, 0 }, { 0x39, 0, 0x1007, 0 } }, { 0x23, -1 } },
    { 2, 0x25, 0, { { 0x35, 0, 0x1005, 0 }, { 0x35, 0, 0x1005, 0 } }, { 0x24, -1 } },
    { 2, 0x26, 0, { { 0x12, 0, 0x1009, 0 }, { 0x12, 0, 0x1009, 0 } }, { 0x25, -1 } },
    { 2, 0x27, 0, { { 0x23, 0, 0x100a, 0 }, { 0x23, 0, 0x100a, 0 } }, { 0x26, -1 } },
    { 2, 0x15, 0, { { 0x14, 0, 0x4000, 0 }, { 0x14, 0, 0x4000, 0 } }, { 0x28, -1 } },
    { 2, 0x16, 1, { { 0x1e, 1, 2, 0 }, { 0x20, 1, 2, 1 } }, { 0x29, 0x2a } },
    { 2, 0x17, 1, { { 0x11, 1, 3, 0 }, { 0x1f, 1, 3, 1 } }, { 0x2c, 0x2b } },
    { 2, 0x37, 0, { { 0xc8, 1, 1, 1 }, { 0xc8, 1, 1, 1 } }, { 0x2d, -1 } },
    { 2, 0x38, 0, { { 0xd0, 1, 1, 0 }, { 0xd0, 1, 1, 0 } }, { 0x2e, -1 } },
    { 2, 0x39, 0, { { 0xcb, 1, 0, 0 }, { 0xcb, 1, 0, 0 } }, { 0x2f, -1 } },
    { 2, 0x3a, 0, { { 0xcd, 1, 0, 1 }, { 0xcd, 1, 0, 1 } }, { 0x30, -1 } },
    { 2, 0x3b, 0, { { 0x1c, 0, 0x1004, 1 }, { 0x1c, 0, 0x1004, 1 } }, { 0x31, -1 } },
    { 3, 0x29, 1, { { 0x1f, 1, 1, 0 }, { 0x11, 1, 1, 1 } }, { 0x33, 0x32 } },
    { 3, 0x2a, 1, { { 0x1e, 1, 0, 0 }, { 0x20, 1, 0, 1 } }, { 0x34, 0x35 } },
    { 3, 0x2b, 0, { { 0x12, 0, 0x1009, 0 }, { 0x12, 0, 0x1009, 0 } }, { 0x36, -1 } },
    { 3, 0x2c, 1, { { 0x4b, 1, 2, 0 }, { 0x4d, 1, 2, 1 } }, { 0x37, 0x38 } },
    { 3, 0x2d, 1, { { 0x48, 1, 5, 0 }, { 0x50, 1, 5, 1 } }, { 0x39, 0x3a } },
    { 3, 0x30, 0, { { 0xf, 0, 0x100a, 0 }, { 0xf, 0, 0x100a, 0 } }, { 0x3b, -1 } },
    { 3, 0x31, 0, { { 0x4c, 0, 0x100b, 0 }, { 0x4c, 0, 0x100b, 0 } }, { 0x3c, -1 } },
    { 3, 0x32, 0, { { 0x10, 0, 0x100c, 0 }, { 0x10, 0, 0x100c, 0 } }, { 0x3d, -1 } },
    { 3, 0x33, 0, { { 0x13, 0, 0x100d, 0 }, { 0x13, 0, 0x100d, 0 } }, { 0x3e, -1 } },
    { 3, 0x34, 0, { { 0x39, 0, 0x100e, 0 }, { 0x39, 0, 0x100e, 0 } }, { 0x3f, -1 } },
    { 3, 0x36, 0, { { 0x1d, 0, 0x100f, 0 }, { 0x1d, 0, 0x100f, 0 } }, { 0x40, -1 } },
};

// Rows of a group (map vtbl+0x40, 0x54af20): an axis record gives two rows.
static int KeyRowCount(const KeyRecord* map, int group)
{
    int n = 0;
    for (int i = 0; i < kKeyRecords; ++i)
        if (map[i].group == group) n += map[i].axis == 1 ? 2 : 1;
    return n;
}

// Row -> record and direction (0x54b6b0); nullptr past the end.
static KeyRecord* KeyRow(KeyRecord* map, int group, int row, int& half)
{
    for (int i = 0; i < kKeyRecords; ++i) {
        if (map[i].group != group) continue;
        if (row-- == 0) { half = 0; return &map[i]; }
        if (map[i].axis == 1 && row-- == 0) { half = 1; return &map[i]; }
    }
    return nullptr;
}

static std::wstring TextOf(const std::map<int, std::wstring>& t, int id)
{
    auto it = t.find(id);
    return it == t.end() ? std::wstring() : it->second;
}

// Preferred column (map vtbl+0x64, 0x54b210): keyboard codes get the
// DirectInput key name (GetObjectInfo, 0x583660); here the Windows name of
// the same scan code. 70 "<None>", 83 "<Unknown>" (Text\controls.txt).
static std::wstring KeyName(int code, const std::map<int, std::wstring>& t)
{
    if (code & 0xf000) return TextOf(t, (code & 0xf000) == 0x4000 ? 70 : 83);
    wchar_t buf[64];
    LONG lp = ((code & 0x7f) << 16) | ((code & 0x80) ? 1 << 24 : 0);
    return GetKeyNameTextW(lp, buf, 64) > 0 ? std::wstring(buf) : std::wstring();
}

// Alternative column, buttons (map vtbl+0x68, 0x54b290).
static std::wstring AltName(int code, const std::map<int, std::wstring>& t)
{
    int kind = code & 0xf000, i = code - 0x1000;
    if (kind == 0x1000) {
        if (i >= 0 && i <= 3) return TextOf(t, 71) + TextOf(t, 75 + i);  // "Joy POV1 " + Up/Down/Left/Right
        if (i >= 4 && i <= 0x23) return TextOf(t, 79) + std::to_wstring(code - 0x1003);  // "Joy But n"
        if (i >= 0x24 && i <= 0x2f) return TextOf(t, 72 + (i - 0x24) / 4) + TextOf(t, 75 + (i - 0x24) % 4);
        return TextOf(t, 83);
    }
    if (kind == 0x2000 && code <= 0x2007) return TextOf(t, 82) + std::to_wstring(code - 0x1fff);  // "Mouse But n"
    return TextOf(t, kind == 0x4000 ? 70 : 83);
}

// Alternative column, axes (map vtbl+0x6c, 0x54b580): "Joy X Axis" + " -"/" +".
static std::wstring AxisName(int axis, int dir, const std::map<int, std::wstring>& t)
{
    if (axis >= 0 && axis <= 7) return TextOf(t, 84 + axis) + TextOf(t, dir ? 81 : 80);
    return TextOf(t, axis == 11 ? 70 : 83);
}

// ======================================================== display modes ====
// Resolution record, 0x54 bytes, as kept in the settings at +0xc34: built by
// 0x54a890 ("%dx%d %dHz" plus " 32bit" / " 16bit" by format, widened to 32
// characters).
struct DisplayMode
{
    int32_t index;      // mode index in the adapter's table
    wchar_t name[32];   // txt_displayres shows this (+0xc38)
    uint32_t w, h, hz;  // +0x44
    uint32_t fmt;       // D3DFORMAT; 0 matches any in a search
};
static_assert(sizeof(DisplayMode) == 0x54, "settings +0xc34 record");

static DisplayMode MakeMode(int index, uint32_t w, uint32_t h, uint32_t hz, uint32_t fmt)
{
    DisplayMode m = {};
    m.index = index;
    m.w = w, m.h = h, m.hz = hz, m.fmt = fmt;
    const wchar_t* bits = L"";
    if (fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_A2R10G10B10) bits = L" 32bit";
    else if (fmt == D3DFMT_R5G6B5 || fmt == D3DFMT_X1R5G5B5) bits = L" 16bit";
    swprintf(m.name, 32, L"%ux%u %uHz%ls", w, h, hz, bits);
    return m;
}

// Search 0x54a990: same size and rate, and same format unless the key's is 0.
static int FindMode(const std::vector<DisplayMode>& list, const DisplayMode& k)
{
    for (size_t i = 0; i < list.size(); ++i) {
        const DisplayMode& e = list[i];
        if (e.w == k.w && e.h == k.h && e.hz == k.hz && (e.fmt == k.fmt || k.fmt == 0)) return int(i);
    }
    return -1;
}

// Mode list (settings +0x1928, built by 0x54a500 when the settings object is
// created): the adapter's modes from 640x480 to 2048x1536 whose format can
// hold an A8R8G8B8 render target (0x5c8d00, at most 100), inserted before
// the first entry that is at least as wide, high and fast (0x54a9e0). The
// original reads the modes from its startup device table; here Direct3D 9 is
// asked directly for the 32-bit and 16-bit formats. The same pass checks
// anti-aliasing (0x5c9560): supported when a multisample type of 2 or more
// works, otherwise the front end hides the Anti-Alias row.
static std::vector<DisplayMode> EnumDisplayModes(bool& antialias)
{
    std::vector<DisplayMode> list;
    antialias = false;
    HMODULE dll = LoadLibraryA("d3d9.dll");
    typedef IDirect3D9*(WINAPI * CreateFn)(UINT);
    CreateFn create = dll ? reinterpret_cast<CreateFn>(GetProcAddress(dll, "Direct3DCreate9")) : nullptr;
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) {
        Log("Display: Direct3D 9 not available, no mode list");
        return list;
    }
    const D3DFORMAT formats[] = { D3DFMT_X8R8G8B8, D3DFMT_R5G6B5 };
    int index = 0;
    for (D3DFORMAT f : formats) {
        UINT n = d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT, f);
        for (UINT i = 0; i < n; ++i, ++index) {
            D3DDISPLAYMODE dm;
            if (FAILED(d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, f, i, &dm))) continue;
            if (dm.Width < 640 || dm.Width > 2048 || dm.Height < 480 || dm.Height > 1536) continue;
            if (FAILED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dm.Format, D3DUSAGE_RENDERTARGET,
                                              D3DRTYPE_SURFACE, D3DFMT_A8R8G8B8)))
                continue;
            if (list.size() >= 100) break;
            size_t pos = 0;
            while (pos < list.size() && !(list[pos].w >= dm.Width && list[pos].h >= dm.Height && list[pos].hz >= dm.RefreshRate))
                ++pos;
            list.insert(list.begin() + pos, MakeMode(index, dm.Width, dm.Height, dm.RefreshRate, dm.Format));
        }
    }
    int best = 0;
    for (int t = 0; t <= 16; ++t)
        if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, FALSE,
                                                      D3DMULTISAMPLE_TYPE(t), nullptr)))
            best = t;
    antialias = best >= 2;
    d3d->Release();
    Log("Display: %u modes, anti-aliasing %s", unsigned(list.size()), antialias ? "supported" : "not supported");
    return list;
}

// Default mode 0x54a7a0: 640x480 at 60Hz 32-bit (800x600 when the territory,
// renderer vtbl+0x48, is 2 or 3; Europe is 1), else that size in any format,
// else the first mode.
static DisplayMode DefaultMode(const std::vector<DisplayMode>& list)
{
    DisplayMode k = MakeMode(0, 640, 480, 60, D3DFMT_X8R8G8B8);
    int i = FindMode(list, k);
    if (i < 0) {
        k.fmt = 0;
        i = FindMode(list, k);
    }
    if (i < 0) i = 0;
    return list.empty() ? MakeMode(0, 640, 480, 60, D3DFMT_X8R8G8B8) : list[i];
}

// ======================================================== sound drivers ====
// Sound driver list of the sound system (0x8ac2b0: count +0x74c, entries of
// 0xe8 bytes from +0xc). 0x595f25 passes the callback 0x597ef0 to
// DirectSoundEnumerateA (dsound ordinal 2). For each device Windows reports,
// the callback opens it with DirectSoundCreate8 (ordinal 11), keeps it when
// GetCaps reports 16-bit primary and secondary buffers and mono secondary
// buffers (flags & 0x908), and stores the device description as a wide name
// of 64 characters; at most 8 devices.
static std::vector<GUID> g_soundGuids;  // per driver (GUID_NULL: primary), for the video sound

static BOOL CALLBACK OnSoundDevice(LPGUID guid, LPCSTR desc, LPCSTR, LPVOID ctx)
{
    auto* list = static_cast<std::vector<std::wstring>*>(ctx);
    static HMODULE dll = GetModuleHandleA("dsound.dll");
    typedef HRESULT(WINAPI * CreateFn)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(dll, "DirectSoundCreate8"));
    IDirectSound8* ds = nullptr;
    if (!create || FAILED(create(guid, &ds, nullptr))) return TRUE;
    DSCAPS caps = {};
    caps.dwSize = sizeof(caps);
    bool ok = SUCCEEDED(ds->GetCaps(&caps)) && (caps.dwFlags & 0x908) == 0x908;
    ds->Release();
    if (!ok) return TRUE;
    wchar_t name[64] = {};
    MultiByteToWideChar(CP_ACP, 0, desc, -1, name, 64);
    name[63] = 0;
    list->push_back(name);
    g_soundGuids.push_back(guid ? *guid : GUID_NULL);
    Log("Sound: driver %u = %s", unsigned(list->size() - 1), desc);  // cf. "Found device: " (0x78d708)
    return list->size() < 8;
}

static std::vector<std::wstring> EnumSoundDrivers()
{
    std::vector<std::wstring> list;
    HMODULE dll = LoadLibraryA("dsound.dll");
    typedef HRESULT(WINAPI * EnumFn)(LPDSENUMCALLBACKA, LPVOID);
    auto enumerate = dll ? reinterpret_cast<EnumFn>(GetProcAddress(dll, "DirectSoundEnumerateA")) : nullptr;
    if (enumerate) enumerate(OnSoundDevice, &list);
    Log("Sound: %u drivers", unsigned(list.size()));
    return list;
}

// ====================================================== screens / states ===
// State IDs are the indices in the factory registry FUN_005867f0
// (creator slot = id * 0x14 + 0x10), registered by the static initialisers
// at 0x6df220.. / 0x6dc5b5.
enum StateId
{
    ST_FRONTEND  = 0xAD,  // front end          vtbl 0x78a51c, creator 0x5562c0; the manager
                          // (vtbl 0x78a3bc) creates A9, AD, AC, ... and vtbl+0x24 after
                          // "bootup_over" moves from A9 to the next one, AD
    ST_BOOT_SEQ  = 0xA9,  // sequencer          vtbl 0x78b834, Next() @ 0x5564c0
    ST_INTRO_FMV = 0xAA,  // intro movie        vtbl 0x78b7d8, Enter @ 0x5563a0
    ST_LANGUAGE  = 0xAB,  // GUI\LANGUAGE       vtbl 0x78b878, Enter @ 0x5565f0
    ST_TITLE     = 0xAC,  // GUI\BOOTUP         vtbl 0x7891e8, Update @ 0x541a70
    ST_GAMEPLAY  = 0xB3,  // game load, pushed by FUN_00557d70 -> not implemented
};

class Screen
{
public:
    virtual ~Screen() {}
    virtual void Enter() {}
    virtual void Leave() {}
    virtual void Update() {}
    virtual void Render(HDC, Canvas&) {}
    virtual void OnKey(WPARAM) {}
    // Keyboard key newly pressed (no auto-repeat), as a DirectInput code
    // (DIK_*), like the poll 0x551f50 used for the cheat codes.
    virtual void OnKeyPress(int) {}
    // GUI mouse event (code, pointer as fractions of the client area).
    virtual void OnMouse(int, float, float) {}
    // Mouse button newly pressed, DirectInput order (0 left, 1 right,
    // 2 middle, 3..): the code 0x2000 + n polled by 0x583a00.
    virtual void OnMouseButton(int) {}
    // Joystick: a newly pressed POV direction or button as 0x1000 + n
    // (0x583fb0), or an axis pushed past half way (0x583ff0, dir: positive).
    // Return true when the press is used up (the menu does not get it).
    virtual bool OnJoyButton(int) { return false; }
    virtual bool OnJoyAxis(int, bool) { return false; }
    // Menu navigation from the joystick (GUI inputs 2..7, see
    // DispatchJoystick): the same as the arrow keys, Enter and Escape.
    virtual void OnPad(WPARAM vk) { OnKey(vk); }
};

static const int kWidth = 640, kHeight = 480;  // CreateWindowExA in WinMain (0x576bf0)

struct App
{
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool running = true;
    int mouseX = -1, mouseY = -1;  // last WM_MOUSEMOVE position (0x8ac278 +0xc / +0x10)
    std::string gameDir, territory = "Europe";
    // Languages in use (indices into kLanguages): text = renderer vtbl+0x50 /
    // +0x54 (LANG: files, font), audio = vtbl+0x60 / +0x64 (AUDIO: files, Bink
    // sound track). The settings keep their own copies, +0x2c text and +0x28
    // audio, which the Language screen changes and Accept / Escape apply.
    int lang = 0, audioLang = 0;
    int textSetting = 0, audioSetting = 0;
    Font font;
    HDC backDC = nullptr;
    HBITMAP backBmp = nullptr;
    Canvas canvas;
    std::map<int, Screen*> screens;
    Screen* cur = nullptr;
    std::vector<std::function<void()>> onLanguageChanged;       // text language
    std::vector<std::function<void()>> onAudioLanguageChanged;  // audio language

    // Sound settings (settings object 0x8ac58c: +0x34 overall, +0x38 effects,
    // +0x3c music; 0..100, defaults from 0x5498e0).
    struct Volumes { int overall = 100, effects = 100, music = 60; } vol;
    float EffectsGain() const { return vol.effects * vol.overall / 10000.0f; }
    float MusicGain() const { return vol.music * vol.overall / 10000.0f; }

    // On/off settings by key ("<key>_toggle" / "stat_<key>"); unknown keys read as on.
    std::map<std::string, bool> toggles;
    bool Toggle(const std::string& k) const { auto it = toggles.find(k); return it == toggles.end() || it->second; }

    int turn = 50;  // look sensitivity, +0x17c (0..100, default 0x549880)
    float motionBlur = 0.6f;  // Game screen, +0xc (0.0..0.8, default 0x5497f0)
    KeyRecord keymap[kKeyRecords];
    void ResetKeyMap() { memcpy(keymap, kDefaultKeyMap, sizeof(keymap)); }

    // Display (screens 12 and 29): aspect +0x160 (0 = 4:3, 2 = 16:9), gamma
    // +0xc30, resolution +0xc34, draw distance +0xc88 and filter +0xc90
    // (0..2; defaults 0x549950 / 0x549980). The values are kept and saved;
    // this program does not change the display with them.
    int aspect = 0;
    float gamma = 1.0f;
    DisplayMode res = {};
    int drawdist = 1, filter = 1;
    std::vector<DisplayMode> modes;  // settings +0x1928
    bool antialiasOk = false;        // front end +0x110

    // Sound card (screen 30): driver index +0x14c (-1 = no sound) and its
    // name +0xcc, mixer rate +0x150 (defaults 0x549920: 0, "", 44100).
    int soundDriver = 0;
    std::wstring soundDriverName;
    int mixRate = 44100;
    std::vector<std::wstring> soundDrivers;
    // Driver name, "-" for an index outside the list (0x597d60 / 0x548060).
    std::wstring DriverName(int i) const
    {
        return i >= 0 && i < int(soundDrivers.size()) ? soundDrivers[i] : L"-";
    }

    // "TERR:" / "LANG:" prefixes as used by the game's file layer (0x8ac230 vtbl+0x10).
    std::string Resolve(const std::string& p) const
    {
        if (StartsWithI(p, "TERR:")) return "Territory\\" + territory + "\\" + p.substr(5);
        if (StartsWithI(p, "LANG:"))
            return "Territory\\" + territory + "\\Locale\\" + kLanguages[lang].folder + "\\" + p.substr(5);
        if (StartsWithI(p, "AUDIO:"))
            return "Territory\\" + territory + "\\Locale\\" + kLanguages[audioLang].folder + "\\" + p.substr(6);
        return p;
    }

    uint32_t AudioTrack() const
    {
        // Driv3r maps the audio language (vtbl+0x60) -> Bink sound track 0..4
        // (0x599520), others use 0.
        uint32_t id = kLanguages[audioLang].id;
        return id <= 4 ? id : 0;
    }

    void SetTextLanguage(int i)
    {
        i = (i % kLanguageCount + kLanguageCount) % kLanguageCount;
        if (i == lang && font.Loaded()) return;
        lang = i;
        font.Load(Resolve("LANG:FONTS\\FONT.BNK"));  // FUN_00467ca0 loads it per language
        for (auto& f : onLanguageChanged) f();
        Log("Text language: %s", kLanguages[lang].folder);
    }

    void SetAudioLanguage(int i)
    {
        i = (i % kLanguageCount + kLanguageCount) % kLanguageCount;
        if (i == audioLang && audioSet_) return;
        audioLang = i;
        audioSet_ = true;
        for (auto& f : onAudioLanguageChanged) f();
        Log("Audio language: %s", kLanguages[audioLang].folder);
    }

    // Language screen (0x556900): text and audio (when available) both
    // follow the choice; the settings start from these (0x549a6e).
    void SetLanguage(int i)
    {
        SetTextLanguage(i);
        SetAudioLanguage(i);
        textSetting = audioSetting = lang;
    }

    bool audioSet_ = false;

    void Goto(int id)
    {
        Log("State -> 0x%02X", id);
        if (cur) cur->Leave();
        cur = screens[id];
        if (cur) cur->Enter();
    }

    void Quit()
    {
        running = false;
        if (hwnd) DestroyWindow(hwnd);
    }
};
static App g;

// Video volume (0x59962b): categories 3 (overall) and 0 (effects), each
// clamped to 0..1 by 0x596e70; 0 with no sound device (0x596e50).
static float VideoGain()
{
    if (g.soundDriver < 0) return 0.0f;
    auto cat = [](int v) { return std::min(1.0f, std::max(0.0f, v * 0.01f)); };
    return cat(g.vol.overall) * cat(g.vol.effects);
}

// Renderer vtbl+0x48. The order is taken from the language pages
// (0 US, 1 EU, 2 JP, other AS, 0x556660); only Europe ships here.
static int TerritoryId()
{
    if (_stricmp(g.territory.c_str(), "Usa") == 0 || _stricmp(g.territory.c_str(), "America") == 0) return 0;
    if (_stricmp(g.territory.c_str(), "Europe") == 0) return 1;
    if (_stricmp(g.territory.c_str(), "Japan") == 0) return 2;
    return 3;
}

// A language is offered when its Locale folder exists. The original asks the
// renderer (vtbl+0x5c text, +0x6c audio), whose list was not traced.
static bool LanguageAvailable(uint32_t id)
{
    int i = LanguageIndex(id);
    if (i < 0) return false;
    DWORD a = GetFileAttributesA(("Territory\\" + g.territory + "\\Locale\\" + kLanguages[i].folder).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Next / previous language id (0x5496c0 / 0x549760): fixed orders through
// the ids 0..9 (jump tables 0x549730 / 0x5497c8; ids past them go to 0),
// skipping languages that are not available.
static int StepLanguage(int index, bool next)
{
    static const uint32_t kNext[] = { 1, 2, 3, 4, 5, 6, 8, 9, 7 };
    static const uint32_t kPrev[] = { 9, 0, 1, 2, 3, 4, 5, 8, 6, 8 };
    uint32_t id = kLanguages[index].id;
    for (int n = 0; n < 16; ++n) {
        id = next ? (id < 9 ? kNext[id] : 0) : (id < 10 ? kPrev[id] : 0);
        if (LanguageAvailable(id)) return LanguageIndex(id);
    }
    return index;
}

static bool IsSkipKey(WPARAM vk) { return vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_SPACE; }

// ============================================================ joystick =====
// DirectInput game controllers (input object 0x8ac2b8, joystick part at
// +0x24c). Init 0x583a40: EnumDevices(game controllers, attached only)
// creates up to 8 devices (callback 0x584550); each gets the joystick data
// format, the window's cooperative level, a range of -1000..1000 on every
// axis (0x5845c0) and its instance name (0x5841c0: 64 wide characters at +0x8
// of its 0x90-byte slot). Only the selected device (+0x480, set by Controller
// Setup through 0x5522b0, 0 at start) is read. Force feedback (vibration) is
// not implemented.
class Joystick
{
public:
    static const int kCodes = 48;  // codes 0x1000..0x102f

    ~Joystick()
    {
        for (Device& d : devs_) { d.dev->Unacquire(); d.dev->Release(); }
        if (di_) di_->Release();
    }

    void Init(HWND hwnd)
    {
        if (FAILED(DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                                      reinterpret_cast<void**>(&di_), nullptr))) {
            Log("Joystick: DirectInput8Create failed");
            return;
        }
        di_->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevice, this, DIEDFL_ATTACHEDONLY);
        for (Device& d : devs_) {
            d.dev->SetDataFormat(&c_dfDIJoystick2);
            d.dev->SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
            d.dev->EnumObjects(SetAxisRange, d.dev, DIDFT_AXIS);
        }
        Log("Joystick: %d device(s)", Count());
    }

    int Count() const { return int(devs_.size()); }
    // txt_controller (0x5522d0): the slot's name; slots past the count are empty.
    std::wstring Name(int i) const { return i >= 0 && i < Count() ? devs_[i].name : std::wstring(); }
    void Select(int i) { if (i >= 0 && i < 8) sel_ = i; }  // 0x5522b0

    // 0x582670 polls at most every 8 ms; 0x583c10 then reads the selected
    // device. Codes: 0..3 POV 1 up / down / left / right (only the exact
    // angles 0, 18000, 27000, 9000), POV 2..4 at 36..47, then buttons 0..43
    // from code 4 (written last, so they cover POV 2..4). Axes: x, y, z,
    // rx, ry, rz, slider 0, slider 1 times 0.001 (y and the sliders -0.001).
    // A failed Poll acquires the device and leaves everything released.
    // Returns true when new data was read.
    bool Poll()
    {
        DWORD now = GetTickCount();
        if (now - last_ < 8) return false;
        last_ = now;
        memcpy(prev_, cur_, sizeof(cur_));
        memcpy(prevAxes_, axes_, sizeof(axes_));
        if (sel_ >= Count()) return false;
        memset(cur_, 0, sizeof(cur_));
        memset(axes_, 0, sizeof(axes_));
        IDirectInputDevice8A* dev = devs_[sel_].dev;
        DIJOYSTATE2 st;
        if (FAILED(dev->Poll())) { dev->Acquire(); return true; }
        if (FAILED(dev->GetDeviceState(sizeof(st), &st))) return true;
        for (int p = 0; p < 4; ++p) {
            DWORD a = st.rgdwPOV[p];
            uint8_t* c = cur_ + (p ? 32 + 4 * p : 0);
            c[0] = a == 0;
            c[1] = a == 18000;
            c[2] = a == 27000;
            c[3] = a == 9000;
        }
        for (int i = 4; i < kCodes; ++i) cur_[i] = (st.rgbButtons[i - 4] & 0x80) != 0;
        const LONG v[8] = { st.lX, st.lY, st.lZ, st.lRx, st.lRy, st.lRz, st.rglSlider[0], st.rglSlider[1] };
        for (int i = 0; i < 8; ++i) axes_[i] = float(v[i]) * (i == 1 || i >= 6 ? -0.001f : 0.001f);
        return true;
    }

    bool Down(int i) const { return cur_[i] != 0; }
    bool WasDown(int i) const { return prev_[i] != 0; }
    // 0x583fb0: the first code down now and not in the previous poll.
    bool NewButton(int& code) const
    {
        for (int i = 0; i < kCodes; ++i)
            if (cur_[i] && !prev_[i]) { code = 0x1000 + i; return true; }
        return false;
    }
    // 0x583ff0: the first axis at |v| >= 0.5 now and below 0.5 before;
    // dir is true for a positive value.
    bool NewAxis(int& axis, bool& dir) const
    {
        for (int i = 0; i < 8; ++i)
            if (fabsf(axes_[i]) >= 0.5f && fabsf(prevAxes_[i]) < 0.5f) {
                axis = i;
                dir = axes_[i] > 0.0f;
                return true;
            }
        return false;
    }

private:
    struct Device { IDirectInputDevice8A* dev; std::wstring name; };

    static BOOL CALLBACK EnumDevice(LPCDIDEVICEINSTANCEA inst, void* ctx)
    {
        Joystick* self = static_cast<Joystick*>(ctx);
        IDirectInputDevice8A* dev = nullptr;
        if (FAILED(self->di_->CreateDevice(inst->guidInstance, &dev, nullptr))) return DIENUM_CONTINUE;
        wchar_t name[64] = {};
        MultiByteToWideChar(CP_ACP, 0, inst->tszInstanceName, -1, name, 64);
        name[63] = 0;
        self->devs_.push_back({ dev, name });
        Log("Joystick %d: %ls", int(self->devs_.size()) - 1, name);
        return self->devs_.size() < 8 ? DIENUM_CONTINUE : DIENUM_STOP;
    }
    static BOOL CALLBACK SetAxisRange(LPCDIDEVICEOBJECTINSTANCEA obj, void* ctx)
    {
        DIPROPRANGE r = {};
        r.diph.dwSize = sizeof(r);
        r.diph.dwHeaderSize = sizeof(r.diph);
        r.diph.dwObj = obj->dwType;
        r.diph.dwHow = DIPH_BYID;
        r.lMin = -1000;
        r.lMax = 1000;
        return SUCCEEDED(static_cast<IDirectInputDevice8A*>(ctx)->SetProperty(DIPROP_RANGE, &r.diph))
                   ? DIENUM_CONTINUE : DIENUM_STOP;
    }

    IDirectInput8A* di_ = nullptr;
    std::vector<Device> devs_;
    int sel_ = 0;
    DWORD last_ = 0;
    uint8_t cur_[kCodes] = {}, prev_[kCodes] = {};
    float axes_[8] = {}, prevAxes_[8] = {};
};
static Joystick g_joy;

// Skip Sequence (key-map control 0x21, read through scheme 0x162 by the
// movie player 0x558260): its first alternative may be a joystick code.
// By default it has none, so the joystick does not skip videos.
static bool IsSkipJoy(int code)
{
    for (const KeyRecord& r : g.keymap)
        if (r.id == 0x21) return !r.bind[0].altAxis && r.bind[0].alt == code;
    return false;
}

// ============================================================ SOUND.GSD ====
// AUDIO:SOUNDS\SOUND.GSD: "01KB", u32 bankCount, u32 bankOffset[bankCount].
// Bank: u32 count, u32 dataOffset, u32 dataSize, then count x 20-byte records
// {u32 offset; u32 size; u16 rate; u8 flags; u8; i32 loop; u8; u8; u16} and
// raw 16-bit PCM. Short samples (menu sounds) have flags bit 7 clear, mono
// data at bank + dataOffset + offset and loop = -1. The front-end music
// (bank 59) has flags 0x81, loop 0 and offset == dataOffset: its data is
// interleaved stereo at bank + offset (inferred from the data itself).
struct GsdSample
{
    std::vector<uint8_t> pcm;
    uint32_t rate = 0;
    int channels = 1;
    bool loop = false;
};

static bool LoadGsdSample(int bank, int sample, GsdSample& out)
{
    std::string gsd = g.Resolve("AUDIO:SOUNDS\\SOUND.GSD");
    FILE* f = fopen(gsd.c_str(), "rb");
    if (!f) { Log("Sound: cannot open %s", gsd.c_str()); return false; }
    char magic[4] = {};
    uint32_t banks = 0, bankOff = 0, count = 0, dataOff = 0, dataSize = 0;
    uint8_t rec[20];
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "01KB", 4) == 0 &&
              fread(&banks, 4, 1, f) == 1 && bank >= 0 && uint32_t(bank) < banks &&
              fseek(f, 8 + bank * 4, SEEK_SET) == 0 && fread(&bankOff, 4, 1, f) == 1 &&
              fseek(f, long(bankOff), SEEK_SET) == 0 && fread(&count, 4, 1, f) == 1 &&
              fread(&dataOff, 4, 1, f) == 1 && fread(&dataSize, 4, 1, f) == 1 &&
              sample >= 0 && uint32_t(sample) < count &&
              fseek(f, long(bankOff + 12 + sample * 20), SEEK_SET) == 0 && fread(rec, 1, 20, f) == 20;
    if (ok) {
        uint32_t off, size;
        uint16_t rate;
        int32_t loop;
        memcpy(&off, rec, 4);
        memcpy(&size, rec + 4, 4);
        memcpy(&rate, rec + 8, 2);
        memcpy(&loop, rec + 12, 4);
        bool stereo = (rec[10] & 0x80) != 0;
        uint32_t start = stereo ? off : dataOff + off;  // relative to the bank
        ok = size != 0 && start >= dataOff && start + size <= dataOff + dataSize;
        if (ok) {
            out.pcm.resize(size);
            ok = fseek(f, long(bankOff + start), SEEK_SET) == 0 && fread(out.pcm.data(), 1, size, f) == size;
            out.rate = rate;
            out.channels = stereo ? 2 : 1;
            out.loop = loop >= 0;
        }
    }
    fclose(f);
    if (!ok) out.pcm.clear();
    return ok;
}

// ========================================================= sound output ====
// Sound card settings in use (sound manager 0x8ac2ac, set by 0x594920 from
// the settings commit 0x548655): driver index (+0x20), mixer rate (+0x24),
// EAX (+0x28). When one of them changes the manager stops everything
// (0x594a90), opens DirectSound again on that driver with the primary
// buffer at the mixer rate (0x597d90) and reloads its banks (0x594ad0);
// driver -1 ("No Sound") opens nothing. Here the sounds go through waveOut:
// the driver is matched to a waveOut device by name (the primary driver is
// the default device), and every sound is resampled to the mixer rate, as
// a primary buffer at that rate would. EAX has no waveOut equivalent.
struct AudioOut
{
    int driver = 0;  // sound driver index of the settings
    UINT device = WAVE_MAPPER;
    bool mute = false;
    uint32_t rate = 44100;
    int generation = 0;  // bumped on every change; players reopen their devices
};
static AudioOut g_audio;

// Linear resampling of 16-bit PCM (interleaved channels).
static std::vector<uint8_t> Resample16(const std::vector<uint8_t>& pcm, int channels, uint32_t from, uint32_t to)
{
    if (from == to || from == 0 || to == 0 || pcm.empty()) return pcm;
    const int16_t* src = reinterpret_cast<const int16_t*>(pcm.data());
    size_t n = pcm.size() / (2 * size_t(channels));
    size_t m = size_t(double(n) * to / from);
    std::vector<uint8_t> out(m * 2 * channels);
    int16_t* dst = reinterpret_cast<int16_t*>(out.data());
    for (size_t i = 0; i < m; ++i) {
        double p = double(i) * from / to;
        size_t a = size_t(p), b = std::min(a + 1, n - 1);
        double t = p - double(a);
        for (int c = 0; c < channels; ++c)
            dst[i * channels + c] = int16_t(src[a * channels + c] * (1 - t) + src[b * channels + c] * t);
    }
    return out;
}

// waveOut device for a DirectSound driver name: the primary driver (index 0,
// "Primary Sound Driver") is the default device; others by name (waveOut
// names are cut to 31 characters).
static UINT WaveDeviceFor(int index, const std::wstring& name)
{
    if (index <= 0) return WAVE_MAPPER;
    UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        WAVEOUTCAPSW caps = {};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        std::wstring p = caps.szPname;
        if (!p.empty() && name.compare(0, p.size(), p) == 0) return i;
    }
    Log("Sound: no waveOut device named like driver %d, using the default", index);
    return WAVE_MAPPER;
}

// ========================================================== menu sounds ====
// Menu sound object (global 0x8ac3b8, vtbl 0x781f60, created by 0x46a6c0).
// Load (0x46a4f0 / 0x46a580): SOUNDS\MENU.DAT = "01SM", u8 bank, u8 sample[3].
// Play (0x46a650): id 0/1/2 -> sample[id] of that bank in AUDIO:SOUNDS\SOUND.GSD,
// volume 1.0, pitch 1.0. The GUI plays id 0 when the focus moves
// (FUN_0046a170), 1 on select (FUN_00469d20 input 6) and 2 on back (input 7);
// the front-end handlers play id 0 when a value changes.
// Voices: the constructor (0x46a730) makes 5 sound-handle slots at +0x10.
// Play takes the first empty slot (0x46a67f); 0x46a300 starts the sample
// there and gives it the end callback 0x4a9a40, which empties the slot when
// the sound stops. With all 5 busy the sound is dropped (no stealing). Each
// voice here is a waveOut device; it is free once its buffer is done.
class MenuSounds
{
public:
    // 0..2 from MENU.DAT; CHEAT is bank 0 sample 3, fixed in the code
    // that accepts a cheat code (0x551ecb).
    enum { MOVE = 0, SELECT = 1, BACK = 2, CHEAT = 3 };
    static const int kVoices = 5;

    ~MenuSounds() { for (Voice& v : voices_) v.Close(); }

    void Load()
    {
        for (Voice& v : voices_) v.Close();  // stop before freeing the samples
        for (auto& p : pcm_) p.clear();
        gen_ = -1;  // resample again

        uint8_t dat[8] = {};
        FILE* f = fopen("SOUNDS\\MENU.DAT", "rb");
        size_t n = f ? fread(dat, 1, sizeof(dat), f) : 0;
        if (f) fclose(f);
        if (n < 8 || memcmp(dat, "01SM", 4) != 0) { Log("Sound: SOUNDS\\MENU.DAT missing or invalid"); return; }
        int bank = int8_t(dat[4]);

        for (int id = 0; id < 3; ++id) {
            int s = int8_t(dat[5 + id]);
            GsdSample smp;
            if (s < 0 || !LoadGsdSample(bank, s, smp)) continue;
            rate_[id] = smp.rate;
            pcm_[id] = std::move(smp.pcm);
            Log("Sound: menu id %d = bank %d sample %d (%u Hz, %u ms)", id, bank, s, rate_[id],
                unsigned(pcm_[id].size() / 2 * 1000 / (rate_[id] ? rate_[id] : 1)));
        }
        GsdSample smp;
        if (LoadGsdSample(0, 3, smp)) {
            rate_[CHEAT] = smp.rate;
            pcm_[CHEAT] = std::move(smp.pcm);
            Log("Sound: cheat = bank 0 sample 3 (%u Hz, %u ms)", rate_[CHEAT],
                unsigned(pcm_[CHEAT].size() / 2 * 1000 / (rate_[CHEAT] ? rate_[CHEAT] : 1)));
        }
    }

    // The effects volume is applied to the voice's copy of the sample (the
    // original sets sound-manager categories 0/1 and 3).
    void Play(int id)
    {
        if (id < 0 || id >= 4 || pcm_[id].empty() || g_audio.mute) return;
        if (gen_ != g_audio.generation) {  // sound card changed: reopen, resample
            for (Voice& v : voices_) v.Close();
            for (int i = 0; i < 4; ++i) out_[i] = Resample16(pcm_[i], 1, rate_[i], g_audio.rate);
            gen_ = g_audio.generation;
        }
        Voice* v = nullptr;
        for (Voice& x : voices_)
            if (!x.Busy()) { v = &x; break; }
        if (!v) { Log("Sound: all %d voices busy, menu id %d dropped", kVoices, id); return; }
        v->Start(out_[id], g_audio.rate, g.EffectsGain());
        if (int n = Busy(); n > 1) Log("Sound: menu id %d on voice %d, %d playing", id, int(v - voices_), n);
    }
    int Busy() const  // voices playing (for the log / tests)
    {
        int n = 0;
        for (const Voice& v : voices_) n += v.Busy();
        return n;
    }

private:
    struct Voice
    {
        HWAVEOUT out = nullptr;
        uint32_t rate = 0;
        WAVEHDR hdr = {};
        std::vector<int16_t> buf;

        bool Busy() const { return out && (hdr.dwFlags & WHDR_PREPARED) && !(hdr.dwFlags & WHDR_DONE); }
        void Start(const std::vector<uint8_t>& pcm, uint32_t r, float gain)
        {
            if (out && (hdr.dwFlags & WHDR_PREPARED)) waveOutUnprepareHeader(out, &hdr, sizeof(hdr));
            if (out && rate != r) Close();
            if (!out) {
                WAVEFORMATEX wf = {};
                wf.wFormatTag = WAVE_FORMAT_PCM;
                wf.nChannels = 1;
                wf.nSamplesPerSec = r;
                wf.wBitsPerSample = 16;
                wf.nBlockAlign = 2;
                wf.nAvgBytesPerSec = r * 2;
                if (waveOutOpen(&out, g_audio.device, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
                    out = nullptr;
                    Log("Sound: waveOutOpen failed");
                    return;
                }
                rate = r;
            }
            const int16_t* src = reinterpret_cast<const int16_t*>(pcm.data());
            buf.resize(pcm.size() / 2);
            for (size_t i = 0; i < buf.size(); ++i) buf[i] = int16_t(src[i] * gain);
            hdr = {};
            hdr.lpData = reinterpret_cast<LPSTR>(buf.data());
            hdr.dwBufferLength = DWORD(buf.size() * 2);
            waveOutPrepareHeader(out, &hdr, sizeof(hdr));
            if (waveOutWrite(out, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) Log("Sound: waveOutWrite failed");
        }
        void Close()
        {
            if (!out) return;
            waveOutReset(out);
            if (hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(out, &hdr, sizeof(hdr));
            waveOutClose(out);
            out = nullptr;
            hdr = {};
        }
    };

    std::vector<uint8_t> pcm_[4];  // original samples, at full volume
    uint32_t rate_[4] = {};
    std::vector<uint8_t> out_[4];  // the same at the mixer rate
    int gen_ = -1;                 // g_audio.generation they were made for
    Voice voices_[kVoices];
};
static MenuSounds g_sounds;

// ====================================================== front-end music ====
// Music object (vtbl 0x789b0c, loaded by 0x54b860 from FUN_00552480, global
// 0x8ac594). Load callback (0x54b8f0): SOUNDS\FEMUSIC.DAT = "01MM", u8 bank[2],
// u8 sample[2]; the file holds 3b 3c 00 00, i.e. bank 59 sample 0 and bank 60
// sample 0. Both banks are reserved (0x54ba00) and both layers are started
// together at volume 1.0, pitch 1.0 (0x551ca0 on front-end start, 0x54b9a0
// restarts them if they stopped). The front-end exit (0x552c6c) releases the
// object, which stops them. Bank 60 is empty in every shipped SOUND.GSD, so
// only bank 59 (~63 s, 44.1 kHz stereo, looping) is heard.
// The music goes through waveOut, like the menu sound voices.
class FrontEndMusic
{
public:
    ~FrontEndMusic() { Stop(); }

    void Load()
    {
        Stop();
        for (auto& l : layers_) l.smp = GsdSample();
        uint8_t dat[8] = {};
        FILE* f = fopen("SOUNDS\\FEMUSIC.DAT", "rb");
        size_t n = f ? fread(dat, 1, sizeof(dat), f) : 0;
        if (f) fclose(f);
        if (n < 8 || memcmp(dat, "01MM", 4) != 0) { Log("Music: SOUNDS\\FEMUSIC.DAT missing or invalid"); return; }
        int bank[2] = { int8_t(dat[4]), int8_t(dat[5]) };
        if (bank[0] < 0 || bank[1] < 0) return;  // 0x54b9a0 needs both
        for (int i = 0; i < 2; ++i) {
            int s = int8_t(dat[6 + i]);
            Layer& l = layers_[i];
            if (!LoadGsdSample(bank[i], s, l.smp)) { Log("Music: bank %d sample %d empty", bank[i], s); continue; }
            Log("Music: layer %d = bank %d sample %d (%u Hz, %d ch, %u s%s)", i, bank[i], s, l.smp.rate,
                l.smp.channels, unsigned(l.smp.pcm.size() / (2 * l.smp.channels) / (l.smp.rate ? l.smp.rate : 1)),
                l.smp.loop ? ", loop" : "");
        }
    }

    void Play()  // starts the layers that are not already playing
    {
        if (g_audio.mute) return;  // "No Sound"
        for (Layer& l : layers_) {
            if (l.out || l.smp.pcm.empty()) continue;
            // The layer at the mixer rate (sound card setting).
            if (l.gen != g_audio.generation) {
                l.base = Resample16(l.smp.pcm, l.smp.channels, l.smp.rate, g_audio.rate);
                l.buf = l.base;
                l.gain = -1;
                l.gen = g_audio.generation;
            }
            Scale(l);
            WAVEFORMATEX wf = {};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = WORD(l.smp.channels);
            wf.nSamplesPerSec = g_audio.rate;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = WORD(wf.nChannels * 2);
            wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
            if (waveOutOpen(&l.out, g_audio.device, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
                Log("Music: waveOutOpen failed");
                l.out = nullptr;
                continue;
            }
            l.hdr = WAVEHDR();
            l.hdr.lpData = reinterpret_cast<LPSTR>(l.buf.data());
            l.hdr.dwBufferLength = DWORD(l.buf.size() / wf.nBlockAlign * wf.nBlockAlign);
            waveOutPrepareHeader(l.out, &l.hdr, sizeof(l.hdr));
            if (l.smp.loop) {
                l.hdr.dwFlags |= WHDR_BEGINLOOP | WHDR_ENDLOOP;
                l.hdr.dwLoops = 0xffffffffu;
            }
            waveOutWrite(l.out, &l.hdr, sizeof(l.hdr));
        }
    }

    // Music volume = sound-manager category 2 (and 0x8ac48c+0x40), scaled by
    // the overall volume (category 3). Applied to the samples: on current
    // Windows waveOutSetVolume sets the volume of the whole process (its
    // audio session), so it also turned down the videos and menu sounds.
    // The looping buffer is rewritten in place while it plays.
    void UpdateVolume()
    {
        for (Layer& l : layers_)
            if (l.out) Scale(l);
    }

    void Stop()
    {
        for (Layer& l : layers_) {
            if (!l.out) continue;
            waveOutReset(l.out);
            waveOutUnprepareHeader(l.out, &l.hdr, sizeof(l.hdr));
            waveOutClose(l.out);
            l.out = nullptr;
        }
    }

private:
    struct Layer
    {
        GsdSample smp;
        std::vector<uint8_t> base;  // smp at the mixer rate
        std::vector<uint8_t> buf;   // base x the music volume (played)
        float gain = -1;            // volume buf was made with
        int gen = -1;               // g_audio.generation base was made for
        HWAVEOUT out = nullptr;
        WAVEHDR hdr = {};
    };
    Layer layers_[2];

    static void Scale(Layer& l)
    {
        float gain = std::min(1.0f, std::max(0.0f, g.MusicGain()));
        if (gain == l.gain || l.buf.size() != l.base.size()) return;
        const int16_t* src = reinterpret_cast<const int16_t*>(l.base.data());
        int16_t* dst = reinterpret_cast<int16_t*>(l.buf.data());
        for (size_t i = 0; i < l.base.size() / 2; ++i) dst[i] = int16_t(src[i] * gain);
        l.gain = gain;
    }

public:
    bool Playing() const { return layers_[0].out || layers_[1].out; }
    bool Loaded() const { return !layers_[0].smp.pcm.empty() || !layers_[1].smp.pcm.empty(); }
};
static FrontEndMusic g_music;

// Sound card settings -> output (0x594920): only when driver, rate or mute
// changed. Playing music restarts on the new device / rate, as the
// original reloads its banks; the menu sounds reopen on their next play.
// DirectSound object for the videos, as the sound system makes it in
// 0x5976b0: created on the driver's GUID, cooperative level 3
// (DSSCL_EXCLUSIVE, the same as priority since DirectX 8) and a primary
// buffer of 16-bit stereo at the mixer rate. driver -1: none, videos muted.
static void OpenVideoSound(int driver, uint32_t rate)
{
    g_videoSound.Retire();
    g_videoSound.mute = driver < 0;
    if (driver < 0 || !g.hwnd) return;
    typedef HRESULT(WINAPI * CreateFn)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
    HMODULE dll = GetModuleHandleA("dsound.dll");
    auto create = dll ? reinterpret_cast<CreateFn>(GetProcAddress(dll, "DirectSoundCreate8")) : nullptr;
    const GUID* guid = size_t(driver) < g_soundGuids.size() && g_soundGuids[driver] != GUID_NULL ? &g_soundGuids[driver] : nullptr;
    IDirectSound8* ds = nullptr;
    if (!create || FAILED(create(guid, &ds, nullptr))) {
        Log("Sound: video DirectSound on driver %d failed, default device", driver);
        return;
    }
    if (FAILED(ds->SetCooperativeLevel(g.hwnd, DSSCL_EXCLUSIVE))) {
        Log("Sound: video SetCooperativeLevel failed, default device");
        ds->Release();
        return;
    }
    DSBUFFERDESC desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    IDirectSoundBuffer* primary = nullptr;
    if (SUCCEEDED(ds->CreateSoundBuffer(&desc, &primary, nullptr))) {
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2;
        wf.nSamplesPerSec = rate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = rate * 4;
        primary->SetFormat(&wf);
        primary->Release();
    }
    g_videoSound.ds = ds;
}

static void ApplySound()
{
    bool mute = g.soundDriver < 0;
    UINT dev = mute ? WAVE_MAPPER : WaveDeviceFor(g.soundDriver, g.soundDriverName);
    uint32_t rate = g.mixRate == 11025 || g.mixRate == 22050 || g.mixRate == 48000 ? uint32_t(g.mixRate) : 44100u;
    if (g.soundDriver == g_audio.driver && dev == g_audio.device && mute == g_audio.mute && rate == g_audio.rate &&
        g_audio.generation > 0)
        return;
    // Playing music moves to the new output; music silenced by "No Sound"
    // starts again when a driver is chosen (it is loaded only in the front end).
    bool music = g_music.Playing() || (g_audio.mute && !mute && g_music.Loaded());
    g_music.Stop();
    g_audio.device = dev;
    g_audio.mute = mute;
    g_audio.rate = rate;
    g_audio.driver = g.soundDriver;
    ++g_audio.generation;
    OpenVideoSound(mute ? -1 : g.soundDriver, rate);
    Log("Sound: driver %d (%s), mixer %u Hz%s", g.soundDriver, dev == WAVE_MAPPER ? "default device" : "by name",
        rate, mute ? ", no sound" : "");
    if (music) g_music.Play();
}

// ============================================================== display ====
// Display settings -> screen (settings commit 0x5483a9 / 0x5483cd): the
// renderer gets the mode (vtbl+0x88: width, height, mode index) and the
// gamma (vtbl+0x70). The original runs fullscreen, so the monitor switches
// to the chosen mode; the 640x480 frame is stretched to it here. Gamma
// (0x5c8a40): device gamma ramp, the same for R, G and B,
// ramp[i] = 65535 x ((i + 1) / 256) ^ (1 / gamma), clamped.
// Code switch (not a game option): true keeps a window of the chosen size,
// without a mode change and without gamma.
static const bool kWindowed = true;

//=============================
//=============================
//=============================
//=============================
//=============================
//=============================
struct DisplayState
{
    bool applied = false, active = true;
    uint32_t w = 0, h = 0, hz = 0, bpp = 0;
    float gamma = 0;
    WORD desktopRamp[3][256];
    bool haveDesktopRamp = false;
};
static DisplayState g_display;

static void SetGamma(float gamma)
{
    HDC dc = GetDC(nullptr);
    if (!g_display.haveDesktopRamp)
        g_display.haveDesktopRamp = GetDeviceGammaRamp(dc, g_display.desktopRamp) != FALSE;
    WORD ramp[3][256];
    for (int i = 0; i < 256; ++i) {
        double v = 65535.0 * std::pow((i + 1) / 256.0, 1.0 / std::max(0.01f, gamma));
        ramp[0][i] = ramp[1][i] = ramp[2][i] = WORD(std::min(65535.0, std::max(0.0, v)));
    }
    if (!SetDeviceGammaRamp(dc, ramp)) Log("Display: SetDeviceGammaRamp failed (gamma %.2f)", gamma);
    ReleaseDC(nullptr, dc);
}

// Desktop mode and gamma back (exit, or switching away from the program).
static void RestoreDisplay()
{
    if (kWindowed || !g_display.applied) return;
    ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
    if (g_display.haveDesktopRamp) {
        HDC dc = GetDC(nullptr);
        SetDeviceGammaRamp(dc, g_display.desktopRamp);
        ReleaseDC(nullptr, dc);
    }
}

// Mode and gamma of the settings; force = set them again (after Alt-Tab).
static void ApplyDisplay(bool force = false)
{
    if (!g.hwnd || g.res.w == 0 || g.res.h == 0) return;
    uint32_t bpp = g.res.fmt == 23 || g.res.fmt == 24 || g.res.fmt == 25 ? 16 : 32;  // R5G6B5, X1R5G5B5, A1R5G5B5
    bool same = g_display.applied && g_display.w == g.res.w && g_display.h == g.res.h && g_display.hz == g.res.hz &&
                g_display.bpp == bpp && g_display.gamma == g.gamma;
    if (same && !force) return;
    if (kWindowed) {
        RECT wa;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        int w = std::min<int>(g.res.w, wa.right - wa.left), h = std::min<int>(g.res.h, wa.bottom - wa.top);
        SetWindowPos(g.hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2,
                     w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        DEVMODEA dm = {};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = g.res.w;
        dm.dmPelsHeight = g.res.h;
        dm.dmBitsPerPel = bpp;
        dm.dmDisplayFrequency = g.res.hz;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
        LONG r = ChangeDisplaySettingsExA(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr);
        if (r != DISP_CHANGE_SUCCESSFUL) {  // try without the refresh rate
            dm.dmFields &= ~DM_DISPLAYFREQUENCY;
            r = ChangeDisplaySettingsExA(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr);
        }
        if (r != DISP_CHANGE_SUCCESSFUL) Log("Display: mode %ux%u %uHz %ubit failed (%ld)", g.res.w, g.res.h, g.res.hz, bpp, r);
        SetWindowPos(g.hwnd, HWND_TOP, 0, 0, int(g.res.w), int(g.res.h), SWP_SHOWWINDOW);
        SetGamma(g.gamma);
    }
    g_display.applied = true;
    g_display.w = g.res.w;
    g_display.h = g.res.h;
    g_display.hz = g.res.hz;
    g_display.bpp = bpp;
    g_display.gamma = g.gamma;
    Log("Display: %ux%u %uHz %ubit, gamma %.2f%s", g.res.w, g.res.h, g.res.hz, bpp, g.gamma,
        kWindowed ? " (window)" : "");
}

// ============================================================ profile ======
// Settings are saved per player in Saves\D3P_<name>\ProfileData: a raw dump
// of the settings object 0x8ac58c from +0x8 (0x324 dwords). 0x548300 copies
// the live block (+0x8) to the saved one (+0xC98) on settings_accept, which
// then autosaves the profile (0x8ac5a0 vtbl+0x18); 0x548840 copies it back
// on settings_restore. Offsets below are object offsets (file offset + 8).
class Profile
{
public:
    static const int kSize = 0x324 * 4;

    Profile() : data_(kSize) {}

    // Block defaults (0x5499e0), run by the settings constructor 0x547e90 on
    // the live and the saved block, so a profile created before any change
    // holds these. Bytes the original leaves unset (heap fill 0xDEC015A1)
    // stay 0 here.
    void SetDefaults()
    {
        std::fill(data_.begin(), data_.end(), uint8_t(0));
        float f = 0.6f;
        Write(0xc, &f, 4);                                     // motion blur
        f = 1.0f;
        Write(0x10, &f, 4);                                    // (not identified)
        data_[0x14 - 8] = 1;                                   // tc_pressure_slomo; +0x15 single press 0
        SetInt(0x20, 1);                                       // furthest mission
        SetInt(0x28, kLanguages[g.audioSetting].id);           // audio language (config vtbl+0x60)
        SetInt(0x2c, kLanguages[g.textSetting].id);            // text language (config vtbl+0x50)
        SetInt(0x30, 2);                                       // config vtbl+0x70 (not traced; 2 on the game)
        SetInt(0x34, 100);                                     // overall, effects, music volume
        SetInt(0x38, 100);
        SetInt(0x3c, 60);
        SetInt(0x150, 44100);                                  // mixer rate; driver +0x14c 0, name +0xcc empty
        f = 0.25f;
        Write(0x164, &f, 4);                                   // dead zone
        data_[0x170 - 8] = 1;                                  // vibration (input vtbl+0x78)
        data_[0x171 - 8] = TerritoryId() == 2 || TerritoryId() == 3;  // subtitles
        // 0x549c90 on +0x174: mph, inverted look, brake, auto weapon on, auto-aim
        // off, look sensitivity 50, every cheat byte 0, +0x1c1 = 1, +0x1d0 = 999.
        for (int off = 0x174; off <= 0x177; ++off) data_[off - 8] = 1;
        SetInt(0x17c, 50);
        data_[0x1c1 - 8] = 1;
        SetInt(0x1d0, 999);
        data_[0x204 - 8] = 1;                                  // full joint physics
        Write(0x208, kDefaultKeyMap, sizeof(kDefaultKeyMap));  // key map (copied from 0x8ac590 vtbl+0x70)
        f = 1.0f;
        Write(0xc30, &f, 4);                                   // gamma; mode +0xc34 is set later (0x547fb0)
        SetInt(0xc88, 1);                                      // draw distance
        for (int off = 0xc8c; off <= 0xc8e; ++off) data_[off - 8] = 1;  // shadows, sync every frame, particles
        SetInt(0xc90, 1);                                      // filter; advanced shaders, anti-alias 0
    }

    // Keeps a copy of the original file next to it before the first write.
    bool Save()
    {
        if (path_.empty()) return false;
        std::string bak = path_ + ".bak", tmp = path_ + ".tmp";
        if (!FileExists(bak)) CopyFileA(path_.c_str(), bak.c_str(), TRUE);
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return false;
        bool ok = fwrite(data_.data(), 1, kSize, f) == size_t(kSize);
        ok = fclose(f) == 0 && ok;
        return ok && MoveFileExA(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    // ---- profile folders (save device 0x8ac2bc, Saves\<prefix><name>) ------
    static std::string SavesDir() { return g.gameDir + "\\Saves\\"; }

    // Load (0x5421f0): the folder must hold ProfileData of exactly 0xc90
    // bytes (0x542460), else 3 ("corrupt"); a read error gives 2. The data
    // goes straight into the live block, so the caller reverts on failure.
    int LoadFolder(const std::string& folder)
    {
        std::string p = SavesDir() + folder + "\\ProfileData";
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a) || a.nFileSizeHigh || a.nFileSizeLow != kSize)
            return 3;
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) return 2;
        std::vector<uint8_t> d(kSize);
        bool ok = fread(d.data(), 1, kSize, f) == size_t(kSize);
        fclose(f);
        if (!ok) return 2;
        data_.swap(d);
        return 1;
    }
    // Save (0x5422f0 -> device vtbl+0x60 = 0x583250): CreateDirectory Saves
    // and Saves\<folder>, then write ProfileData (the live block).
    bool SaveFolder(const std::string& folder)
    {
        CreateDirectoryA(SavesDir().c_str(), nullptr);
        CreateDirectoryA((SavesDir() + folder).c_str(), nullptr);
        std::string keep = path_;
        path_ = SavesDir() + folder + "\\ProfileData";
        bool ok = Save();
        if (!ok) path_ = keep;
        return ok;
    }
    // Delete (0x5423c0 -> device vtbl+0x68 = 0x583370): every file of the
    // folder, then the folder.
    static bool DeleteFolder(const std::string& folder)
    {
        std::string dir = SavesDir() + folder;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((dir + "\\" + fd.cFileName).c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        return RemoveDirectoryA(dir.c_str()) != 0;
    }
    static bool FolderHasProfile(const std::string& folder)
    {
        return FileExists(SavesDir() + folder + "\\ProfileData");
    }
    // The current profile (0x8ac5a0 vtbl+4), where Accept saves the settings.
    void SetCurrent(const std::string& folder) { path_ = SavesDir() + folder + "\\ProfileData"; }
    std::vector<uint8_t> Snapshot() const { return data_; }
    void Restore(const std::vector<uint8_t>& d) { if (d.size() == data_.size()) data_ = d; }

    bool HasFile() const { return !path_.empty(); }
    const std::string& Path() const { return path_; }
    int32_t Int(int off) const { int32_t v; memcpy(&v, &data_[off - 8], 4); return v; }
    void SetInt(int off, int32_t v) { memcpy(&data_[off - 8], &v, 4); }
    bool Flag(int off) const { return data_[off - 8] != 0; }
    void Read(int off, void* p, size_t n) const { memcpy(p, &data_[off - 8], n); }
    void Write(int off, const void* p, size_t n) { memcpy(&data_[off - 8], p, n); }
    // Some flags hold fill bytes (e.g. 0xA1); only rewrite on a real change.
    void SetFlag(int off, bool v) { if (Flag(off) != v) data_[off - 8] = v ? 1 : 0; }

private:
    std::string path_;
    std::vector<uint8_t> data_;
};
static Profile g_profile;

// On/off settings: "<key>_toggle" (settings handler 0x548860) flips the byte,
// "stat_<key>" shows row 0 when it is non-zero (e.g. 0x5553af).
struct SettingFlag { const char* key; int off; };
static const SettingFlag kSettingFlags[] = {
    { "tc_pressure_slomo", 0x14 }, { "eax", 0x154 },           { "vib", 0x170 },
    { "sub", 0x171 },              { "mph", 0x174 },           { "inv", 0x175 },
    { "brake", 0x176 },            { "weapon", 0x177 },        { "autoaim", 0x178 },
    { "allmissions", 0x19f },      { "weapons", 0x1a1 },       { "ammo", 0x1a3 },
    { "invincibility", 0x1a5 },    { "immunity", 0x1a7 },      { "tommy", 0x1a9 },
    { "vehicle", 0x1ab },          { "fulljointphysics", 0x204 }, { "shadows", 0xc8c },
    { "synceveryframe", 0xc8d },   { "particle", 0xc8e },      { "advshaders", 0xc94 },
    { "antialias", 0xc95 },
};

// Saved block -> live settings (startup, and 0x548840 on settings_restore).
static void ApplyProfile()
{
    g.vol.overall = std::max(0, std::min(100, int(g_profile.Int(0x34))));
    g.vol.effects = std::max(0, std::min(100, int(g_profile.Int(0x38))));
    g.vol.music = std::max(0, std::min(100, int(g_profile.Int(0x3c))));
    for (const SettingFlag& f : kSettingFlags) g.toggles[f.key] = g_profile.Flag(f.off);
    g.turn = std::max(0, std::min(100, int(g_profile.Int(0x17c))));
    g_profile.Read(0xc, &g.motionBlur, 4);
    // Key map copy at +0x208; a block without the four groups keeps the current map.
    KeyRecord km[kKeyRecords];
    g_profile.Read(0x208, km, sizeof(km));
    bool valid = true;
    for (int grp = 0; grp < 4; ++grp) valid = valid && KeyRowCount(km, grp) > 0;
    if (valid) memcpy(g.keymap, km, sizeof(km));
    g.aspect = g_profile.Int(0x160);
    g_profile.Read(0xc30, &g.gamma, 4);
    g_profile.Read(0xc34, &g.res, sizeof(g.res));
    g.drawdist = std::max(0, std::min(2, int(g_profile.Int(0xc88))));
    g.soundDriver = g_profile.Int(0x14c);
    g.mixRate = g_profile.Int(0x150);
    wchar_t name[64];
    g_profile.Read(0xcc, name, sizeof(name));
    name[63] = 0;
    g.soundDriverName = name;
    g.filter = std::max(0, std::min(2, int(g_profile.Int(0xc90))));
    // +0x28 audio and +0x2c text language; 0x548300 then sets both in use.
    int audio = LanguageIndex(uint32_t(g_profile.Int(0x28))), text = LanguageIndex(uint32_t(g_profile.Int(0x2c)));
    if (audio >= 0) g.audioSetting = audio;
    if (text >= 0) g.textSetting = text;
    g.SetAudioLanguage(g.audioSetting);
    g.SetTextLanguage(g.textSetting);
    // 0x548840 ends in the commit 0x548300(this, 1), which applies the
    // display and the sound card settings.
    ApplyDisplay();
    ApplySound();
}

// The live settings, kept in g, written into the block as they are. The
// original's settings handlers change the live block directly, so this is
// what a profile save (0x5422f0) or the copy at Profiles start writes.
static void StoreLive()
{
    g_profile.SetInt(0x34, g.vol.overall);
    g_profile.SetInt(0x38, g.vol.effects);
    g_profile.SetInt(0x3c, g.vol.music);
    for (const SettingFlag& f : kSettingFlags) g_profile.SetFlag(f.off, g.Toggle(f.key));
    g_profile.SetInt(0x17c, g.turn);
    g_profile.Write(0xc, &g.motionBlur, 4);
    g_profile.Write(0x208, g.keymap, sizeof(g.keymap));
    g_profile.SetInt(0x160, g.aspect);
    g_profile.Write(0xc30, &g.gamma, 4);
    g_profile.Write(0xc34, &g.res, sizeof(g.res));
    g_profile.SetInt(0xc88, g.drawdist);
    g_profile.SetInt(0xc90, g.filter);
    g_profile.SetInt(0x14c, g.soundDriver);
    g_profile.SetInt(0x150, g.mixRate);
    // The name is copied up to its terminator (0x5420e0); the bytes after it
    // are left as they are.
    wchar_t old[64];
    g_profile.Read(0xcc, old, sizeof(old));
    old[63] = 0;
    if (g.soundDriverName != old)
        g_profile.Write(0xcc, g.soundDriverName.c_str(), (std::min<size_t>(g.soundDriverName.size(), 63) + 1) * 2);
    g_profile.SetInt(0x28, kLanguages[g.audioSetting].id);
    g_profile.SetInt(0x2c, kLanguages[g.textSetting].id);
}

// Live settings -> saved block (0x548300), then the profile file (autosave)
// when there is one.
static void SaveProfile(bool toFile = true)
{
    // A resolution missing from the mode list becomes the default (0x548385).
    if (!g.modes.empty() && FindMode(g.modes, g.res) < 0) g.res = DefaultMode(g.modes);
    // Sound driver check on accept (0x54851e): no drivers -> -1; a driver
    // whose name no longer matches is looked up by name (else entry 0); the
    // name is then taken from the list.
    int n = int(g.soundDrivers.size());
    if (n == 0) {
        g.soundDriver = -1;
    } else if (g.soundDriver >= n || g.DriverName(g.soundDriver) != g.soundDriverName) {
        g.soundDriver = 0;
        for (int i = 1; i < n; ++i)
            if (g.soundDrivers[i] == g.soundDriverName) { g.soundDriver = i; break; }
    }
    g.soundDriverName = g.DriverName(g.soundDriver);
    StoreLive();
    // 0x548300 sets the audio (vtbl+0x64) and text (vtbl+0x54) language in
    // use, the display mode and gamma (0x5483b9, 0x5483ce) and the sound
    // card (0x54866e).
    g.SetAudioLanguage(g.audioSetting);
    g.SetTextLanguage(g.textSetting);
    ApplyDisplay();
    ApplySound();
    if (!toFile) return;
    if (!g_profile.HasFile()) { Log("Settings: no profile in Saves\\, not written"); return; }
    bool ok = g_profile.Save();
    Log("Settings: %s %s", ok ? "saved to" : "FAILED to save", g_profile.Path().c_str());
}

// ============================================================ MEC menus ====
// Greedy word wrap into a box width; explicit line breaks are kept. Used by
// the FMV subtitles and by MEC texts wider than their element. The original's
// code was not traced; the making-of subtitle line 0 and the MUMAIN popup
// "ASOffQ" broke at the same words as the original (compared on screen).
static std::vector<std::wstring> WrapText(const std::wstring& s, float width, float sx)
{
    std::vector<std::wstring> lines;
    std::wstring cur, word;
    auto flushWord = [&] {
        if (word.empty()) return;
        std::wstring trial = cur.empty() ? word : cur + L' ' + word;
        if (!cur.empty() && g.font.Width(trial, sx) > width) { lines.push_back(cur); cur = word; }
        else cur = trial;
        word.clear();
    };
    for (wchar_t c : s) {
        if (c == L'\r') continue;
        if (c == L'\n') { flushWord(); lines.push_back(cur); cur.clear(); continue; }
        if (c == L' ') { flushWord(); continue; }
        word += c;
    }
    flushWord();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Runtime for one .MEC file: screen stack, focus, navigation and drawing.
// Mirrors the GUI manager at 0x8ac3b0 (handler vtbl 0x781e48): select links
// push a screen (FUN_00467a00 flag 2), back pops it (flag 8), action strings
// go to the game as events, "stat_*" strings ask the game for a button's
// state (0x8ac3b4 vtbl+0xc, used in FUN_00467020).
class MecMenu
{
public:
    MecFile mec;
    TextTable text;
    std::vector<int> stack;
    std::function<void(const std::string&)> onAction;
    std::function<int(const std::string&, int)> onStatus;  // returns new state
    std::function<void(HDC, int, int, int, int)> drawFmv;
    std::function<bool(const std::string&, std::wstring&)> dynamicText;  // TEXT textFn
    std::function<bool(const std::string&, int&)> valueText;  // "val_*" -> number for "%d"
    bool rootBackFiresExit = false;

    bool Load(const std::string& name)  // e.g. "GUI\\FRONT" -> TERR:%s.MEC + LANG:%s.TXT
    {
        mec = MecFile();
        bool ok = mec.Load(g.Resolve("TERR:" + name + ".MEC"));
        Log("MEC %s: %s, %zu screens", name.c_str(), ok ? "ok" : "FAILED", mec.screens.size());
        name_ = name;
        ReloadText();
        return ok;
    }
    void ReloadText() { text = LoadTxt(g.Resolve("LANG:" + name_ + ".TXT")); }

    MecScreen* Top() { return stack.empty() ? nullptr : &mec.screens[stack.back()]; }
    // Focus back on the top screen's default item (header focus).
    void ResetFocus() { if (MecScreen* s = Top()) SetFocus(*s, s->defaultFocus); }

    void Reset(int screen)
    {
        stack.clear();
        Push(screen);
    }

    void Push(int screen)
    {
        if (screen < 0 || screen >= int(mec.screens.size())) return;
        // Back history (go-to-screen 0x467b70; stack = history + current).
        // A current screen without the +16 byte is kept in the history. One
        // with it is not, and the history's top entry is dropped too unless
        // it is that same screen (0x467bb7..0x467bef): UC_Repl 5 -> popup
        // 7 -> 5 leaves 22 under 5, so Back from 5 goes to 22.
        if (!stack.empty() && mec.screens[stack.back()].overlay) {
            int cur = stack.back();
            stack.pop_back();
            if (!stack.empty() && stack.back() != cur) stack.pop_back();
        }
        stack.push_back(screen);
        MecScreen& s = mec.screens[screen];
        // +81 byte (Main): the history is cleared (0x467c65).
        if (s.clearsHistory) stack.erase(stack.begin(), stack.end() - 1);
        Log("Menu: enter screen %d '%s'", screen, s.name.c_str());
        for (Element& e : s.items) e.state = 1;
        // Go-to-screen 0x467b70 (links, show by name) starts on the default
        // item; Pop leaves the parent's focus as it was (checked on the game:
        // Controller Setup opens on Select Controller every time, Options
        // keeps Settings after backing out of it).
        SetFocus(s, s.defaultFocus);
        Fire(s.onEnter);
    }

    // Show by name (GUI vtbl+0x30): the named screen on top of the stack.
    void PushByName(const char* name)
    {
        for (size_t i = 0; i < mec.screens.size(); ++i)
            if (_stricmp(mec.screens[i].name.c_str(), name) == 0) return Push(int(i));
    }

    void Pop()
    {
        if (stack.size() <= 1) return;
        std::string exitAction = Top()->onExit;
        stack.pop_back();
        Fire(exitAction);
    }

    void OnKey(WPARAM vk)
    {
        MecScreen* s = Top();
        if (!s || s->items.empty()) return;
        if (vk == VK_ESCAPE || vk == VK_BACK) {  // input 7: back sound, then pop
            s->hover = false;
            g_sounds.Play(MenuSounds::BACK);
            // Popups shown by name (MUMAIN, NAME) have no parent: back sends
            // the screen's exit action (mu_cancel, mu_no, mu_exit, ...).
            if (rootBackFiresExit && stack.size() <= 1) { Fire(s->onExit); return; }
            // Back on the root screen (0x46a015, 0x467b4b): the exit action
            // unless "<NoAction>", then the event "exitmenu" (the front end
            // shows QuitCon; seen on the game with the pad's back button).
            if (stack.size() <= 1) {
                if (s->onExit != "<NoAction>") Fire(s->onExit);
                Fire("exitmenu");
                return;
            }
            Pop();
            return;
        }
        int slot = vk == VK_UP ? ACT_UP : vk == VK_DOWN ? ACT_DOWN : vk == VK_LEFT ? ACT_LEFT
                 : vk == VK_RIGHT ? ACT_RIGHT : vk == VK_RETURN ? ACT_SELECT : -1;
        if (slot < 0) return;
        s->hover = false;  // keyboard navigation ends mouse hover (0x46a06d)
        Press(*s, slot);
    }

    // Mouse event from the window procedure (GUI listener 0x467c80): only the
    // latest one is kept until the next update. x, y are fractions of the
    // client area (0x576650 divides by the client size).
    void OnMouse(int code, float x, float y)
    {
        mouse_.pending = true;
        mouse_.code = code;
        mouse_.x = x;
        mouse_.y = y;
    }

    // Once per update (0x467a00 -> 0x469d20). First, while the screen is in
    // hover mode, the focus moves to any item under the last pointer
    // position (no sound). Then the pending event: code 0 (left button
    // down) selects the item under the pointer, 9 / 10 (wheel) send the
    // focused button's action slot 6 / 7, 11 (move) turns hover mode on
    // (0x46a1f0). Other codes are ignored. The event is then cleared (0x467b67).
    void Update()
    {
        MecScreen* s = Top();
        Mouse m = mouse_;
        mouse_.pending = false;
        if (!s || s->items.empty()) return;
        if (s->hover)
            for (int i = 0; i < int(s->items.size()); ++i)
                if (Hit(s->items[i], m.x, m.y)) SetFocus(*s, i);
        if (!m.pending) return;
        switch (m.code) {
        case 0:
            // The original loops over all items and selects every hit one;
            // FRONT.MEC buttons do not overlap, and a select can change the
            // screen here at once, so stop at the first.
            for (int i = 0; i < int(s->items.size()); ++i) {
                if (!Hit(s->items[i], m.x, m.y)) continue;
                SetFocus(*s, i);
                Press(*s, ACT_SELECT);
                break;
            }
            break;
        case 9:
        case 10: {  // button vtbl+0x10 (0x4671c0): +0x180 / +0x1a0, unless "<NoAction>"
            Element& e = s->items[s->focus];
            if (e.type == EL_BUTTON) Fire(e.actions[m.code == 9 ? 6 : 7]);
            break;
        }
        case 11:
            s->hover = true;
            break;
        }
    }

private:
    struct Mouse { bool pending = false; int code = 0; float x = -1, y = -1; };
    Mouse mouse_;

    // Button hit test (vtbl+0x28, 0x467260): enabled flag (+0x7c), strictly
    // inside the button's own rectangle, and not disabled (state 2). Other
    // element types never hit (0x520e80).
    static bool Hit(const Element& e, float x, float y)
    {
        return e.type == EL_BUTTON && e.enabled && e.state != 2 &&
               x > e.x && x < e.x + e.w && y > e.y && y < e.y + e.h;
    }

    // Input on the focused button: a direction or select (input 6).
    void Press(MecScreen& screen, int slot)
    {
        MecScreen* s = &screen;
        Element& e = s->items[s->focus];
        if (e.type != EL_BUTTON) return;
        MecLink link = e.links[slot];
        std::string action = e.actions[slot];
        size_t depth = stack.size();
        if (slot == ACT_SELECT) g_sounds.Play(MenuSounds::SELECT);  // input 6
        if (link.kind == 0) {
            // FUN_0046a170: follow the link, skipping disabled (state 2) items;
            // the move sound plays only if the focus really changed.
            int from = s->focus, to = link.target;
            for (int guard = 0; guard < int(s->items.size()); ++guard) {
                if (to < 0 || to >= int(s->items.size()) || s->items[to].type != EL_BUTTON) break;
                if (s->items[to].state != 2) break;
                MecLink next = s->items[to].links[slot];
                if (next.kind != 0) break;
                to = next.target;
            }
            SetFocus(*s, to);
            if (s->focus != from) g_sounds.Play(MenuSounds::MOVE);
        }
        Fire(action);
        if (stack.size() != depth || Top() != s) return;  // the action changed screens
        if (link.kind == 1) Push(link.target);
    }

public:
    void Render(HDC dc, Canvas& cv)
    {
        if (stack.empty()) return;
        // Only the current screen is drawn (GUI draw 0x4677a0 -> 0x469a40).
        // The header byte kept as "overlay" does not bring the parent in:
        // QuitCon has it and shows alone over the video on the game.
        size_t first = stack.size() - 1;
        // Two passes: the items add sprites to the GUI render list
        // (0x57e2b0) but give their texts to the text manager (0x8ac254),
        // which draws later; so texts are always above sprites, whatever the
        // item order (MUMAIN popups list their panel after the texts).
        // Sprites carry the key (layer << 16) | material (0x466b40) into
        // the render list, which draws them by that key, not in item order:
        // MUMAIN lists draw the panel fill (material 1) under the bullets
        // (6) and dashes (3) that come before it in the file (as observed).
        // Video panes are not in that list; they are drawn first.
        sprites_.clear();
        for (size_t i = first; i < stack.size(); ++i)
            for (Element& e : mec.screens[stack[i]].items) DrawElement(dc, cv, e, 0);
        // The screen's background material (header +8 >= 0) is added to the
        // same render list after the items (0x469a85..0x469bb8, key
        // (slot << 16) | material): it lands above the video panes, which are
        // drawn first (the grid of TAR_Main's picture crosses the preview,
        // whose box is transparent), and below sprites of higher materials.
        // A movie background (-2) is drawn by the owner of the movie.
        // On the game the back arrow (material 0, inside a list) stays above
        // TAR_Main's opaque picture, so the background is drawn under every
        // sprite here (the list's layer byte was not traced yet).
        std::stable_sort(sprites_.begin(), sprites_.end(),
                         [](const Element* a, const Element* b) { return a->material < b->material; });
        int bg = mec.screens[stack.back()].background;
        if (bg >= 0) {
            backdrop_.material = bg;
            sprites_.insert(sprites_.begin(), &backdrop_);
        }
        for (const Element* e : sprites_) {
            if (e->material < 0 || e->material >= int(mec.materials.size())) continue;
            const Texture& t = mec.materials[e->material];
            cv.Draw(t, e->u0, e->v0, e->u1, e->v1, e->x * kWidth, e->y * kHeight, e->w * kWidth, e->h * kHeight,
                    e->rgba, t.additive);
        }
        for (size_t i = first; i < stack.size(); ++i)
            for (Element& e : mec.screens[stack[i]].items) DrawElement(dc, cv, e, 1);
    }

private:
    void Fire(const std::string& a)
    {
        if (!a.empty() && onAction) onAction(a);
    }

    void SetFocus(MecScreen& s, int i)
    {
        if (i < 0 || i >= int(s.items.size())) return;
        if (s.focus < int(s.items.size()) && s.items[s.focus].type == EL_BUTTON) s.items[s.focus].state = 1;
        s.focus = i;
        if (s.items[i].type == EL_BUTTON) s.items[i].state = 0;
    }

    void DrawElement(HDC dc, Canvas& cv, Element& e, int pass)  // pass 0 sprites / video, 1 texts
    {
        float x = e.x * kWidth, y = e.y * kHeight, w = e.w * kWidth, h = e.h * kHeight;
        if (pass != (e.type == EL_TEXT ? 1 : 0) && e.type != EL_BUTTON) return;
        switch (e.type) {
        case EL_SPRITE:
            sprites_.push_back(&e);  // drawn sorted by material after this pass
            break;
        case EL_TEXT: {
            std::wstring s;
            if (e.textFn.empty() || !dynamicText || !dynamicText(e.textFn, s)) {
                auto it = text.find(e.textId);
                if (it == text.end()) break;
                s = it->second;
            }
            int value;
            size_t pct = s.find(L"%d");
            if (pct != std::wstring::npos && !e.colourFn.empty() && valueText && valueText(e.colourFn, value))
                s.replace(pct, 2, std::to_wstring(value));
            if (s.empty() || !g.font.Loaded()) break;
            s = ExpandIcons(s);
            // Compared with the original (aspect 0, 8 texts with scales from
            // 0.4 to 1.25): widths follow sx, heights follow sy x ~0.93
            // (taken as 480/512; the code behind it was not traced), and y
            // is the top of the capitals, as for the subtitles. Text wider
            // than the element wraps inside it, lines LinePitch x sy apart
            // (MUMAIN ASOffQ: 22 and 19 px for sy 0.7 and 0.6, measured).
            // The glyphs go through the same Direct3D sampling as the sprites,
            // 0.5 px lower (measured against the original's screenshots: the
            // vertical offset was the only one left, the same on every text).
            float hs = e.sy * (480.0f / 512.0f);
            float ly = y + 0.5f - g.font.TopOffset() * hs;
            // The text box is kept in whole pixels (int fields read with fild
            // by the line placement 0x575300), so x and the width are cut
            // before the alignment (247.9999 -> 247 moved some centred
            // texts 1 px left in the original).
            float bx = float(int(x)), bw = float(int(w));
            for (const std::wstring& line : WrapText(s, w, e.sx)) {
                float tw = g.font.Width(line, e.sx);
                // 0 left, 1 right (x + w - line width), 2 centre (x + (w -
                // line width) / 2): 0x575305..0x5753d4, the same values as
                // the FMV subtitles (0x5992ac).
                float tx = e.align == 1 ? bx + bw - tw : e.align == 2 ? bx + (bw - tw) / 2 : bx;
                g.font.Draw(cv, line, tx, ly, e.sx, hs, e.rgba);
                ly += g.font.LinePitch() * e.sy;
            }
            break;
        }
        case EL_BUTTON:
            if (pass == 0 && !e.actions[ACT_STATUS].empty() && onStatus) e.state = onStatus(e.actions[ACT_STATUS], e.state);
            if (e.state >= 0 && e.state < int(e.rows.size()))
                for (Element& c : e.rows[e.state]) DrawElement(dc, cv, c, pass);
            break;
        case EL_FMV:
            if (drawFmv) drawFmv(dc, int(x), int(y), int(w), int(h));
            break;
        }
    }

    std::vector<const Element*> sprites_;
    Element backdrop_ = FullScreen();  // the screen background quad (UV 0..1)
    static Element FullScreen()
    {
        Element e;
        e.w = e.h = 1;
        return e;
    }

    // "$^a".."$^d" in the text files select the icon glyphs U+2480..U+2483
    // (down, up, right, left arrows) of FONT.BNK.
    static std::wstring ExpandIcons(const std::wstring& s)
    {
        std::wstring out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == L'$' && i + 2 < s.size() && s[i + 1] == L'^' && s[i + 2] >= L'a' && s[i + 2] <= L'z') {
                out += wchar_t(0x2480 + (s[i + 2] - L'a'));
                i += 2;
            } else {
                out += s[i];
            }
        }
        return out;
    }

    std::string name_;
};

// ================================================ profiles (GUI\MUMAIN) ====
// The "memory unit" classes: an emulation of the Xbox save flow on top of
// the save device 0x8ac2bc (Saves\<folder>\<file>). Three of them belong to
// the menus:
//   BOOT     slot 221, global 0x8ac5a8, vtbl 0x78930c (events 0x543090):
//            run by the title screen after bu_continue;
//   PROFILES slot 223, global 0x8ac5ac, vtbl 0x7894f0 (events 0x545060):
//            Options -> Profiles (event LoadSave -> state 0xB2);
//   REPLAYS  slot 222, global 0x8ac5b0, vtbl 0x7894a8 (events 0x544260):
//            Options -> Replays (LoadReplay -> state 0xB1).
// Each has Start / Busy (one step per update) / Finish, a step number and the
// number of the popup on screen, both kept here with the original values.
// Popups are MUMAIN screens shown by name (GUI slot 6); popup 4 is the name
// entry, the only screen of GUI\NAME (slot 7). Folder names are a prefix
// from LANG:TEXT\generic.txt (text 0 "D3P_", text 1 "D3R_") + the name.
class MemoryUnit
{
public:
    enum Mode { BOOT, PROFILES, REPLAYS };
    bool Active() const { return active_; }
    // Replay folder chosen with Load in REPLAYS mode (empty if none).
    std::string TakeReplay() { std::string r; r.swap(replay_); return r; }

    void Start(Mode m)  // 0x5428b0 / 0x544910 / 0x543df0
    {
        mode_ = m;
        replay_.clear();
        if (!loaded_) {
            mu_.Load("GUI\\MUMAIN");
            name_.Load("GUI\\NAME");
            for (MecMenu* menu : { &mu_, &name_ }) {
                menu->rootBackFiresExit = true;
                menu->onAction = [this](const std::string& a) { Event(a); };
                menu->onStatus = [this](const std::string& s, int st) { return Status(s, st); };
                menu->dynamicText = [this](const std::string& fn, std::wstring& out) { return Text(fn, out); };
            }
            loaded_ = true;
        }
        TextTable generic = LoadTxt(g.Resolve("LANG:Text\\generic.txt"));
        auto it = generic.find(m == REPLAYS ? 1 : 0);
        prefix_ = it != generic.end() ? it->second : (m == REPLAYS ? L"D3R_" : L"D3P_");
        StoreLive();                       // the live block holds the current settings
        backup_ = g_profile.Snapshot();    // live -> saved copy (+0x8 -> +0xc98)
        name58_.clear();
        offset_ = count_ = op_ = 0;
        step_ = 0;
        active_ = true;
        Log("Profiles: start (%s), prefix %ls", m == BOOT ? "boot" : m == PROFILES ? "profiles" : "replays", prefix_.c_str());
        Show(0);
    }

    // Busy (0x542ac0 / 0x544ab0): false once the flow is over.
    bool Update()
    {
        if (!active_) return false;
        (popup_ == 4 ? name_ : mu_).Update();
        DWORD el = GetTickCount() - shownAt_;
        switch (step_) {
        case 0: step_ = 1; break;
        case 1: Enumerate(); step_ = 2; break;
        case 2:
            // Boot: no wait; no profile -> NoSave (popup 1). Others: Checking
            // for at least 40 updates (0x544be1), then LSD1.
            if (mode_ == BOOT) {
                if (count_ == 0) { step_ = 3; Show(1); }
                else { step_ = 4; Show(2); }
            } else if (el >= kCheckingMs) {
                step_ = 4;
                Show(2);
            }
            break;
        case 12: case 13: case 14: Work(el); break;
        case 18: Verify(); break;
        case kExit: active_ = false; Log("Profiles: done"); return false;
        }
        return true;
    }

    void Render(HDC dc, Canvas& cv) { (popup_ == 4 ? name_ : mu_).Render(dc, cv); }
    // An Enter that accepted the name is used up: it must not also reach the
    // popup char_accept opened (FileExists stays up until the next Enter on
    // the original; checked 2026-10-11).
    void OnKey(WPARAM vk)
    {
        if (nameAccepted_) { nameAccepted_ = false; return; }
        (popup_ == 4 ? name_ : mu_).OnKey(vk);
    }
    void OnMouse(int code, float x, float y) { (popup_ == 4 ? name_ : mu_).OnMouse(code, x, y); }

    // Name entry (0x5414c0, step 6): newly pressed keys from the table at
    // 0x7a1f78 (0-9, a-z, space -> '_'); a key that the keyboard layout
    // turns into one letter or digit uses that one. Shift XOR Caps Lock ->
    // upper case. Backspace deletes, Enter / keypad Enter accepts if the name
    // is not blank (then char_accept). At most 16 characters (0x5414a0).
    void OnKeyPress(int dik)
    {
        nameAccepted_ = false;
        if (!active_ || popup_ != 4) return;
        if (dik == 0x0e) { if (!name58_.empty()) name58_.pop_back(); return; }
        if (dik == 0x1c || dik == 0x9c) {
            if (name58_.find_first_not_of(L' ') != std::wstring::npos) { Event("char_accept"); nameAccepted_ = true; }
            return;
        }
        static const char* kDigits = "1234567890";  // DIK 0x02..0x0b
        static const struct { int dik; char c; } kLetters[] = {
            { 0x1e, 'a' }, { 0x30, 'b' }, { 0x2e, 'c' }, { 0x20, 'd' }, { 0x12, 'e' }, { 0x21, 'f' }, { 0x22, 'g' },
            { 0x23, 'h' }, { 0x17, 'i' }, { 0x24, 'j' }, { 0x25, 'k' }, { 0x26, 'l' }, { 0x32, 'm' }, { 0x31, 'n' },
            { 0x18, 'o' }, { 0x19, 'p' }, { 0x10, 'q' }, { 0x13, 'r' }, { 0x1f, 's' }, { 0x14, 't' }, { 0x16, 'u' },
            { 0x2f, 'v' }, { 0x11, 'w' }, { 0x2d, 'x' }, { 0x15, 'y' }, { 0x2c, 'z' },
        };
        wchar_t ch = 0;
        if (dik >= 0x02 && dik <= 0x0b) ch = wchar_t(kDigits[dik - 2]);
        for (const auto& l : kLetters) if (l.dik == dik) ch = wchar_t(l.c);
        if (dik == 0x39) ch = L'_';
        if (!ch) return;
        if (dik != 0x39) {  // the layout's character, if it is one letter or digit (0x583660)
            UINT vk = MapVirtualKeyW(UINT(dik), MAPVK_VSC_TO_VK);
            BYTE ks[256] = {};
            wchar_t buf[4] = {};
            if (vk && ToUnicode(vk, UINT(dik), ks, buf, 4, 0) == 1 && iswalnum(buf[0])) ch = buf[0];
        }
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
        ch = shift != caps ? towupper(ch) : towlower(ch);
        if (name58_.size() < 16) name58_ += ch;
    }

private:
    static const int kExit = 99;
    static const DWORD kCheckingMs = 333;  // 40 updates (~120 per second, as the attract counter)
    static const DWORD kWorkMs = 1000;     // 120 updates: shortest time "Saving..." etc. stays up

    std::string ScreenName(int p) const
    {
        static const char* kNames[] = { "Checking", "EmptyCard", "LSD1", "LSD2", "EnterName", "InvName",
                                        "FileExists", "LODOyesno", "LODyesno", "LODyesno", "LSDing", "LSDing",
                                        "LSDing", "LSDok", "LSDok", "LSDok", "LSDfailed", "LSDfailed",
                                        "LSDfailed", "Corrupt", "ASOffQ" };
        if (mode_ == BOOT && p == 1) return "NoSave";      // 0x542910
        if (mode_ == BOOT && p == 7) return "LODyesno";    // 0x5428dd
        return p >= 0 && p < 21 ? kNames[p] : "";
    }

    // 0x545920: show popup p (MUMAIN by name, or GUI\NAME for 4).
    void Show(int p)
    {
        popup_ = p;
        shownAt_ = GetTickCount();
        if (p == 2) op_ = 0;
        if (p == 4) {
            name_.Reset(0);
        } else {
            int i = mu_.mec.Find(ScreenName(p));
            if (i >= 0) {
                mu_.mec.screens[i].focus = mu_.mec.screens[i].defaultFocus;  // 0x4677e0 -> 0x467730
                mu_.Reset(i);
            }
        }
        Log("Profiles: step %d, popup %d '%s'", step_, p, ScreenName(p).c_str());
    }

    // Step 1: list the save folders (device vtbl+0x50/+0x54, 0x582f10). A
    // folder counts if its name starts with the prefix (0x542860, exact
    // case) and the rest is letters, digits or '_' (0x541770); at most 100.
    void Enumerate()
    {
        names_.clear();
        folders_.clear();
        WIN32_FIND_DATAW fd;
        std::string saves = Profile::SavesDir();
        std::wstring dir(saves.begin(), saves.end());
        HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::wstring n = fd.cFileName;
                if (n.compare(0, prefix_.size(), prefix_) != 0 || n.size() == prefix_.size()) continue;
                std::wstring rest = n.substr(prefix_.size());
                bool ok = true;
                for (wchar_t c : rest) ok = ok && (iswalnum(c) || c == L'_');
                if (!ok) continue;
                names_.push_back(rest);
                std::string a;
                for (wchar_t c : n) a += c < 0x80 ? char(c) : '?';  // 0x542110
                folders_.push_back(a);
            } while (names_.size() < 100 && FindNextFileW(h, &fd));
            FindClose(h);
        }
        count_ = int(names_.size());
        Log("Profiles: %d folder(s) with prefix %ls", count_, prefix_.c_str());
    }

    void Select(const std::string& folder) { sel_ = folder; }  // 0x542140 (device vtbl+0x10)
    std::string FolderOf(const std::wstring& name) const
    {
        std::string a;
        for (wchar_t c : prefix_ + name) a += c < 0x80 ? char(c) : '?';
        return a;
    }
    void Revert()  // 0x548840: saved -> live
    {
        g_profile.Restore(backup_);
        ApplyProfile();
    }

    // Steps 12 load / 13 save / 14 delete (0x5421f0 / 0x5422f0 / 0x5423c0).
    // Done once; on success the popup stays for at least 120 updates.
    void Work(DWORD el)
    {
        if (mode_ == REPLAYS && step_ == 12) {  // a replay plays in the game: gameplay boundary
            replay_ = sel_;
            step_ = kExit;
            return;
        }
        if (!workDone_) {
            workDone_ = true;
            if (step_ == 12) result_ = g_profile.LoadFolder(sel_);
            else if (step_ == 13) { StoreLive(); result_ = g_profile.SaveFolder(sel_) ? 1 : 2; }
            else result_ = Profile::DeleteFolder(sel_) ? 1 : 2;
            Log("Profiles: %s %s -> %d", step_ == 12 ? "load" : step_ == 13 ? "save" : "delete", sel_.c_str(), result_);
        }
        if (result_ == 1) {
            if (el < kWorkMs) return;
            workDone_ = false;
            if (step_ == 14) { step_ = 15; Show(15); return; }  // deleted
            prev_ = step_;
            step_ = 18;
            return;
        }
        workDone_ = false;
        if (step_ == 12) {  // 0x544d1d / 0x544c9a: revert; corrupt or failed
            Revert();
            if (result_ == 3) { step_ = 17; Show(19); }
            else { step_ = 16; Show(16); }
        } else if (step_ == 13) { step_ = 17; Show(17); }
        else { step_ = 16; Show(18); }
    }

    // Step 18 (0x544f06): ProfileData must be in the folder now; then the
    // settings are committed (0x548300) and the folder becomes the current
    // profile (0x8ac5a0 vtbl+4).
    void Verify()
    {
        bool found = Profile::FolderHasProfile(sel_);
        bool load = prev_ == 12;
        if (found) {
            if (load) ApplyProfile();
            g_profile.SetCurrent(sel_);
            backup_ = g_profile.Snapshot();
            Log("Profiles: current profile %s", sel_.c_str());
            step_ = 15;
            Show(load ? 13 : 14);
        } else {
            if (load) Revert();
            step_ = 16;
            Show(load ? 16 : 17);
        }
    }

    // Event handlers (0x543090 / 0x545060), by popup.
    void Event(const std::string& a)
    {
        Log("Profiles: event %s (popup %d)", a.c_str(), popup_);
        auto is = [&](const char* s) { return _stricmp(a.c_str(), s) == 0; };
        switch (popup_) {
        case 1:  // EmptyCard; at boot NoSave ("create a profile?")
            if (mode_ == BOOT) {
                if (is("mu_yes")) { name58_.clear(); op_ = 2; step_ = 6; Show(4); }
                else if (is("mu_no")) { step_ = 19; Show(20); }
            } else if (is("mu_continue") || is("mu_cancel")) { step_ = 4; Show(2); }
            break;
        case 2:  // LSD1: Load / Save / Delete / Exit
            if (is("mu_load") || is("mu_delete") || (is("mu_save") && mode_ != REPLAYS)) {
                op_ = is("mu_load") ? 1 : is("mu_save") ? 2 : 3;
                if (op_ == 2 || count_ > 0) { step_ = 5; Show(3); }
                else { step_ = 3; Show(1); }
            } else if (is("mu_exit")) {
                if (mode_ == BOOT) { step_ = 19; Show(20); }  // ASOffQ
                else step_ = kExit;
            }
            break;
        case 3:  // LSD2: the list
            if (StartsWithI(a, "mu_file")) {
                int idx = atoi(a.c_str() + 7) + offset_ - 1;
                if (op_ == 1 && idx < count_) {
                    Select(folders_[idx]);
                    name58_ = names_[idx];  // only a load copies the name (0x545432)
                    step_ = 9;
                    Show(7);
                } else if (op_ == 2) {
                    if (idx < count_) { Select(folders_[idx]); step_ = 10; Show(8); }
                    else { name58_.clear(); step_ = 6; Show(4); }  // the free slot: enter a name
                } else if (op_ == 3 && idx < count_) {
                    Select(folders_[idx]);
                    step_ = 11;
                    Show(9);
                }
            } else if (is("mu_cancel")) {
                if (offset_ > 0 && count_ - offset_ < 10) --offset_;
                step_ = 4;
                Show(2);
            } else if (is("mu_pageup")) {
                if (offset_ > 0) --offset_;
            } else if (is("mu_pagedown")) {
                if (Limit() > offset_ + 10) ++offset_;
            }
            break;
        case 4:  // name entry
            if (is("char_accept")) {
                for (const std::wstring& n : names_)
                    if (_wcsicmp(n.c_str(), name58_.c_str()) == 0) { step_ = 8; Show(6); return; }  // FileExists
                Select(FolderOf(name58_));
                step_ = 13;
                Show(11);
            } else if (is("mu_cancel")) {
                step_ = 5;
                Show(3);
            }
            break;
        case 5: case 6:  // InvName / FileExists
            if (is("mu_cancel") || is("mu_continue")) { step_ = 6; Show(4); }
            break;
        case 7: case 8: case 9:  // confirm load / overwrite / delete
            if (is("mu_yes")) { step_ = 12 + (popup_ - 7); Show(10 + (popup_ - 7)); }
            else if (is("mu_no")) { step_ = 5; Show(3); }
            break;
        case 13: case 14: case 15: case 16: case 17: case 18: case 19:  // results
            if (!is("mu_continue")) break;
            if (mode_ == BOOT && (popup_ == 13 || popup_ == 14)) step_ = kExit;  // 0x5439a8
            else { step_ = 0; Show(0); }
            break;
        case 20:  // ASOffQ (boot): carry on without a profile?
            if (is("mu_yes")) step_ = kExit;
            else if (is("mu_no")) { step_ = 0; Show(0); }
            break;
        }
    }

    int Limit() const { return count_ + (op_ == 2 && count_ < 100 ? 1 : 0); }  // save adds the free slot

    // Status queries (0x5459a0 and the boot copy at 0x543b20..).
    int Status(const std::string& s, int st) const
    {
        auto is = [&](const char* n) { return _stricmp(s.c_str(), n) == 0; };
        if (is("mustat_datatype")) return mode_ == REPLAYS ? 1 : 0;  // row 0 profiles, 1 replays
        if (is("mustat_replay")) return mode_ == REPLAYS ? st : 2;
        if (is("mustat_title")) return op_ == 2 ? 1 : op_ == 3 ? 2 : 0;  // table 0x545c4c
        if (StartsWithI(s, "mustat_file")) {
            int idx = atoi(s.c_str() + 11) + offset_ - 1;
            return idx >= (op_ == 2 ? Limit() : count_) ? 2 : st;
        }
        if (is("mustat_load")) return op_ == 1 ? 0 : st;
        if (is("mustat_save")) return op_ == 2 ? 0 : st;
        if (is("mustat_del")) return op_ == 3 ? 0 : st;
        if (is("mustat_pageup")) return offset_ > 0 ? 0 : 2;
        if (is("mustat_pagedown")) return offset_ + 10 < Limit() ? 0 : 2;
        return st;
    }
    bool Text(const std::string& fn, std::wstring& out) const
    {
        if (StartsWithI(fn, "mutxt_file")) {
            int idx = atoi(fn.c_str() + 10) + offset_ - 1;
            if (idx < 0 || idx >= count_) return false;  // left as it is (MEC text)
            out = names_[idx];
            return true;
        }
        if (_stricmp(fn.c_str(), "mutxt_name") == 0 || _stricmp(fn.c_str(), "txt_entryname") == 0) {
            out = name58_;
            return true;
        }
        return false;
    }

    MecMenu mu_, name_;
    bool loaded_ = false, active_ = false, workDone_ = false;
    Mode mode_ = BOOT;
    std::wstring prefix_, name58_;  // +0x58: the chosen or typed name
    std::vector<std::wstring> names_;
    std::vector<std::string> folders_;
    std::string sel_, replay_;
    std::vector<uint8_t> backup_;
    int step_ = 0, popup_ = 0, op_ = 0, offset_ = 0, count_ = 0, prev_ = 0, result_ = 0;
    bool nameAccepted_ = false;  // set by OnKeyPress, read by OnKey for the same WM_KEYDOWN
    DWORD shownAt_ = 0;
};
static MemoryUnit g_mu;

// --------------------------------------------------- 0xAA intro movie ------
// =========================================================== FMV subtitles =
// The movie player's open (0x551220) builds "<video>.SBN" (timing) and
// "LANG:<video>.TXT" (text) from the video path and loads them into a
// subtitle object (registry slot 0x258, vtbl 0x78d724, loader 0x598e90,
// parser 0x598ff0). The .SBN is: u32 0x02111966, float 5.01, u32 offset[12],
// then per offset a track {u32 count, u32 cursor, count x 28-byte record:
// float start, end (s); u16 flags; u16 text id; u8 r, g, b, a; u32;
// float sx, sy}. Every frame (0x5990c0) the time is frame * 0.04 and each
// track shows the record with start <= time <= end; one finished record
// per frame moves the track's cursor on.
class Subtitles
{
public:
    void Load(const std::string& videoPath)
    {
        tracks_.clear();
        text_.clear();
        size_t dot = videoPath.find_last_of('.');
        if (dot == std::string::npos) return;
        std::string base = videoPath.substr(0, dot);
        std::vector<uint8_t> d;
        FILE* f = fopen(g.Resolve(base + ".SBN").c_str(), "rb");
        if (!f) return;
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
        fclose(f);
        float version = 0;
        if (d.size() < 0x38) return;
        memcpy(&version, &d[4], 4);
        if (version != 5.01f) return;  // 0x599068: anything else is dropped
        for (int t = 0; t < 12; ++t) {
            uint32_t off;
            memcpy(&off, &d[8 + 4 * t], 4);
            Track tr;
            if (off && off + 8 <= d.size()) {
                uint32_t count;
                memcpy(&count, &d[off], 4);
                for (uint32_t i = 0; i < count && off + 8 + 28 * (i + 1) <= d.size(); ++i) {
                    const uint8_t* r = &d[off + 8 + 28 * i];
                    Record rec;
                    memcpy(&rec.start, r, 4);
                    memcpy(&rec.end, r + 4, 4);
                    memcpy(&rec.flags, r + 8, 2);
                    memcpy(&rec.id, r + 10, 2);
                    for (int k = 0; k < 4; ++k) rec.rgba[k] = r[12 + k] / 255.0f;
                    memcpy(&rec.sx, r + 20, 4);
                    memcpy(&rec.sy, r + 24, 4);
                    tr.recs.push_back(rec);
                }
            }
            tracks_.push_back(tr);
        }
        text_ = LoadTxt(g.Resolve("LANG:" + base + ".TXT"));
        // 0x551220: the making-of has English speech only, so its subtitles
        // are switched on when the text language is not English.
        forceOn_ = _stricmp(videoPath.c_str(), "FMV\\THEMAKINGOF.XMV") == 0 && kLanguages[g.textSetting].id != 0;
        Log("Subtitles: %s.SBN, %zu text entries", base.c_str(), text_.size());
    }
    void Clear() { tracks_.clear(); text_.clear(); }

    void Draw(Canvas& cv, uint32_t frame)
    {
        if (tracks_.empty() || !g.font.Loaded()) return;
        float time = float(frame) * 0.04f;
        bool on = forceOn_ || g.Toggle("sub");  // settings +0x171 -> object +0x58
        // 0x598e90: text block between y 0.125 and 0.725 (aspect 2: 0.0833 and
        // 0.76666), each moved 0.045 inwards.
        bool wide = g.aspect == 2;
        float top = (wide ? 0.08334f : 0.125f) + 0.045f, bottom = (wide ? 0.76666f : 0.725f) - 0.045f;
        for (size_t t = 0; t < tracks_.size(); ++t) {
            Track& tr = tracks_[t];
            if (tr.cur >= tr.recs.size()) continue;
            if (tr.recs[tr.cur].end <= time && ++tr.cur >= tr.recs.size()) continue;
            const Record& r = tr.recs[tr.cur];
            if (!(r.start <= time && time <= r.end)) continue;
            if (!on && !(r.flags & 0x80)) continue;  // 0x80: shown even with subtitles off
            if (!LanguageAllowed(r.flags)) continue;
            auto it = text_.find(r.id);
            if (it == text_.end() || it->second.empty()) continue;
            // 0x599330: tracks 0-2 and 6-8 from the top line, 3-5 and 9-11
            // from the bottom line, 0.05 apart. y is the top of the text.
            int row = int(t % 6);
            float y = (row < 3 ? top + 0.05f * row : bottom + 0.05f * (row - 3)) * kHeight;
            // Box x 0.1, width 0.8 (0x5991fc / 0x59925d); flags & 0x3000:
            // 0x1000 -> 1 (right, measured), 0x2000 -> 2 (centre), else 0 (left).
            // The text object gets the box (x, y, width) through vtbl+0x20
            // (0x5761ca), so long lines wrap inside it, downwards.
            // y is the top of the capitals (measured: 351 for y 0.73 x 480).
            // Drawn like the menu text (same text object): glyphs sampled as
            // sprites, box in whole pixels (section 28). The vertical
            // correction is -0.5 px here, not the menu's +0.5: measured on
            // the making-of lines and the attract "DEMO" (both 1 px low with
            // +0.5); the two y formulas differ (top offset x sy here, x the
            // 480/512 height there).
            y += -0.5f - g.font.TopOffset() * r.sy;
            float x0 = float(int(0.1f * kWidth)), w = float(int(0.8f * kWidth));
            int align = (r.flags & 0x3000) == 0x1000 ? 1 : (r.flags & 0x3000) == 0x2000 ? 2 : 0;
            for (const std::wstring& line : Wrap(it->second, w, r.sx)) {
                float tw = g.font.Width(line, r.sx);
                float x = align == 1 ? x0 + w - tw : align == 2 ? x0 + (w - tw) / 2 : x0;
                g.font.Draw(cv, line, x, y, r.sx, r.sy, r.rgba);
                y += g.font.LinePitch() * r.sy;  // 32 x 0.75 = 24 px, as measured
            }
        }
    }

private:
    // 0x5991b7: one bit per text language (+0x2c): English 0x1, French 0x2,
    // German 0x4, Spanish 0x8, Italian 0x10, Japanese 0x20, Chinese 0x40,
    // Taiwanese 0x100, Korean 0x200, Portuguese 0x400; other ids: always.
    static std::vector<std::wstring> Wrap(const std::wstring& s, float width, float sx) { return WrapText(s, width, sx); }

    static bool LanguageAllowed(uint16_t flags)
    {
        static const uint16_t kBits[] = { 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x100, 0x200, 0x400 };
        uint32_t id = kLanguages[g.textSetting].id;
        return id > 9 || (flags & kBits[id]) != 0;
    }

    struct Record { float start = 0, end = 0; uint16_t flags = 0, id = 0; float rgba[4] = {}; float sx = 1, sy = 1; };
    struct Track { std::vector<Record> recs; size_t cur = 0; };
    std::vector<Track> tracks_;
    TextTable text_;
    bool forceOn_ = false;
};

class IntroFmvScreen : public Screen
{
public:
    void SetIndex(int i) { index_ = i; }                    // vtbl+0x20 (0x5507f0)
    void SetFile(const std::string& f) { custom_ = f; }     // vtbl+0x24 (0x556380)
    // The attract video (0x551dc0) comes back to the front end and could not
    // be skipped with a key in the original (observed, see Update below).
    void SetReturn(int state, bool skippable) { returnTo_ = state; skippable_ = skippable; }

    void Enter() override                                    // 0x5563a0
    {
        static const char* kFiles[] = { "TERR:FMV\\LEGAL.XMV", "FMV\\ATARI.XMV", "FMV\\REFLECT.XMV" };
        std::string file = index_ <= 2 ? kFiles[index_] : custom_;
        if (!movie_.Open(g.Resolve(file), g.AudioTrack(), false))
            return Finish();  // missing file: carry on
        subs_.Load(file);
    }
    void Leave() override { movie_.Close(); subs_.Clear(); }
    void Update() override
    {
        movie_.Update();
        if (movie_.Done()) Finish();  // listener 0x556470 -> manager vtbl+0x28
    }
    void Render(HDC dc, Canvas& cv) override
    {
        movie_.Draw(dc, 0, 0, kWidth, kHeight, true);
        subs_.Draw(cv, movie_.Frame());
    }
    void OnKey(WPARAM vk) override
    {
        if (skippable_ && IsSkipKey(vk)) Finish();
    }
    // The player (state 0xD1, 0x558260) has no GUI; the joystick skips only
    // when Skip Sequence has a joystick alternative.
    void OnPad(WPARAM) override {}
    bool OnJoyButton(int code) override
    {
        if (!skippable_ || !IsSkipJoy(code)) return false;
        Finish();
        return true;
    }

private:
    // At boot the original returns to the sequencer. After the attract video
    // it returned to the main menu (observed; the static path through manager
    // vtbl+0x28, whose "home" is the first state created, was not resolved).
    void Finish()
    {
        int to = returnTo_;
        returnTo_ = ST_BOOT_SEQ;
        skippable_ = true;
        g.Goto(to);
    }

    int index_ = 0;
    std::string custom_;
    int returnTo_ = ST_BOOT_SEQ;
    bool skippable_ = true;
    Movie movie_;
    Subtitles subs_;
};
static IntroFmvScreen g_introFmv;

// --------------------------------------------------- 0xA9 boot sequencer ---
class BootSequencer : public Screen
{
public:
    void Enter() override  // FUN_005564c0: one step per activation
    {
        int step = step_++;
        if (step == 0) step = step_++;  // case 0: engine warm-up (0x8ac270 vtbl+0x18), no screen
        switch (step) {
        case 1: g_introFmv.SetIndex(0); g.Goto(ST_INTRO_FMV); break;  // TERR:FMV\LEGAL.XMV
        case 2: g_introFmv.SetIndex(1); g.Goto(ST_INTRO_FMV); break;  // FMV\ATARI.XMV
        case 3:
            g_introFmv.SetFile("FMV\\NVIDIA.XMV");
            g_introFmv.SetIndex(3);
            g.Goto(ST_INTRO_FMV);
            break;
        case 4: g.Goto(ST_LANGUAGE); break;
        case 5: g.Goto(ST_TITLE); break;
        case 6:
            Log("event: bootup_over");
            g.Goto(ST_FRONTEND);
            break;
        }
    }

private:
    int step_ = 0;
};

// --------------------------------------------------- 0xAB language select -
class LanguageScreen : public Screen
{
public:
    void Enter() override  // 0x5565f0: GUI\LANGUAGE, page Lang_EU for Europe
    {
        menu_.Load("GUI\\LANGUAGE");
        menu_.onAction = [](const std::string& a) {  // events handled at 0x556770
            for (int i = 0; i < kLanguageCount; ++i)
                if (a == kLanguages[i].event) {
                    g.SetLanguage(i);  // both languages and settings, even if unchanged
                    // The original loads the profile after this screen; loading
                    // ends in 0x548840 (0x542d03), which applies its saved
                    // settings, both languages included. Here the profile is
                    // already loaded, so apply it again.
                    if (g_profile.HasFile()) ApplyProfile();
                    g.Goto(ST_BOOT_SEQ);
                    return;
                }
        };
        int page = menu_.mec.Find("Lang_EU");
        menu_.Reset(page < 0 ? 0 : page);
        // Start on the current language's button.
        MecScreen* s = menu_.Top();
        for (int i = 0; s && i < int(s->items.size()); ++i)
            if (s->items[i].actions[ACT_SELECT] == kLanguages[g.lang].event) {
                s->items[s->focus].state = 1;
                s->focus = i;
                s->items[i].state = 0;
            }
    }
    void Render(HDC dc, Canvas& cv) override { menu_.Render(dc, cv); }
    void Update() override { menu_.Update(); }
    void OnKey(WPARAM vk) override
    {
        if (vk != VK_ESCAPE) menu_.OnKey(vk);
    }
    void OnMouse(int code, float x, float y) override { menu_.OnMouse(code, x, y); }

private:
    MecMenu menu_;
};

// --------------------------------------------------- 0xAC title screen -----
class TitleScreen : public Screen
{
public:
    void Enter() override  // 0x541a70: loads GUI\BOOTUP (slot 5) + GUI\MUMAIN (slot 6)
    {
        menu_.Load("GUI\\BOOTUP");
        menu_.onAction = [this](const std::string& a) {
            if (a == "bu_continue" && !profiles_) {  // handler 0x541b60
                Log("event: bu_continue");
                // GUI slot 5 (BOOTUP) is hidden and the profile object
                // (0x8ac5a8) started; Update waits until it is done (0x541ae5).
                profiles_ = true;
                g_mu.Start(MemoryUnit::BOOT);
            }
        };
        menu_.Reset(0);
        profiles_ = false;
    }
    void Render(HDC dc, Canvas& cv) override
    {
        if (profiles_) g_mu.Render(dc, cv);
        else menu_.Render(dc, cv);
    }
    void Update() override
    {
        if (!profiles_) { menu_.Update(); return; }
        if (!g_mu.Update()) g.Goto(ST_BOOT_SEQ);  // finished: unload slots 5/6, back to 0xA9
    }
    void OnKey(WPARAM vk) override
    {
        if (profiles_) g_mu.OnKey(vk);
        else menu_.OnKey(vk);
    }
    void OnKeyPress(int dik) override { if (profiles_) g_mu.OnKeyPress(dik); }
    void OnMouse(int code, float x, float y) override
    {
        if (profiles_) g_mu.OnMouse(code, x, y);
        else menu_.OnMouse(code, x, y);
    }

private:
    MecMenu menu_;
    bool profiles_ = false;
};

// --------------------------------------------------- 0xB3 gameplay (stub) -
class GameplayStub : public Screen
{
public:
    std::string request;
    void Enter() override
    {
        Log("Gameplay requested (%s) - not implemented, closing.", request.c_str());
        g.Quit();
    }
};
static GameplayStub g_gameplay;

// --------------------------------------------------- front end -------------
// FUN_00552480 loads GUI\FRONT; the screens, layout, textures and text come
// from front.MEC / front.TXT. This class plays the part of the game-side
// event handlers (0x552c00-0x5555a0) for the actions that matter before
// gameplay starts.
// Cutscene -> mission that unlocks it (table 0x7a20e8, {name, mission}, ends
// with a null name). "stat_sc<id>" asks for the cutscene, "cut <id>" plays it.
// Front-end cheat codes, table 0x7a20a0: 6 x {u32 cheat index; u8 position;
// u8 keys[7]} with DirectInput key codes. The index is the {unlocked,
// enabled} pair at settings +0x19e + 2 x index (0 allmissions, 1 weapons,
// 2 ammo, 3 invincibility, 4 immunity, 6 vehicle; 5 "tommy" has no code).
// The position lives in the table, so it is kept for the whole run.
struct CheatCode { int index; int pos; uint8_t keys[7]; };
static CheatCode g_cheatCodes[] = {
    { 1, 0, { 0x14, 0x21, 0x3f, 0x26, 0x08, 0x08, 0x19 } },  // T F F5 L 7 7 P
    { 2, 0, { 0x14, 0x22, 0x3e, 0x25, 0x07, 0x08, 0x18 } },  // T G F4 K 6 7 O
    { 3, 0, { 0x13, 0x21, 0x3f, 0x26, 0x08, 0x07, 0x19 } },  // R F F5 L 7 6 P
    { 4, 0, { 0x13, 0x22, 0x3e, 0x26, 0x07, 0x07, 0x19 } },  // R G F4 L 6 6 P
    { 0, 0, { 0x30, 0x21, 0x3f, 0x25, 0x06, 0x08, 0x18 } },  // B F F5 K 5 7 O
    { 6, 0, { 0x30, 0x22, 0x3e, 0x25, 0x06, 0x06, 0x19 } },  // B G F4 K 5 5 P
};

struct CutsceneUnlock { const char* id; int mission; };
static const CutsceneUnlock kCutscenes[] = {
    { "01", 2 },   { "5a", 4 },   { "5b", 4 },   { "07", 5 },   { "09", 7 },   { "10b", 8 },
    { "11", 9 },   { "13a", 10 }, { "13b", 11 }, { "14", 13 },  { "15", 14 },  { "16", 15 },
    { "17a", 16 }, { "17b", 17 }, { "18a", 18 }, { "18b", 19 }, { "19", 21 },  { "20", 22 },
    { "21", 22 },  { "22", 24 },  { "24", 25 },  { "25a", 27 }, { "25b", 28 }, { "27", 30 },
    { "28", 31 },  { "30", 32 },  { "31", 32 },
};

// Take A Ride vehicles: one list per city, filled in this order by the front
// end's constructor (0x552541.., insert 0x5559a0). Each node keeps the preview
// video "FMV\<CITY>_CAR<car>.XMV" (+0x8) and the vehicle id (+0xc).
struct RideVehicle { int car, id; };
static const RideVehicle kMiamiVehicles[] = {
    { 12, 6 }, { 23, 45 }, { 9, 31 },  { 17, 44 }, { 1, 22 },  { 16, 34 }, { 4, 33 },  { 8, 35 },
    { 14, 2 }, { 15, 38 }, { 6, 40 },  { 2, 32 },  { 11, 24 }, { 13, 37 }, { 3, 23 },  { 5, 10 },
    { 19, 36 }, { 20, 39 }, { 18, 64 }, { 27, 19 }, { 24, 47 }, { 25, 46 }, { 22, 80 }, { 21, 66 },
    { 7, 43 },
};
static const RideVehicle kNiceVehicles[] = {
    { 21, 50 }, { 26, 69 }, { 2, 67 },  { 24, 68 }, { 19, 93 }, { 6, 25 },  { 11, 13 }, { 4, 16 },
    { 8, 17 },  { 16, 61 }, { 7, 92 },  { 20, 62 }, { 12, 94 }, { 1, 55 },  { 3, 27 },  { 10, 51 },
    { 22, 52 }, { 23, 53 }, { 18, 59 }, { 17, 60 }, { 14, 58 }, { 13, 19 }, { 27, 71 }, { 28, 70 },
    { 5, 65 },  { 25, 42 }, { 9, 54 },
};
static const RideVehicle kIstanbulVehicles[] = {
    { 1, 29 },  { 19, 85 }, { 16, 81 }, { 15, 82 }, { 11, 21 }, { 10, 30 }, { 5, 72 },  { 9, 75 },
    { 2, 28 },  { 3, 74 },  { 14, 77 }, { 13, 73 }, { 6, 15 },  { 12, 76 }, { 18, 19 }, { 21, 84 },
    { 20, 83 }, { 8, 79 },  { 4, 78 },  { 17, 41 },
};
struct RideCity { const char* prefix; const RideVehicle* list; int count; };
static const RideCity kRideCities[] = {
    { "MIAMI_", kMiamiVehicles, int(_countof(kMiamiVehicles)) },
    { "NICE_", kNiceVehicles, int(_countof(kNiceVehicles)) },
    { "ISTAN_", kIstanbulVehicles, int(_countof(kIstanbulVehicles)) },
};

// 0x549d70: vehicles that must be unlocked, and their byte in the settings
// object (+0x174 + 0x3e..0x4c). Any other id is always available.
struct VehicleLock { int id, off; };
static const VehicleLock kVehicleLocks[] = {
    { 37, 0x1b2 }, { 35, 0x1b3 }, { 33, 0x1b4 }, { 66, 0x1b5 }, { 80, 0x1b6 }, { 43, 0x1b7 }, { 51, 0x1b8 },
    { 77, 0x1ba }, { 42, 0x1bb }, { 54, 0x1bc }, { 65, 0x1bd }, { 79, 0x1be }, { 78, 0x1bf }, { 41, 0x1c0 },
};

// 0x556190: boats (vehicle ids 0x2d..0x5b, byte table 0x5561c0).
static bool IsBoat(int id)
{
    static const int kBoats[] = { 45, 46, 47, 48, 69, 70, 71, 83, 84, 85, 89, 90, 91 };
    for (int b : kBoats)
        if (b == id) return true;
    return false;
}

class FrontEnd : public Screen
{
public:
    FrontEnd()
    {
        g.onLanguageChanged.push_back([this] {
            if (menu_.mec.screens.size()) menu_.ReloadText();
            LoadControlsText();
        });
    }

    void Enter() override
    {
        menu_.Load("GUI\\FRONT");
        menu_.onAction = [this](const std::string& a) { Action(a); };
        menu_.onStatus = [this](const std::string& s, int st) { return Status(s, st); };
        menu_.drawFmv = [this](HDC dc, int x, int y, int w, int h) { pane_.Draw(dc, x, y, w, h, false); };
        menu_.dynamicText = [this](const std::string& fn, std::wstring& out) {
            // Vehicle names come from the game's vehicle database: not loaded here,
            // so hide the "DevOnly: Vehicle txt" placeholder.
            if (fn == "reqvehicletext") { out.clear(); return true; }
            // Controller name (0x554f26 -> 0x5522d0) of the shown index +0x48.
            if (fn == "txt_controller") { out = g_joy.Name(joyIndex_); return true; }
            // Gamma (0x555228): _gcvt with 3 digits, "1." completed to "1.0".
            if (fn == "txt_gamma") {
                char buf[32];
                _gcvt(g.gamma, 3, buf);
                size_t n = strlen(buf);
                if (n && buf[n - 1] == '.') strcat(buf, "0");
                out.assign(buf, buf + strlen(buf));
                return true;
            }
            // Resolution (0x5552ce): the name in the settings record (+0xc38).
            if (fn == "txt_displayres") { out = g.res.name; return true; }
            if (fn == "Txt_SoundDriver") { out = g.DriverName(g.soundDriver); return true; }  // 0x55471f
            return KeyMapText(fn, out);
        };
        // "%d" values: val_overallvol +0x34 (0x5551c4), Val_Volume +0x38 and
        // Val_Music +0x3c (0x5545b0 / 0x5545de), Val_Turn +0x17c (0x5548dd),
        // val_mixer +0x150 (0x5547c2), Val_TC_MotionBlur (below). The names
        // are compared without case.
        menu_.valueText = [](const std::string& fn, int& v) {
            if (fn == "val_overallvol") { v = g.vol.overall; return true; }
            if (fn == "val_volume") { v = g.vol.effects; return true; }
            if (fn == "val_music") { v = g.vol.music; return true; }
            if (fn == "val_turn") { v = g.turn; return true; }
            if (fn == "val_mixer") { v = g.mixRate; return true; }
            // Val_TC_MotionBlur (0x55493f): +0xc times 10.05, truncated (fistp
            // with the rounding mode set to chop).
            if (fn == "val_tc_motionblur") { v = int(double(g.motionBlur) * double(10.05f)); return true; }
            return false;
        };
        LoadControlsText();
        menu_.Reset(0);  // "Main"
        g_music.Load();  // SOUNDS\FEMUSIC.DAT (FUN_00552480), started at 0x551ca0
        g_music.Play();
        idleSince_ = GetTickCount();  // 0x551d22: attract counter +0x18 = 0
    }
    void Leave() override
    {
        g_music.Stop();  // 0x552c6c releases the music object
        bg_.Close();
        pane_.Close();
        extra_.Close();
    }

    void Update() override
    {
        if (extra_.IsOpen()) {
            extra_.Update();
            if (extra_.Done()) EndExtra();
            return;
        }
        bg_.Update();
        pane_.Update();
        // Profiles / Replays (states 0xB2 / 0xB1, 0x541d10): the profile object
        // runs until it is done, then the front end (0xAD) carries on; its own
        // update (attract counter, cheat codes) does not run meanwhile.
        if (g_mu.Active()) {
            idleSince_ = GetTickCount();
            if (!g_mu.Update()) {
                // Back in the front end the Options screen has its default
                // focus again (Extras, observed on the original).
                menu_.ResetFocus();
                std::string replay = g_mu.TakeReplay();
                if (!replay.empty()) LaunchGameplay("LoadReplay " + replay);
            }
            return;
        }
        menu_.Update();
        // Waiting for a key: the chosen row's two cells blink (0x549f84, 30
        // updates per phase; taken here as 0.5 s).
        if (km_.state == 1) km_.blink = ((GetTickCount() - km_.since) / 500) % 2;
        UpdateAttract();
    }

    void Render(HDC dc, Canvas& cv) override
    {
        if (extra_.IsOpen()) {
            extra_.Draw(dc, 0, 0, kWidth, kHeight, true);
            extraSubs_.Draw(cv, extra_.Frame());
            return;
        }
        // While Profiles / Replays is open only its popup is drawn: the
        // original shows it on black, without the front end (observed).
        if (g_mu.Active()) return g_mu.Render(dc, cv);
        // The movie fills the screen only on screens whose background is
        // the movie (header +8 = -2); TAR_Main, TAR_Start and DG_Main show
        // the picture of material 1 instead (drawn by the menu).
        MecScreen* top = menu_.Top();
        if (!top || top->background == -2) bg_.Draw(dc, 0, 0, kWidth, kHeight, false);
        menu_.Render(dc, cv);
    }

    void OnKey(WPARAM vk) override
    {
        idleSince_ = GetTickCount();  // any input clears the counter (0x551e00 -> +0x18 = 0)
        if (extra_.IsOpen()) { if (IsSkipKey(vk)) EndExtra(); return; }
        if (km_.state == 1) return CaptureKey(vk);
        if (g_mu.Active()) return g_mu.OnKey(vk);
        menu_.OnKey(vk);
    }

    void OnMouse(int code, float x, float y) override
    {
        if (extra_.IsOpen()) return;
        if (g_mu.Active()) g_mu.OnMouse(code, x, y);
        else menu_.OnMouse(code, x, y);
    }

    // DirectInput mouse buttons (0x583a00) count as input for the attract
    // counter (0x551e51) and are read by the key-map editor (0x549ec2).
    void OnMouseButton(int button) override
    {
        idleSince_ = GetTickCount();
        if (extra_.IsOpen()) return;
        if (km_.state == 1) CaptureAlt(0x2000 + button);
    }

    // Joystick buttons and POV directions (0x583fb0) also count for the
    // attract counter (0x551e46); the key-map editor reads them before the
    // mouse, and axes after it (0x549eae, 0x549f41).
    bool OnJoyButton(int code) override
    {
        idleSince_ = GetTickCount();
        if (extra_.IsOpen()) {
            if (!IsSkipJoy(code)) return false;
            EndExtra();
            return true;
        }
        if (km_.state != 1) return false;
        CaptureAlt(code);
        return true;
    }
    bool OnJoyAxis(int axis, bool dir) override
    {
        if (km_.state != 1 || extra_.IsOpen()) return false;
        CaptureAxis(axis, dir);
        return true;
    }
    // While a video plays or the editor waits, the menu does not move.
    void OnPad(WPARAM vk) override
    {
        if (extra_.IsOpen() || km_.state == 1) return;
        OnKey(vk);
    }

    // Cheat codes (0x551e63..0x551f14, every front-end update with a key
    // from 0x551f50, on any screen). Each table entry keeps its own
    // position: the key either matches the next one (position + 1) or the
    // position goes back to 0, so a wrong key is not tried as a first key
    // again. After 7 keys the cheat is unlocked (settings +0x19e + 2 x
    // index, only if index < 10 and the runtime byte +0x5 is clear, taken
    // here as clear) and bank 0 sample 3 is played (only if the previous
    // one has finished); then the position is reset.
    void OnKeyPress(int dik) override
    {
        if (g_mu.Active()) return g_mu.OnKeyPress(dik);  // name entry; no cheat check outside 0xAD
        for (CheatCode& c : g_cheatCodes) {
            if (dik != c.keys[c.pos]) { c.pos = 0; continue; }
            if (++c.pos < 7) continue;
            c.pos = 0;
            if (c.index < 10) g_profile.SetFlag(0x19e + 2 * c.index, true);
            Log("Cheat code %d entered: cheat %d unlocked", int(&c - g_cheatCodes), c.index);
            g_sounds.Play(MenuSounds::CHEAT);
        }
    }

private:
    // ---- attract mode ----------------------------------------------------
    // 0x551e00, once per front-end update: input or a GUI query that is
    // non-zero away from the Main screen (0x8ac3b0 vtbl+0x38(1)) clears the
    // counter +0x18; otherwise it counts up to +0x14 = 3600 (set at
    // 0x551d29) and then calls 0x551dc0, which plays FMV\ATTRACT.XMV
    // through the intro-video state. The original reached 3600 in about
    // 30 s (measured), so the count is kept here in real time.
    static const DWORD kAttractMs = 30000;
    void UpdateAttract()
    {
        DWORD now = GetTickCount();
        bool onMain = menu_.stack.size() == 1 && menu_.stack[0] == 0;
        if (extra_.IsOpen() || km_.state != 0 || !onMain) { idleSince_ = now; return; }
        if (now - idleSince_ < kAttractMs) return;
        idleSince_ = now;
        Log("Attract mode: no input for %lu ms on the Main screen", kAttractMs);
        g_introFmv.SetFile("FMV\\ATTRACT.XMV");  // 0x551dc0: vtbl+0x24, then index 3
        g_introFmv.SetIndex(3);
        g_introFmv.SetReturn(ST_FRONTEND, false);
        g.Goto(ST_INTRO_FMV);
    }
    DWORD idleSince_ = 0;

    // ---- events --------------------------------------------------------
    void Action(const std::string& a)
    {
        Log("event: %s", a.c_str());
        if (StartsWithI(a, "movie=")) { PlayBackground(a.substr(6)); return; }

        // Everything that leaves the front end for the game: FUN_00557d70 -> 0xB3.
        // Options -> Profiles / Replays (0x553f08 / 0x553f3b): states 0xB2 / 0xB1.
        if (_stricmp(a.c_str(), "LoadSave") == 0) return g_mu.Start(MemoryUnit::PROFILES);
        if (_stricmp(a.c_str(), "LoadReplay") == 0) return g_mu.Start(MemoryUnit::REPLAYS);
        static const char* kGameplay[] = { "StartGame", "NewGame", "ContinueGame", "fe_dgstart",
                                           "ReplayMission2" };
        for (const char* gp : kGameplay)
            if (_stricmp(a.c_str(), gp) == 0) return LaunchGameplay(a);
        if (StartsWithI(a, "RunMission=")) return LaunchGameplay(a);
        if (a == "fe_quitgame") { g.Quit(); return; }  // 0x8ac258 vtbl+0x10
        // Back on the root screen (0x554e71): GUI vtbl+0x30(1, "QuitCon"),
        // "Quit DRIV3R?" over the main menu (Enter quits, Escape returns).
        if (a == "exitmenu") { menu_.PushByName("QuitCon"); return; }

        // Take A Ride
        if (a == "fe_setupmood" || a == "fe_playmood" || a == "fe_playweather") return PreviewRide();
        if (_stricmp(a.c_str(), "fe_playVehicle") == 0) return PreviewVehicle();  // 0x553487
        // fe_/dg_ next/prev city (0x55351d / 0x553586) share the city (+0x10).
        if (a == "fe_nextcity" || a == "dg_nextcity" || a == "fe_prevcity" || a == "dg_prevcity") {
            Changed();
            StepCity(a[3] == 'n');
            return a[0] == 'f' ? PreviewRide() : PreviewDg();  // "fe" prefix: 0x555c40, else 0x555e20
        }
        if (a == "nexttime" || a == "prevtime") { Changed(); Step(time_, 4, a[0] == 'n'); return PreviewRide(); }
        if (a == "nextweather" || a == "prevweather") { Changed(); Step(weather_, 3, a[0] == 'n'); return PreviewRide(); }
        if (a == "fe_nextVehicle" || a == "fe_prevVehicle") { Changed(); StepVehicle(a[3] == 'n'); return PreviewVehicle(); }
        if (a == "cops_toggle") { Changed(); cops_ = !cops_; return; }
        // Start points (screen 3 'TAR_Sta', front end +0xc8): 4 per city, 3 with a boat.
        if (a == "fe_enter_sp_screen") { startPoint_ = 0; return; }  // 0x553185
        if (a == "nextstartpoint") {  // 0x55319f
            Changed();
            if (++startPoint_ >= (BoatSelected() ? 3 : 4)) startPoint_ = 0;
            return;
        }
        if (a == "prevstartpoint") {  // 0x5531e9
            Changed();
            if (--startPoint_ < 0) startPoint_ = BoatSelected() ? 2 : 3;
            return;
        }
        // "<city>_sp<N>" / "boat_<city>_sp<N>" (0x553246..0x55330e): point N-1, the city is not checked.
        for (const char* p : { "boat_istan_sp", "istan_sp", "boat_miami_sp", "miami_sp", "boat_nice_sp", "nice_sp" })
            if (StartsWithI(a, p)) { Changed(); startPoint_ = atoi(a.c_str() + strlen(p)) - 1; return; }

        // Driving Games
        if (a == "fe_setupdg" || a == "fe_playdg") return PreviewDg();
        if (a == "nextdg" || a == "prevdg") { Changed(); Step(dg_, 6, a[0] == 'n'); return PreviewDg(); }
        if (a == "nextsubgame" || a == "prevsubgame") { Changed(); Step(subgame_, 3, a[0] == 'n'); return; }

        // Options: Sound (settings handler 0x548b77..0x5492fb)
        if (a == "volup" || a == "voldown") return StepVolume(g.vol.effects, a == "volup");
        if (a == "musicup" || a == "musicdown") return StepVolume(g.vol.music, a == "musicup");
        if (a == "overallvol_up" || a == "overallvol_down") return StepVolume(g.vol.overall, a == "overallvol_up");

        // Settings screens. The settings handler compares only the prefix
        // (strncmp 15/16 at 0x549584/0x5495d4), so "settings_restore_no_dropback"
        // (the screens' exit action) also reverts. Accept commits (0x548300) and
        // the front end then goes back one screen (0x554da8).
        if (StartsWithI(a, "settings_accept")) { SaveProfile(); menu_.Pop(); return; }
        if (StartsWithI(a, "settings_restore")) {
            ApplyProfile();  // 0x548840
            g_music.UpdateVolume();
            // The front end (0x554de1 / 0x554e06) puts the shown device back
            // from +0x4c; the device being read is not changed back.
            joyIndex_ = joyBackup_;
            if (a == "settings_restore") menu_.Pop();
            return;
        }
        // Restore-defaults popup (screen 'Restore'): restore_enter_* selects the
        // page (0x5555xx, this+0xac8), restore_accept runs its defaults routine
        // (jump table at 0x5558fc) and goes back.
        static const char* kRestorePages[] = { "restore_enter_game", "restore_enter_cheats", "restore_enter_controls",
                                               "restore_enter_controller", "restore_enter_sound", "restore_enter_scard",
                                               "restore_enter_appearance", "restore_enter_vcard" };
        for (int i = 0; i < 8; ++i)
            if (a == kRestorePages[i]) { restorePage_ = i; return; }
        if (a == "restore_accept") {
            if (restorePage_ == 0) {  // 0x5497f0: Game screen (+0x15 single press = 0 is not kept here)
                g.toggles["weapon"] = true;
                g.toggles["autoaim"] = false;
                g.toggles["mph"] = true;
                g.toggles["tc_pressure_slomo"] = true;
                g.toggles["sub"] = TerritoryId() == 2 || TerritoryId() == 3;  // renderer vtbl+0x48
                g.motionBlur = 0.6f;
            }
            if (restorePage_ == 2) {  // 0x549880: vibration (platform default, taken as on), inverted look, 50
                g.toggles["vib"] = true;
                g.toggles["inv"] = true;
                g.turn = 50;
            }
            if (restorePage_ == 3) g.ResetKeyMap();  // 0x5498b0: "keymap_reset" (dead zone not used here)
            if (restorePage_ == 4) {  // 0x5498e0: overall 100, effects 100, music 60
                g.vol = App::Volumes();
                g_music.UpdateVolume();
            }
            if (restorePage_ == 5) {  // 0x549920: first driver (name resolved below), 44100
                g.soundDriver = 0;
                g.soundDriverName.clear();
                g.mixRate = 44100;
            }
            if (restorePage_ == 6) g.gamma = 1.0f;  // 0x549950: aspect read back from the renderer (kept), gamma 1
            if (restorePage_ == 7) {  // 0x549980: default mode, draw distance and filter 1, these flags
                g.res = DefaultMode(g.modes);
                g.drawdist = g.filter = 1;
                for (const char* k : { "shadows", "fulljointphysics", "synceveryframe", "particle" }) g.toggles[k] = true;
                g.toggles["advshaders"] = g.toggles["antialias"] = false;
            }
            // Every defaults routine ends with 0x548300(this, 0): the languages
            // go back to the saved ones (0x54830f), then the defaults become the
            // saved block (so Escape keeps them), without writing the file.
            int audio = LanguageIndex(uint32_t(g_profile.Int(0x28))), text = LanguageIndex(uint32_t(g_profile.Int(0x2c)));
            if (audio >= 0) g.audioSetting = audio;
            if (text >= 0) g.textSetting = text;
            SaveProfile(false);
            menu_.Pop();
            return;
        }

        // Options: Game (screen 15). Motion blur +0xc: steps of 0.1, at most
        // 0.8 and at least 0.0, then menu sound 0 (0x548869 / 0x5488af).
        if (a == "tc_motionblur_up") { g.motionBlur = std::min(0.8f, g.motionBlur + 0.1f); return Changed(); }
        if (a == "tc_motionblur_down") { g.motionBlur = std::max(0.0f, g.motionBlur - 0.1f); return Changed(); }

        // Options: Controls (screen 13). Look sensitivity: +0x17c, step 5, 0..100,
        // then menu sound 0 (0x54908e / 0x5490cf).
        if (a == "increaseturn" || a == "decreaseturn") {
            g.turn = std::max(0, std::min(100, g.turn + (a[0] == 'i' ? 5 : -5)));
            return Changed();
        }
        // Controller Setup (screen 35, 0x554e20..). The front end shows device
        // +0x48; joystick_backup copies it to +0x4c (on the way in). Prev /
        // Next wrap within the count and select that device for reading
        // (0x5522b0).
        if (a == "joystick_backup") { joyBackup_ = joyIndex_; return; }
        if (a == "Prev_Controller" || a == "Next_Controller") {
            int n = g_joy.Count();
            if (a[0] == 'P') { if (--joyIndex_ < 0) joyIndex_ = n - 1; }
            else if (++joyIndex_ >= n) joyIndex_ = 0;
            if (joyIndex_ < 0) joyIndex_ = 0;
            g_joy.Select(joyIndex_);
            return;
        }
        // keymap_start1..4 (0x555012): open the key list (screen 31) on a group.
        if (StartsWithI(a, "keymap_start") && a.size() == 13) {
            km_ = KeyEditor();
            km_.group = a[12] - '1';
            km_.count = KeyRowCount(g.keymap, km_.group);
            return;
        }
        // Key list events (0x549ff0): scroll, choose a row, accept.
        if (a == "km_pageup") { if (km_.offset > 0) --km_.offset; return; }
        if (a == "km_pagedown") { if (km_.offset < km_.count - 10) ++km_.offset; return; }
        if (StartsWithI(a, "km_a")) {  // "km_aNN" -> wait for a key; the link opens screen 34
            km_.sel = atoi(a.c_str() + 4) + km_.offset - 1;
            km_.state = 1;
            km_.since = GetTickCount();
            km_.blink = 0;
            return;
        }
        // The map is copied into the live settings (+0x208) after every change,
        // so here it already is; the _dropback version also goes back.
        if (a == "keymap_changed") return;
        if (a == "keymap_changed_dropback") { menu_.Pop(); return; }
        // Back-arrow icons that only the mouse can reach (0x554e47): GUI
        // vtbl+0x2c(1), back one screen.
        if (a == "mouseclick_dropback") { menu_.Pop(); return; }
        if (a == "keymap_reset") { g.ResetKeyMap(); return; }

        // Sound card (screen 30). Driver: -1 (no sound) .. count-1, wrapping,
        // and the name follows (0x548e01 / 0x548e55). Mixer rate: 11025,
        // 22050, 44100, 48000, wrapping (0x548ea9 / 0x548f0b).
        if (a == "Prev_SoundDriver" || a == "Next_SoundDriver") {
            int n = int(g.soundDrivers.size());
            if (a[0] == 'P') g.soundDriver = g.soundDriver - 1 < -1 ? n - 1 : g.soundDriver - 1;
            else g.soundDriver = g.soundDriver + 1 == n ? -1 : g.soundDriver + 1;
            g.soundDriverName = g.DriverName(g.soundDriver);
            return Changed();
        }
        if (a == "prev_soundfreq" || a == "next_soundfreq") {
            int r = g.mixRate;
            if (a[0] == 'p') g.mixRate = r == 11025 ? 48000 : r == 22050 ? 11025 : r == 44100 ? 22050 : 44100;
            else g.mixRate = r == 11025 ? 22050 : r == 22050 ? 44100 : r == 44100 ? 48000 : 11025;
            return Changed();
        }

        // Options: Display (screen 12, settings handler 0x548f93..). The aspect is
        // set on the renderer (vtbl+0x20) and read back (vtbl+0x1c); the
        // handlers only choose 0 (4:3) and 2 (16:9).
        if (a == "next_aspect") {
            if (g.aspect == 0 || g.aspect == 1) g.aspect = 2;
            else if (g.aspect == 2) g.aspect = 0;
            return Changed();
        }
        if (a == "prev_aspect") {
            if (g.aspect == 0) g.aspect = 2;
            else if (g.aspect == 1 || g.aspect == 2) g.aspect = 0;
            return Changed();
        }
        // Gamma +0xc30: steps of 0.1 within 0.5..6.0 (0x549302 / 0x54933c).
        if (a == "gamma_up") { g.gamma = std::min(6.0f, g.gamma + 0.1f); return Changed(); }
        if (a == "gamma_down") { g.gamma = std::max(0.5f, g.gamma - 0.1f); return Changed(); }
        // "Advanced Video Card Settings": no handler in the executable; the
        // menu link opens screen 29.
        if (a == "initial_res_check") return;
        // Video card settings (screen 29). Resolution: next / previous entry of
        // the mode list, wrapping (0x54a830 / 0x54a860).
        if (a == "next_res" || a == "prev_res") {
            if (g.modes.empty()) return;
            int n = int(g.modes.size()), i = FindMode(g.modes, g.res);
            i = a[0] == 'n' ? (i + 1 < n ? i + 1 : 0) : (i - 1 >= 0 ? i - 1 : n - 1);
            g.res = g.modes[i];
            return Changed();
        }
        // Draw distance +0xc88 and filter +0xc90: 0..2, wrapping (0x5493e0.., 0x5494d7..).
        if (a == "drawdist_up" || a == "drawdist_down") { Step(g.drawdist, 3, a == "drawdist_up"); return Changed(); }
        if (a == "filter_up" || a == "filter_down") { Step(g.filter, 3, a == "filter_up"); return Changed(); }

        // Options: language
        // Audio (0x548a92 / 0x548ad2): +0x28 changes and the audio language in
        // use follows at once (vtbl+0x64). Text (0x548af0 / 0x548b0e): +0x2c
        // changes, but the handler then passes +0x28, the audio language, to
        // the text setter (vtbl+0x54 at 0x548b39). So the menu text changes
        // only on Accept / Escape, when 0x548300 sets both from the settings.
        // Kept as in the original. Both then play menu sound 0.
        if (a == "nextlangaudio" || a == "prevlangaudio") {
            g.audioSetting = StepLanguage(g.audioSetting, a[0] == 'n');
            g.SetAudioLanguage(g.audioSetting);
            return Changed();
        }
        if (a == "nextlangtext" || a == "prevlangtext") {
            g.textSetting = StepLanguage(g.textSetting, a[0] == 'n');
            g.SetTextLanguage(g.audioSetting);
            return Changed();
        }

        // Extras / cutscenes (FMV only, no gameplay)
        if (a == "extra_credits") return PlayExtra("FMV\\CREDITS.XMV");
        if (a == "extra_makingof") return PlayExtra("FMV\\THEMAKINGOF.XMV");
        if (a == "extra_boiling") return PlayExtra("FMV\\BOILINGPOINT.XMV");
        if (a == "extra_pirates") {
            static const char* byLang[] = { "FMV\\PIRATES.XMV", "FMV\\PIRATES_FRE.XMV", "FMV\\PIRATES_GER.XMV",
                                            "FMV\\PIRATES_ITA.XMV", "FMV\\PIRATES_SPA.XMV" };
            return PlayExtra(byLang[g.textSetting]);  // by +0x2c (0x553d6c)
        }
        if (a == "cut recap") return PlayExtra("FMV\\RECAP.XMV");
        if (StartsWithI(a, "cut ")) return PlayExtra("FMV\\SC" + a.substr(4) + ".XMV");  // "cut 05a" -> SC05a

        // Generic on/off settings: "<x>_toggle" <-> "stat_<x>" / "<x>_onoff".
        if (a.size() > 7 && a.compare(a.size() - 7, 7, "_toggle") == 0) {
            std::string key = a.substr(0, a.size() - 7);
            g.toggles[key] = !g.Toggle(key);
            Changed();
        }
    }

    // Button state for "stat_*" queries: 0 = first row (selected / on),
    // 1 = second row, n = row n. Unknown names keep the current state.
    int Status(const std::string& s, int st)
    {
        if (s == "stat_city") return city_;
        if (s == "stat_weather") return weather_;
        if (s == "stat_dawn") return time_ == 0 ? 0 : 1;
        if (s == "stat_day") return time_ == 1 ? 0 : 1;
        if (s == "stat_dusk") return time_ == 2 ? 0 : 1;
        if (s == "stat_night") return time_ == 3 ? 0 : 1;
        if (s == "stat_cops") return cops_ ? 0 : 1;
        // Start-point markers (0x553316..0x553431): only the current city's
        // markers for the selected vehicle type (boat or not) are shown; the
        // selected point uses row 0, the others row 1.
        {
            static const char* kCity[] = { "miami", "nice", "istan" };
            for (int c = 0; c < 3; ++c) {
                std::string land = std::string("stat_") + kCity[c] + "_sp";
                std::string boat = std::string("stat_") + kCity[c] + "_boat_sp";
                bool isBoat = StartsWithI(s, boat.c_str());
                if (!isBoat && !StartsWithI(s, land.c_str())) continue;
                if (city_ != c || BoatSelected() != isBoat) return 2;
                return startPoint_ == atoi(s.c_str() + (isBoat ? boat : land).size()) - 1 ? 0 : 1;
            }
        }
        if (s == "stat_subgame") return subgame_;
        // stat_restoretitle1..4 (0x55581f..): title n shows row 0 for page
        // 2(n-1), row 1 for page 2(n-1)+1, and nothing (empty row 2) otherwise.
        if (StartsWithI(s, "stat_restoretitle") && s.size() == 18) {
            int first = (s[17] - '1') * 2;
            return restorePage_ == first ? 0 : restorePage_ == first + 1 ? 1 : 2;
        }
        // Controls: the inverted-look row asks "stat_vert" for inv_toggle (+0x175).
        if (s == "stat_vert") return g.Toggle("inv") ? 0 : 1;
        // Controller Setup (count at 0x8ac2b8+0x6d0): "No Device Detected"
        // is hidden when there is a device, the device arrows are shown only
        // for more than one (0x554eec, 0x554fd6).
        if (s == "stat_no_ctrl_detected") return g_joy.Count() > 0 ? 2 : st;
        if (s == "stat_controllercount_arrow") return g_joy.Count() > 1 ? 0 : 2;
        // Key list (0x54a0a7..0x54a2f5).
        if (s == "stat_km_pageup") return km_.offset > 0 ? 0 : 2;
        if (s == "stat_km_pagedown") return km_.offset < km_.count - 10 ? 0 : 2;
        if (StartsWithI(s, "stat_keymap_t") && s.size() == 14) {  // group title: t1 groups 0/1, t2 groups 2/3
            int first = (s[13] - '1') * 2;
            return km_.group == first ? 0 : km_.group == first + 1 ? 1 : 2;
        }
        if (StartsWithI(s, "stat_keymap_") && s.size() == 15) {  // stat_keymap_{a,p,s}NN
            int row = atoi(s.c_str() + 13) + km_.offset - 1;
            if (km_.state == 1) {  // waiting: the chosen row is lit, its key cells blink
                if (row != km_.sel) return 1;
                return s[12] == 'a' ? 0 : km_.blink;
            }
            if (row >= km_.count) return 2;
            // The original leaves the state as it is. The key cells are never
            // focused, so put them back to normal: screen 31's cell p05 asks
            // for stat_keymap_p01 and would otherwise stay lit after blinking.
            return s[12] == 'a' ? st : 1;
        }
        // Display. Stat_Aspect (0x554829): 0 for aspect 0 or 1, 2 for 16:9.
        if (s == "stat_aspect") return g.aspect == 0 || g.aspect == 1 ? 0 : g.aspect == 2 ? 2 : st;
        // One label per value, the others hidden (0x5552fb.., 0x55545d..).
        static const char* kDrawDist[] = { "stat_drawdist_low", "stat_drawdist_medium", "stat_drawdist_high" };
        static const char* kFilter[] = { "stat_filter_none", "stat_filter_bi", "stat_filter_tri" };
        for (int i = 0; i < 3; ++i) {
            if (s == kDrawDist[i]) return g.drawdist == i ? 0 : 2;
            if (s == kFilter[i]) return g.filter == i ? 0 : 2;
        }
        // Without anti-aliasing (+0x110) its row is hidden (0x55552f, 0x555563).
        if (s == "stat_enable_antialias") return g.antialiasOk ? st : 2;
        if (s == "stat_antialias") return g.antialiasOk ? (g.Toggle("antialias") ? 0 : 1) : 2;
        // Sound card: "No Sound" only for driver -1 (0x55474f), the name row
        // hidden then (0x554788), the arrows only with more than one driver
        // (0x5551ee).
        if (s == "stat_sd_nosound") return g.soundDriver < 0 ? 0 : 2;
        if (s == "stat_sounddriver_txt") return g.soundDriver >= 0 ? st : 2;
        if (s == "stat_sd_count_arrow") return g.soundDrivers.size() > 1 ? 0 : 2;
        static const char* kDg[] = { "stat_dg chase", "stat_dg getaway", "stat_dg trail",
                                     "stat_dg survival", "stat_dg check", "stat_dg gate" };
        for (int i = 0; i < 6; ++i)
            if (s == kDg[i]) return dg_ == i ? 0 : 1;
        // Stat_<Language>Text / Audio (0x554374.., 0x55416e..): row 0 when
        // the text (+0x2c) / audio (+0x28) setting is that language id, else
        // row 1. The exe has all nine; only the five European ones can be
        // chosen with this game's Locale folders.
        static const struct { const char* name; uint32_t id; } kLangStat[] = {
            { "english", 0 }, { "french", 1 }, { "german", 2 }, { "spanish", 3 }, { "italian", 4 },
            { "japanese", 5 }, { "chinese", 6 }, { "korean", 8 }, { "portuguese", 9 },
        };
        for (const auto& l : kLangStat) {
            std::string n = std::string("stat_") + l.name;
            if (s == n + "text") return kLanguages[g.textSetting].id == l.id ? 0 : 1;
            if (s == n + "audio") return kLanguages[g.audioSetting].id == l.id ? 0 : 1;
        }
        // Progress (0x553680..0x553e4b): a locked item shows row 2 ("Locked"
        // or a dimmed copy), an unlocked one keeps its state.
        if (StartsWithI(s, "stat_mis")) return MissionUnlocked(atoi(s.c_str() + 8)) ? st : 2;  // 0x553b04
        if (StartsWithI(s, "stat_sc")) {  // 0x553c36: "%s" into a 4-byte buffer -> table 0x7a20e8
            std::string id = s.substr(7, 3);
            for (const CutsceneUnlock& c : kCutscenes)
                if (_stricmp(id.c_str(), c.id) == 0) return MissionUnlocked(c.mission) ? st : 2;
            return st;
        }
        if (s == "stat_undernice") return Progress() >= 11 ? st : 2;  // 0x5536b7
        if (s == "stat_underinst") return Progress() >= 22 ? st : 2;  // 0x5536fb
        if (s == "stat_recap") return Progress() >= 32 ? st : 2;      // 0x553cd4
        // The two documentaries are hidden in territory 3 (0x553d17, 0x553e10).
        if (s == "stat_pirates" || s == "stat_boiling") return TerritoryId() == 3 ? 2 : st;
        // The Language entry of 'Opt_Set' is dimmed in territory 2 (0x55515e).
        if (s == "stat_language_settings") return TerritoryId() == 2 ? 2 : st;
        // stat_unlock, stat_city_arrows (TAR_Mai) and the events Main_Options
        // (Main, Options button) and restore_cancel ('Restore' back event) are
        // in FRONT.MEC but not in the exe: they keep their state / do nothing.
        // Cheats: 10 pairs at +0x19e {unlocked, enabled}; the enabled bytes
        // are the "<cheat>_toggle" flags (allmissions +0x19f, weapons +0x1a1, ...).
        if (s == "stat_cheats") {  // 0x554d31 -> 0x5480f0: any cheat unlocked
            for (int i = 0; i < 10; ++i)
                if (CheatUnlocked(i)) return st;
            return 2;
        }
        if (StartsWithI(s, "stat_cheat")) {  // 0x554d58: stat_cheat0..9, other numbers -> 2 (0x553b34)
            unsigned i = unsigned(atoi(s.c_str() + 10));
            return i < 10 && CheatUnlocked(int(i)) ? st : 2;
        }
        // "<cheat>_onoff" (0x554b45..0x554ce9): 2 while the cheat is locked,
        // else 0 when enabled, 1 when not (checked on the original).
        static const struct { const char* name; int index; } kCheatOnOff[] = {
            { "allmissions", 0 }, { "weapons", 1 }, { "ammo", 2 },
            { "invincibility", 3 }, { "immunity", 4 }, { "vehicle", 6 },
        };
        for (const auto& c : kCheatOnOff)
            if (_stricmp(s.c_str(), (std::string(c.name) + "_onoff").c_str()) == 0)
                return !CheatUnlocked(c.index) ? 2 : g.Toggle(c.name) ? 0 : 1;  // enabled byte +0x19f + 2 x index
        if (StartsWithI(s, "stat_")) {
            std::string key = s.substr(5);
            if (g.toggles.count(key)) return g.Toggle(key) ? 0 : 1;
        }
        if (s.size() > 6 && s.compare(s.size() - 6, 6, "_onoff") == 0) {
            std::string key = s.substr(0, s.size() - 6);
            if (g.toggles.count(key)) return g.Toggle(key) ? 0 : 1;
        }
        return st;
    }

    // ---- helpers ---------------------------------------------------------
    // Settings +0x20: furthest mission reached. Mission script steps of type 3
    // raise it to the step's value (stores at 0x557fa6, 0x558036) unless flag +0x5 is set.
    static int Progress() { return AllMissions() ? 0x7fffffff : g_profile.Int(0x20); }
    // 0x548040: unlocked when the "all missions" cheat (+0x19f) is on or n <= progress.
    static bool MissionUnlocked(int n) { return AllMissions() || uint32_t(n) <= uint32_t(g_profile.Int(0x20)); }
    // Live +0x19f; g.Toggle() would read a missing key (no profile) as on.
    static bool AllMissions() { auto it = g.toggles.find("allmissions"); return it != g.toggles.end() && it->second; }
    static bool CheatUnlocked(int i) { return g_profile.Flag(0x19e + 2 * i); }
    // Istanbul can be chosen with the all-missions cheat or settings +0x24,
    // which a mission-script step of type 2 with value 5 or 6 sets (0x557f83).
    static bool IstanbulOpen() { return AllMissions() || g_profile.Flag(0x24); }

    // City change (0x55351d next, 0x553586 prev): Miami -> Nice -> Istanbul,
    // skipping a closed Istanbul; then 0x555bb0 picks the city's first
    // vehicle and start point 0.
    void StepCity(bool next)
    {
        if (next) city_ = city_ == 0 ? 1 : city_ == 1 && IstanbulOpen() ? 2 : 0;
        else city_ = city_ == 0 ? (IstanbulOpen() ? 2 : 1) : city_ == 1 ? 0 : 1;
        if (city_ == 2 && !IstanbulOpen()) city_ = 0;
        vehicle_ = 0;
        startPoint_ = 0;
    }

    int VehicleId() const { return kRideCities[city_].list[vehicle_].id; }
    bool BoatSelected() const { return IsBoat(VehicleId()); }
    // 0x549d70: the "vehicle" cheat (+0x1ab, i.e. +0x174+0x37) opens all of them.
    static bool VehicleUnlocked(int id)
    {
        auto it = g.toggles.find("vehicle");
        if (it != g.toggles.end() && it->second) return true;
        for (const VehicleLock& l : kVehicleLocks)
            if (l.id == id) return g_profile.Flag(l.off);
        return true;
    }
    // fe_nextVehicle (0x555a30) / fe_prevVehicle (0x555ab0): step through the
    // city's list with wrap-around until an unlocked vehicle is found. The
    // first vehicle of each list is never locked, so the loop ends.
    void StepVehicle(bool next)
    {
        const RideCity& c = kRideCities[city_];
        do vehicle_ = (vehicle_ + (next ? 1 : c.count - 1)) % c.count;
        while (!VehicleUnlocked(c.list[vehicle_].id));
    }
    // 0x555b30: level id for StartGame; vehicle 19 has its own level per city.
    int RideLevel() const { return (VehicleId() == 19 ? 0x4e : 0x4d) + 3 * city_; }
    std::string Top()
    {
        MecScreen* s = menu_.Top();
        return s ? s->name : std::string();
    }
    // Value changed: the front-end handlers play menu sound 0 (e.g. 0x552ef3).
    static void Changed() { g_sounds.Play(MenuSounds::MOVE); }
    static void Step(int& v, int n, bool next) { v = (v + (next ? 1 : n - 1)) % n; }
    // Steps of 5 clamped to 0..100; the handler applies the new volume and
    // then plays menu sound 0 (0x548be6), so the click uses the new level.
    static void StepVolume(int& v, bool up)
    {
        v = std::max(0, std::min(100, v + (up ? 5 : -5)));
        g_music.UpdateVolume();
        Changed();
    }

    // Action and button names: Text\controls.txt, reloaded with the language
    // (object 0x8ac598, 0x54bcc0).
    void LoadControlsText() { controlsText_ = LoadTxt(g.Resolve("LANG:TEXT\\CONTROLS.TXT")); }

    // txt_keymap_aNN / pNN / sNN (0x54a109..0x54a1dd): action, preferred key
    // and alternative (joystick / mouse) of row NN on the current page.
    bool KeyMapText(const std::string& fn, std::wstring& out)
    {
        if (!StartsWithI(fn, "txt_keymap_") || fn.size() != 14) return false;
        int half = 0;
        KeyRecord* r = KeyRow(g.keymap, km_.group, atoi(fn.c_str() + 12) + km_.offset - 1, half);
        out.clear();
        if (!r) return true;
        const KeyBind& b = r->bind[half];
        switch (fn[11]) {
        case 'a': out = TextOf(controlsText_, r->text[half]); break;
        case 'p': out = KeyName(b.key, controlsText_); break;
        case 's': out = b.altAxis ? AxisName(b.alt, b.dir, controlsText_) : AltName(b.alt, controlsText_); break;
        }
        return true;
    }

    // Waiting for a key on screen 34 (update 0x549e70). Escape (code 1)
    // cancels. A keyboard key first clears that code from the other rows of the
    // group (0x54a430), then becomes the row's preferred key (map vtbl+0x4c);
    // the editor then goes back one screen and sends keymap_changed. A
    // joystick button / POV direction (0x1000 + n) or a mouse button
    // (0x2000 + n) is cleared the same way and becomes the row's alternative
    // (vtbl+0x58, which also clears the axis flag); a joystick axis becomes
    // the row's axis alternative (CaptureAxis). The joystick cannot cancel.
    void CaptureKey(WPARAM vk)
    {
        if (vk == VK_ESCAPE) {
            km_.state = 0;
            menu_.OnKey(vk);  // back out of the popup
            return;
        }
        UINT sc = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC_EX);
        int code = int((sc & 0x7f) | ((sc & 0xff00) ? 0x80 : 0));  // scan code -> DIK_*
        if ((sc & 0x7f) == 0) return;
        ClearCode(code);
        int half = 0;
        if (KeyRecord* r = KeyRow(g.keymap, km_.group, km_.sel, half)) r->bind[half].key = code;
        Log("Key map: group %d row %d = 0x%02X", km_.group, km_.sel, code);
        EndCapture();
    }
    void CaptureAlt(int code)
    {
        ClearCode(code);
        int half = 0;
        if (KeyRecord* r = KeyRow(g.keymap, km_.group, km_.sel, half)) {
            r->bind[half].altAxis = 0;
            r->bind[half].alt = code;
        }
        Log("Key map: group %d row %d alt = 0x%04X", km_.group, km_.sel, code);
        EndCapture();
    }
    // 0x54a4a0: rows of the group with the same axis and direction get axis
    // 11 (<None>), direction 0 (the axis flag stays set); then vtbl+0x60
    // (0x54b1b0) sets the row's axis flag, axis and direction.
    void CaptureAxis(int axis, bool dir)
    {
        for (int row = 0; row < km_.count; ++row) {
            int half = 0;
            KeyRecord* r = KeyRow(g.keymap, km_.group, row, half);
            if (!r) continue;
            KeyBind& b = r->bind[half];
            if (b.altAxis == 1 && b.alt == axis && b.dir == (dir ? 1 : 0)) { b.alt = 11; b.dir = 0; }
        }
        int half = 0;
        if (KeyRecord* r = KeyRow(g.keymap, km_.group, km_.sel, half)) {
            r->bind[half].altAxis = 1;
            r->bind[half].alt = axis;
            r->bind[half].dir = dir ? 1 : 0;
        }
        Log("Key map: group %d row %d axis = %d %s", km_.group, km_.sel, axis, dir ? "+" : "-");
        EndCapture();
    }
    // 0x54a430: the new code is removed from every row of the group.
    void ClearCode(int code)
    {
        for (int row = 0; row < km_.count; ++row) {
            int half = 0;
            KeyRecord* r = KeyRow(g.keymap, km_.group, row, half);
            if (!r) continue;
            KeyBind& b = r->bind[half];
            if (b.key == code) b.key = 0x4000;
            if (!b.altAxis && b.alt == code) b.alt = 0x4000;
        }
    }
    void EndCapture()
    {
        km_.state = 0;
        menu_.Pop();
        Action("keymap_changed");
    }

    void PlayBackground(const std::string& file)
    {
        std::string path = g.Resolve(file);
        if (_stricmp(bg_.Path().c_str(), path.c_str()) == 0 && bg_.IsOpen()) return;
        bg_.Open(path, g.AudioTrack(), true);
    }
    void PlayPane(const std::string& path)
    {
        if (_stricmp(pane_.Path().c_str(), path.c_str()) == 0 && pane_.IsOpen()) return;
        if (FileExists(path)) pane_.Open(path, g.AudioTrack(), true);
    }
    // "FMV\" + MIAMI_/NICE_/ISTAN_ + DAWN_/DAY_/DUSK_/NIGHT_ + DRY/OVR/WET + ".XMV"
    void PreviewRide()
    {
        static const char* city[] = { "MIAMI_", "NICE_", "ISTAN_" };
        static const char* time[] = { "DAWN_", "DAY_", "DUSK_", "NIGHT_" };
        static const char* wthr[] = { "DRY", "OVR", "WET" };
        PlayPane(std::string("FMV\\") + city[city_] + time[time_] + wthr[weather_] + ".XMV");
    }
    // 0x8ac3bc vtbl+0x10 with the node's video (+0x8).
    void PreviewVehicle()
    {
        char f[64];
        snprintf(f, sizeof(f), "FMV\\%sCAR%02d.XMV", kRideCities[city_].prefix, kRideCities[city_].list[vehicle_].car);
        PlayPane(f);
    }
    // FUN_00555e20: "FMV\" + MIAMI/NICE/BUL + CHASE/GETAWAY/... + ".XMV"
    void PreviewDg()
    {
        static const char* city[] = { "MIAMI", "NICE", "BUL" };
        static const char* game[] = { "CHASE", "GETAWAY", "TRAILBLZ", "SURVIVAL", "CHECKPNT", "GATERACE" };
        PlayPane(std::string("FMV\\") + city[city_] + game[dg_] + ".XMV");
    }

    void PlayExtra(const std::string& file)
    {
        bg_.Close();
        pane_.Close();
        g_music.Stop();  // assumption: not traced, but the extras have their own soundtrack
        if (!extra_.Open(g.Resolve(file), g.AudioTrack(), false)) return EndExtra();
        extraSubs_.Load(file);  // cutscenes, recap, making-of, ... (0x551220)
    }
    void EndExtra()
    {
        extra_.Close();
        extraSubs_.Clear();
        g_music.Play();  // like 0x54b9a0: restart the music if it is not playing
        // Re-run the screen's enter action to restore its background video.
        if (MecScreen* s = menu_.Top()) Action(s->onEnter.empty() ? "Movie=FMV\\FRONTEND.XMV" : s->onEnter);
        if (!bg_.IsOpen()) PlayBackground("FMV\\FRONTEND.XMV");
    }

    // FUN_00557d70: setup object (0x8ac3ac) vtbl+8, then state 0xB3.
    void LaunchGameplay(const std::string& what)
    {
        // StartGame (0x555920) hands these to the setup object 0x8ac3ac.
        if (_stricmp(what.c_str(), "StartGame") == 0)
            Log("Take A Ride: city %d, time %d, weather %d, vehicle %d (car %d), cops %d, start point %d, level 0x%x",
                city_, time_, weather_, VehicleId(), kRideCities[city_].list[vehicle_].car, cops_ ? 1 : 0,
                startPoint_, RideLevel());
        g_gameplay.request = what;
        g.Goto(ST_GAMEPLAY);
    }

    MecMenu menu_;
    Movie bg_, pane_, extra_;
    Subtitles extraSubs_;
    // City +0x10 (also used by Driving Games), time +0x14, weather +0x18,
    // vehicle = index into the city's list (node pointer +0x75c), start point +0xc8.
    int city_ = 0, time_ = 1, weather_ = 0, vehicle_ = 0, startPoint_ = 0;
    bool cops_ = true;
    int dg_ = 0, subgame_ = 0;
    int restorePage_ = 0;     // page for the restore-defaults popup
    // Key-map editor (front end +0x6a0; init 0x549e40): group, its row count,
    // first row shown, 1 = waiting for a key, chosen row, blink phase.
    struct KeyEditor { int group = 0, count = 0, offset = 0, state = 0, sel = 0, blink = 0; DWORD since = 0; } km_;
    int joyIndex_ = 0, joyBackup_ = 0;  // Controller Setup: shown device +0x48, its backup +0x4c
    TextTable controlsText_;
};

// GUI direction inputs 2..5 (screen input 0x469d20): the menu reads key OR
// POV through one mapper input, so both share one repeat counter: act on a
// new press (counter 30), then while held every 5 updates (value 1.0 >
// 0.7). At ~120 updates per second: 250 ms, then 42 ms. Select and back
// never repeat, so Windows' own key repeat is not used.
// Times in ms from the performance counter: GetTickCount steps by ~15.6 ms,
// which made the 42 ms repeat run slow. Repeats keep a fixed cadence (next
// += kNext), like the original's per-update counter.
struct GuiRepeat
{
    static constexpr double kFirst = 30 * 1000.0 / 120, kNext = 5 * 1000.0 / 120;
    bool key[4] = {};   // arrow key held (WM_KEYDOWN .. WM_KEYUP)
    double next[4] = {};
    static double Now()
    {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return double(c.QuadPart) * 1000.0 / double(f.QuadPart);
    }
    static int Index(WPARAM vk)
    {
        return vk == VK_UP ? 0 : vk == VK_DOWN ? 1 : vk == VK_LEFT ? 2 : vk == VK_RIGHT ? 3 : -1;
    }
    bool Held(int i) const { return key[i] || g_joy.Down(i); }
};
static GuiRepeat g_repeat;

// ============================================================ window ========
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN: {
        if (lp & (1 << 30)) return 0;  // bit 30: Windows' key repeat; repeats come from g_repeat
        int i = GuiRepeat::Index(wp);
        if (i >= 0) {
            // Already down through the POV: no new press for the mapper input.
            bool was = g_repeat.Held(i);
            g_repeat.key[i] = true;
            if (was) return 0;
            g_repeat.next[i] = GuiRepeat::Now() + GuiRepeat::kFirst;
        }
        if (!g.cur) return 0;
        int sc = int((lp >> 16) & 0x7f), ext = int((lp >> 24) & 1);
        g.cur->OnKeyPress(sc | (ext ? 0x80 : 0));  // scan code -> DIK_*
        if (g.cur) g.cur->OnKey(wp);
        return 0;
    }
    case WM_KEYUP: {
        int i = GuiRepeat::Index(wp);
        if (i >= 0) g_repeat.key[i] = false;
        return 0;
    }
    case WM_KILLFOCUS:  // DirectInput (foreground) stops reporting keys
        for (bool& k : g_repeat.key) k = false;
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_F4) { g.Quit(); return 0; }
        break;
    // NOTE: deliberate difference. In the original the GUI mouse path is
    // dead: the input object's +0x28 is zeroed at init (0x581060) and never
    // set, so 0x581560 posts nothing; the pointer is hidden (WM_SETCURSOR ->
    // SetCursor(NULL)) and hover / click do nothing (checked on the game).
    // Here the dormant path is enabled and the arrow cursor is kept.
    // Mouse (0x576650): position / client size, then the input object
    // 0x8ac270 posts a GUI event: move 11 (only if the position changed),
    // left / middle / right down 0 / 1 / 2, up 3 / 4 / 5, wheel 9 (no
    // direction is passed). The class has no CS_DBLCLKS, so no
    // double-click codes (6..8). Buttons are also polled through
    // DirectInput (0x583a00), which sees the X buttons too.
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
    case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP:
    case WM_MOUSEWHEEL: case WM_XBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (msg == WM_MOUSEWHEEL) ScreenToClient(hwnd, &p);  // the wheel gives screen coordinates
        if (msg == WM_MOUSEMOVE) {
            if (p.x == g.mouseX && p.y == g.mouseY) return 0;
            g.mouseX = p.x;
            g.mouseY = p.y;
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        float x = float(p.x) / float(rc.right > 0 ? rc.right : 1);
        float y = float(p.y) / float(rc.bottom > 0 ? rc.bottom : 1);
        if (!g.cur) return 0;
        int button = msg == WM_LBUTTONDOWN ? 0 : msg == WM_RBUTTONDOWN ? 1 : msg == WM_MBUTTONDOWN ? 2
                   : msg == WM_XBUTTONDOWN ? (GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 3 : 4) : -1;
        if (button >= 0) g.cur->OnMouseButton(button);
        if (msg == WM_XBUTTONDOWN) return TRUE;
        int code = msg == WM_MOUSEMOVE ? 11 : msg == WM_LBUTTONDOWN ? 0 : msg == WM_MBUTTONDOWN ? 1
                 : msg == WM_RBUTTONDOWN ? 2 : msg == WM_LBUTTONUP ? 3 : msg == WM_MBUTTONUP ? 4
                 : msg == WM_RBUTTONUP ? 5 : 9;
        g.cur->OnMouse(code, x, y);
        return 0;
    }
    case WM_CLOSE:
        g.Quit();
        return 0;
    // Fullscreen: switching away gives the desktop its mode and gamma back
    // and minimises the window; coming back sets them again.
    case WM_ACTIVATEAPP:
        if (!kWindowed && g_display.applied) {
            if (!wp && g_display.active) {
                g_display.active = false;
                RestoreDisplay();
                ShowWindow(hwnd, SW_MINIMIZE);
            } else if (wp && !g_display.active) {
                g_display.active = true;
                ShowWindow(hwnd, SW_RESTORE);
                ApplyDisplay(true);
            }
        }
        break;
    case WM_DESTROY:
        RestoreDisplay();
        g.running = false;
        g.hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// After each joystick poll. First a newly pressed button / POV direction,
// else a newly pushed axis, goes to the state (attract counter, key-map
// editor, video skip). Then the GUI inputs: the menu reads them through
// the input mapper 0x4d9cc0 (vtbl 0x78777c) with scheme 0x161 (0x558470),
// which binds GUI inputs 2..7 to control ids 2..7 of the fixed table
// 0x7a4c78 (static initialiser 0x6dd790; not in the key map, so they
// cannot be changed): up / down / left / right = arrow key or POV 1 (codes
// 0..3), select = Enter or button 1 (code 4), back = Escape or button 2
// (code 5). The keyboard half comes in through WM_KEYDOWN; the joystick
// half is fed here as the same keys. Screen input 0x469d20: a direction
// acts when newly pressed and then, while held, after 30 updates and every
// 5 updates after that (10 for an analogue value <= 0.7, never the case
// for a POV); select and back act only when newly pressed. At ~120 updates
// per second that is 250 ms, then 42 ms. The analogue stick is not bound,
// so it does not move the menu.
static void DispatchJoystick()
{
    static const WPARAM kVk[6] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE };
    if (!g_joy.Poll() || !g.cur) return;
    Screen* s = g.cur;
    int code = 0, axis = 0;
    bool dir = false;
    bool used = g_joy.NewButton(code) ? s->OnJoyButton(code) : g_joy.NewAxis(axis, dir) && s->OnJoyAxis(axis, dir);
    if (used || g.cur != s) return;  // a new state sees the press as old (no edge)
    double now = GuiRepeat::Now();
    for (int i = 0; i < 6; ++i) {
        if (!g_joy.Down(i) || g_joy.WasDown(i)) continue;
        if (i < 4) {
            if (g_repeat.key[i]) continue;  // already down through the key
            g_repeat.next[i] = now + GuiRepeat::kFirst;
        }
        s->OnPad(kVk[i]);
        if (g.cur != s) return;
    }
}

// Repeats of held directions, keyboard or POV (see GuiRepeat).
static void UpdateGuiRepeat()
{
    static const WPARAM kVk[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    double now = GuiRepeat::Now();
    for (int i = 0; i < 4 && g.cur; ++i) {
        if (!g_repeat.Held(i) || now < g_repeat.next[i]) continue;
        g_repeat.next[i] += GuiRepeat::kNext;
        if (g_repeat.next[i] < now) g_repeat.next[i] = now + GuiRepeat::kNext;  // far behind (stall)
        Screen* s = g.cur;
        s->OnPad(kVk[i]);
    }
}

static void Present()
{
    if (!g.hwnd) return;
    std::fill(g.canvas.px, g.canvas.px + size_t(kWidth) * kHeight, 0xff000000u);
    if (g.cur) g.cur->Render(g.backDC, g.canvas);
    GdiFlush();
    HDC dc = GetDC(g.hwnd);
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    if (rc.right == kWidth && rc.bottom == kHeight) {
        BitBlt(dc, 0, 0, kWidth, kHeight, g.backDC, 0, 0, SRCCOPY);
    } else {  // the display setting's resolution: the frame is stretched to it
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, 0, 0, rc.right, rc.bottom, g.backDC, 0, 0, kWidth, kHeight, SRCCOPY);
    }
    ReleaseDC(g.hwnd, dc);
}

static std::string FindGameDir(const char* cmdLine)
{
    std::vector<std::string> candidates;
    std::string arg = cmdLine ? cmdLine : "";
    while (!arg.empty() && (arg.front() == ' ' || arg.front() == '"')) arg.erase(arg.begin());
    while (!arg.empty() && (arg.back() == ' ' || arg.back() == '"' || arg.back() == '\\')) arg.pop_back();
    if (!arg.empty()) candidates.push_back(arg);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string exeDir(exe);
    candidates.push_back(exeDir.substr(0, exeDir.find_last_of('\\')));
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    candidates.push_back(cwd);
    for (auto& c : candidates)
        if (FileExists(c + "\\FMV\\ATARI.XMV") && FileExists(c + "\\binkw32.dll")) return c;
    return std::string();
}

static std::string ReadTerritory()
{
    // game.ini contains e.g. "[EUROPE]" -> Territory\Europe
    std::string t = "Europe";
    FILE* f = fopen("game.ini", "rb");
    if (!f) return t;
    char buf[64] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char* a = strchr(buf, '[');
    const char* b = a ? strchr(a, ']') : nullptr;
    if (a && b && b > a + 1) {
        t.assign(a + 1, b);
        for (size_t i = 0; i < t.size(); ++i) t[i] = char(i ? tolower(t[i]) : toupper(t[i]));
    }
    return t;
}

// A crash would leave the monitor in the menu's mode: give the desktop its
// mode and gamma back first. (A process killed from outside cannot do this.)
static LONG WINAPI OnCrash(EXCEPTION_POINTERS*)
{
    RestoreDisplay();
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int showCmd)
{
    SetUnhandledExceptionFilter(OnCrash);
    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    strcpy(strrchr(logPath, '\\') + 1, "Driv3rMenu.log");
    g_log = fopen(logPath, "w");

    g.inst = inst;

    //g.gameDir = FindGameDir("D:\\l\\gta-vc\\DRIV3R");
    g.gameDir = FindGameDir(cmdLine);
    if (g.gameDir.empty()) {
        MessageBoxA(nullptr,
                    "Driv3r game folder not found.\n\nPut Driv3rMenu.exe in the DRIV3R folder or pass "
                    "the folder as the first argument.",
                    "Driv3r", MB_ICONERROR);
        return 1;
    }
    SetCurrentDirectoryA(g.gameDir.c_str());
    Log("Game dir: %s", g.gameDir.c_str());
    if (!g_bink.Load(g.gameDir)) {
        MessageBoxA(nullptr, "Could not load binkw32.dll from the game folder.", "Driv3r", MB_ICONERROR);
        return 1;
    }
    g.territory = ReadTerritory();

    // WinMain (0x576bf0): class "HamsterMainWnd", style 3, WS_POPUP 640x480.
    WNDCLASSA wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "HamsterMainWnd";
    if (!RegisterClassA(&wc)) return 0;
    int x = (GetSystemMetrics(SM_CXSCREEN) - kWidth) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - kHeight) / 2;
    g.hwnd = CreateWindowExA(0, "HamsterMainWnd", "Driv3r", WS_POPUP, x, y, kWidth, kHeight,
                             nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) return 0;

    // Back buffer: a 32-bit DIB shared by GDI (videos) and the MEC canvas.
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kWidth;
    bi.bmiHeader.biHeight = -kHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = GetDC(g.hwnd);
    g.backDC = CreateCompatibleDC(dc);
    g.backBmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(g.hwnd, dc);
    if (!g.backBmp) return 0;
    SelectObject(g.backDC, g.backBmp);
    g.canvas.px = static_cast<uint32_t*>(bits);
    g.canvas.w = kWidth;
    g.canvas.h = kHeight;

    g_joy.Init(g.hwnd);  // joystick init 0x583a40
    g.onAudioLanguageChanged.push_back([] { g_sounds.Load(); });  // AUDIO: sounds follow the audio language
    g.SetLanguage(0);
    g.ResetKeyMap();
    g.modes = EnumDisplayModes(g.antialiasOk);
    g.soundDrivers = EnumSoundDrivers();
    g.res = DefaultMode(g.modes);
    // No profile yet: it is chosen (or created) after the title screen by
    // the boot profile flow (MemoryUnit::BOOT), as in the original. Until
    // then the live / saved block holds the constructor's defaults (0x547e90).
    g_profile.SetDefaults();
    g_profile.Write(0xc34, &g.res, sizeof(g.res));  // default mode (0x547fb0 -> 0x54a7a0)
    ApplyProfile();

    BootSequencer seq;
    LanguageScreen language;
    TitleScreen title;
    FrontEnd frontEnd;
    g.screens[ST_BOOT_SEQ] = &seq;
    g.screens[ST_INTRO_FMV] = &g_introFmv;
    g.screens[ST_LANGUAGE] = &language;
    g.screens[ST_TITLE] = &title;
    g.screens[ST_FRONTEND] = &frontEnd;
    g.screens[ST_GAMEPLAY] = &g_gameplay;

    ShowWindow(g.hwnd, showCmd);
    UpdateWindow(g.hwnd);
    g.Goto(ST_BOOT_SEQ);

    // Main loop (FUN_00571d80 / FUN_00571f00): pump messages, tick, draw.
    DWORD lastDraw = 0;
    while (g.running) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g.running = false;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g.running) break;
        DispatchJoystick();
        UpdateGuiRepeat();
        if (!g.running) break;
        if (g.cur) g.cur->Update();
        DWORD now = GetTickCount();
        if (now - lastDraw >= 15) { Present(); lastDraw = now; }
        Sleep(1);
    }

    if (g.cur) g.cur->Leave();
    if (g.hwnd) DestroyWindow(g.hwnd);
    RestoreDisplay();
    UnregisterClassA("HamsterMainWnd", inst);
    DeleteDC(g.backDC);
    DeleteObject(g.backBmp);
    Log("Exit");
    if (g_log) fclose(g_log);
    return 0;
}
