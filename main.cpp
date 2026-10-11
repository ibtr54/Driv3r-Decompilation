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
#include <mmsystem.h>
#include <d3d9.h>
#include <dsound.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "mec.h"

#pragma comment(lib, "winmm.lib")  // PlaySound (menu sounds)

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
                  Get(SetSoundSystem, "_BinkSetSoundSystem@8") &
                  Get(OpenDirectSound, "_BinkOpenDirectSound@4");
        if (ok) SetSoundSystem(reinterpret_cast<void*>(OpenDirectSound), 0);
        return ok;
    }
};
static BinkApi g_bink;

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
        HBINK probe = g_bink.Open(path.c_str(), 0);
        if (!probe) { Log("Movie: cannot open %s", path.c_str()); return false; }
        uint32_t tracks = *reinterpret_cast<uint32_t*>(static_cast<char*>(probe) + BINK_NUMTRACKS);
        uint32_t track = tracks > langTrack ? langTrack : 0;
        g_bink.Close(probe);
        g_bink.SetSoundTrack(1, &track);
        h_ = g_bink.Open(path.c_str(), BINKSNDTRACK);
        if (!h_) h_ = g_bink.Open(path.c_str(), 0);
        if (!h_) return false;

        const BinkHeader* hdr = static_cast<const BinkHeader*>(h_);
        w_ = hdr->Width;
        hgt_ = hdr->Height;
        px_.assign(size_t(w_) * hgt_, 0);
        loop_ = loop;
        done_ = false;
        first_ = true;
        path_ = path;
        Log("Movie: %s %ux%u, %u frames, audio track %u/%u%s", path.c_str(), w_, hgt_,
            hdr->Frames, track, tracks, loop ? " (loop)" : "");
        return true;
    }

    void Close()
    {
        if (h_) g_bink.Close(h_);
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
};

static const int kWidth = 640, kHeight = 480;  // CreateWindowExA in WinMain (0x576bf0)

struct App
{
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool running = true;
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

// ========================================================== menu sounds ====
// Menu sound object (global 0x8ac3b8, vtbl 0x781f60, created by 0x46a6c0).
// Load (0x46a4f0 / 0x46a580): SOUNDS\MENU.DAT = "01SM", u8 bank, u8 sample[3].
// Play (0x46a650): id 0/1/2 -> sample[id] of that bank in AUDIO:SOUNDS\SOUND.GSD,
// volume 1.0, pitch 1.0. The GUI plays id 0 when the focus moves
// (FUN_0046a170), 1 on select (FUN_00469d20 input 6) and 2 on back (input 7);
// the front-end handlers play id 0 when a value changes.
class MenuSounds
{
public:
    enum { MOVE = 0, SELECT = 1, BACK = 2 };

    void Load()
    {
        PlaySoundA(nullptr, nullptr, 0);  // stop before freeing the buffers
        for (auto& w : wav_) w.clear();
        for (auto& p : pcm_) p.clear();

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
            wav_[id] = MakeWav(smp.pcm, smp.rate);
            pcm_[id] = std::move(smp.pcm);
            Log("Sound: menu id %d = bank %d sample %d (%u Hz, %u ms)", id, bank, s, smp.rate,
                unsigned(pcm_[id].size() / 2 * 1000 / (smp.rate ? smp.rate : 1)));
        }
    }

    // PlaySound has no volume control, so the effects volume is applied to
    // the samples (the original sets sound-manager categories 0/1 and 3).
    void Play(int id)
    {
        if (id < 0 || id >= 3 || wav_[id].empty()) return;
        PlaySoundA(nullptr, nullptr, 0);  // the buffer is rewritten below
        float gain = g.EffectsGain();
        const int16_t* src = reinterpret_cast<const int16_t*>(pcm_[id].data());
        int16_t* dst = reinterpret_cast<int16_t*>(wav_[id].data() + 44);
        for (size_t i = 0; i < pcm_[id].size() / 2; ++i) dst[i] = int16_t(src[i] * gain);
        if (!PlaySoundA(reinterpret_cast<LPCSTR>(wav_[id].data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
            Log("Sound: PlaySound failed for menu id %d", id);
    }

private:
    static std::vector<uint8_t> MakeWav(const std::vector<uint8_t>& pcm, uint32_t rate)
    {
        std::vector<uint8_t> w(44 + pcm.size());
        auto put32 = [&](size_t p, uint32_t v) { memcpy(&w[p], &v, 4); };
        auto put16 = [&](size_t p, uint16_t v) { memcpy(&w[p], &v, 2); };
        memcpy(&w[0], "RIFF", 4);
        put32(4, uint32_t(36 + pcm.size()));
        memcpy(&w[8], "WAVEfmt ", 8);
        put32(16, 16);
        put16(20, 1);         // PCM
        put16(22, 1);         // mono
        put32(24, rate);
        put32(28, rate * 2);  // byte rate
        put16(32, 2);         // block align
        put16(34, 16);        // bits
        memcpy(&w[36], "data", 4);
        put32(40, uint32_t(pcm.size()));
        memcpy(&w[44], pcm.data(), pcm.size());
        return w;
    }

    std::vector<uint8_t> wav_[3];  // WAV images played by PlaySound
    std::vector<uint8_t> pcm_[3];  // original samples, at full volume
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
// PlaySound plays one sound at a time (the menu sounds use it), so the
// music goes through waveOut instead.
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
        for (Layer& l : layers_) {
            if (l.out || l.smp.pcm.empty()) continue;
            WAVEFORMATEX wf = {};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = WORD(l.smp.channels);
            wf.nSamplesPerSec = l.smp.rate;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = WORD(wf.nChannels * 2);
            wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
            if (waveOutOpen(&l.out, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
                Log("Music: waveOutOpen failed");
                l.out = nullptr;
                continue;
            }
            l.hdr = WAVEHDR();
            l.hdr.lpData = reinterpret_cast<LPSTR>(l.smp.pcm.data());
            l.hdr.dwBufferLength = DWORD(l.smp.pcm.size() / wf.nBlockAlign * wf.nBlockAlign);
            waveOutPrepareHeader(l.out, &l.hdr, sizeof(l.hdr));
            if (l.smp.loop) {
                l.hdr.dwFlags |= WHDR_BEGINLOOP | WHDR_ENDLOOP;
                l.hdr.dwLoops = 0xffffffffu;
            }
            waveOutWrite(l.out, &l.hdr, sizeof(l.hdr));
        }
        UpdateVolume();
    }

    // Music volume = sound-manager category 2 (and 0x8ac48c+0x40), scaled by
    // the overall volume (category 3).
    void UpdateVolume()
    {
        DWORD v = DWORD(std::min(1.0f, std::max(0.0f, g.MusicGain())) * 0xffff);
        for (Layer& l : layers_)
            if (l.out) waveOutSetVolume(l.out, v | (v << 16));
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
        HWAVEOUT out = nullptr;
        WAVEHDR hdr = {};
    };
    Layer layers_[2];
};
static FrontEndMusic g_music;

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

    // The name screen (GUI\NAME) that creates profiles is not implemented, so
    // use the most recently saved existing profile. Without one the block is
    // kept in memory only (accept / restore still work, nothing is written).
    bool Load(const std::string& gameDir)
    {
        std::string saves = gameDir + "\\Saves\\";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((saves + "D3P_*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        FILETIME newest = {};
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::string p = saves + fd.cFileName + "\\ProfileData";
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a) || a.nFileSizeLow != kSize) continue;
            if (path_.empty() || CompareFileTime(&a.ftLastWriteTime, &newest) > 0) {
                path_ = p;
                newest = a.ftLastWriteTime;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (path_.empty()) return false;

        FILE* f = fopen(path_.c_str(), "rb");
        if (!f) { path_.clear(); return false; }
        std::vector<uint8_t> d(kSize);
        bool ok = fread(d.data(), 1, kSize, f) == size_t(kSize);
        fclose(f);
        if (ok) data_.swap(d);
        else path_.clear();
        return ok;
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
}

// Live settings -> saved block (0x548300), then the profile file (autosave)
// when there is one.
static void SaveProfile(bool toFile = true)
{
    g_profile.SetInt(0x34, g.vol.overall);
    g_profile.SetInt(0x38, g.vol.effects);
    g_profile.SetInt(0x3c, g.vol.music);
    for (const SettingFlag& f : kSettingFlags) g_profile.SetFlag(f.off, g.Toggle(f.key));
    g_profile.SetInt(0x17c, g.turn);
    g_profile.Write(0xc, &g.motionBlur, 4);
    g_profile.Write(0x208, g.keymap, sizeof(g.keymap));
    // A resolution missing from the mode list becomes the default (0x548385).
    if (!g.modes.empty() && FindMode(g.modes, g.res) < 0) g.res = DefaultMode(g.modes);
    g_profile.SetInt(0x160, g.aspect);
    g_profile.Write(0xc30, &g.gamma, 4);
    g_profile.Write(0xc34, &g.res, sizeof(g.res));
    g_profile.SetInt(0xc88, g.drawdist);
    g_profile.SetInt(0xc90, g.filter);
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
    // 0x548300 sets the audio (vtbl+0x64) and text (vtbl+0x54) language in use.
    g.SetAudioLanguage(g.audioSetting);
    g.SetTextLanguage(g.textSetting);
    if (!toFile) return;
    if (!g_profile.HasFile()) { Log("Settings: no profile in Saves\\, not written"); return; }
    bool ok = g_profile.Save();
    Log("Settings: %s %s", ok ? "saved to" : "FAILED to save", g_profile.Path().c_str());
}

// ============================================================ MEC menus ====
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

    void Reset(int screen)
    {
        stack.clear();
        Push(screen);
    }

    void Push(int screen)
    {
        if (screen < 0 || screen >= int(mec.screens.size())) return;
        stack.push_back(screen);
        MecScreen& s = mec.screens[screen];
        Log("Menu: enter screen %d '%s'", screen, s.name.c_str());
        for (Element& e : s.items) e.state = 1;
        SetFocus(s, s.focus);
        Fire(s.onEnter);
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
            g_sounds.Play(MenuSounds::BACK);
            Pop();
            return;
        }
        Element& e = s->items[s->focus];
        if (e.type != EL_BUTTON) return;
        int slot = vk == VK_UP ? ACT_UP : vk == VK_DOWN ? ACT_DOWN : vk == VK_LEFT ? ACT_LEFT
                 : vk == VK_RIGHT ? ACT_RIGHT : vk == VK_RETURN ? ACT_SELECT : -1;
        if (slot < 0) return;
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

    void Render(HDC dc, Canvas& cv)
    {
        if (stack.empty()) return;
        size_t first = stack.size() - 1;
        while (first > 0 && mec.screens[stack[first]].overlay) --first;  // popups draw over their parent
        for (size_t i = first; i < stack.size(); ++i)
            for (Element& e : mec.screens[stack[i]].items) DrawElement(dc, cv, e);
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

    void DrawElement(HDC dc, Canvas& cv, Element& e)
    {
        float x = e.x * kWidth, y = e.y * kHeight, w = e.w * kWidth, h = e.h * kHeight;
        switch (e.type) {
        case EL_SPRITE:
            if (e.material >= 0 && e.material < int(mec.materials.size())) {
                const Texture& t = mec.materials[e.material];
                cv.Draw(t, e.u0, e.v0, e.u1, e.v1, x, y, w, h, e.rgba, t.additive);
            }
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
            float tw = g.font.Width(s, e.sx);
            // 0 left, 1 right, 2 centre: the game's text API (the same values
            // as the FMV subtitles, 0x5992ac); front.MEC uses only these three.
            float tx = e.align == 1 ? x + w - tw : e.align == 2 ? x + (w - tw) / 2 : x;
            // Compared with the original (aspect 0, 8 texts with scales from
            // 0.4 to 1.25): widths follow sx, heights follow sy x ~0.93
            // (taken as 480/512; the code behind it was not traced), and y
            // is the top of the capitals, as for the subtitles.
            float hs = e.sy * (480.0f / 512.0f);
            g.font.Draw(cv, s, tx, y - g.font.TopOffset() * hs, e.sx, hs, e.rgba);
            break;
        }
        case EL_BUTTON:
            if (!e.actions[ACT_STATUS].empty() && onStatus) e.state = onStatus(e.actions[ACT_STATUS], e.state);
            if (e.state >= 0 && e.state < int(e.rows.size()))
                for (Element& c : e.rows[e.state]) DrawElement(dc, cv, c);
            break;
        case EL_FMV:
            if (drawFmv) drawFmv(dc, int(x), int(y), int(w), int(h));
            break;
        }
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
            y -= g.font.TopOffset() * r.sy;
            float x0 = 0.1f * kWidth, w = 0.8f * kWidth;
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
    // Greedy word wrap into the box width; explicit line breaks are kept.
    // The original's code was not traced, but its making-of line 0 broke at
    // the same words (compared on screen).
    static std::vector<std::wstring> Wrap(const std::wstring& s, float width, float sx)
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
    void OnKey(WPARAM vk) override
    {
        if (vk != VK_ESCAPE) menu_.OnKey(vk);
    }

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
        menu_.onAction = [](const std::string& a) {
            if (a == "bu_continue") {  // handler 0x541b60
                Log("event: bu_continue");
                g.Goto(ST_BOOT_SEQ);
            }
        };
        menu_.Reset(0);
    }
    void Render(HDC dc, Canvas& cv) override { menu_.Render(dc, cv); }
    void OnKey(WPARAM vk) override { menu_.OnKey(vk); }

private:
    MecMenu menu_;
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
            // Controller name (0x554f26 -> 0x5522d0): no joystick support, so none.
            if (fn == "txt_controller") { out.clear(); return true; }
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
        bg_.Draw(dc, 0, 0, kWidth, kHeight, false);
        menu_.Render(dc, cv);
    }

    void OnKey(WPARAM vk) override
    {
        idleSince_ = GetTickCount();  // any input clears the counter (0x551e00 -> +0x18 = 0)
        if (extra_.IsOpen()) { if (IsSkipKey(vk)) EndExtra(); return; }
        if (km_.state == 1) return CaptureKey(vk);
        menu_.OnKey(vk);
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
        static const char* kGameplay[] = { "StartGame", "NewGame", "ContinueGame", "fe_dgstart",
                                           "LoadSave", "LoadReplay", "ReplayMission2" };
        for (const char* gp : kGameplay)
            if (_stricmp(a.c_str(), gp) == 0) return LaunchGameplay(a);
        if (StartsWithI(a, "RunMission=")) return LaunchGameplay(a);
        if (a == "fe_quitgame") { g.Quit(); return; }  // 0x8ac258 vtbl+0x10

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
                g.toggles["sub"] = false;  // on only in territories 2 and 3 (renderer vtbl+0x48); Europe is 1
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
        // Controller Setup (screen 35, 0x554e20..). No joystick support: the
        // device list is empty, so the controller events have nothing to do.
        if (a == "joystick_backup" || a == "Prev_Controller" || a == "Next_Controller") return;
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
        // Controller Setup with no joystick (count at 0x8ac2b8+0x6d0 is 0):
        // "No Device Detected" stays, the device arrows are hidden (0x554eec).
        if (s == "stat_no_ctrl_detected") return st;
        if (s == "stat_controllercount_arrow") return 2;
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
            // focused, so put them back to normal: the popup is drawn over
            // screen 31, whose cell p05 asks for stat_keymap_p01 and would
            // otherwise stay lit after blinking.
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
        for (int i = 0; i < kLanguageCount; ++i) {
            std::string l = std::string("stat_") + kLanguages[i].stat;
            // Stat_<Language>Text / Audio (0x554374.., 0x55416e..): +0x2c / +0x28.
            if (s == l + "text") return g.textSetting == i ? 0 : 1;
            if (s == l + "audio") return g.audioSetting == i ? 0 : 1;
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
    // Renderer vtbl+0x48. The order is taken from the language pages
    // (0 US, 1 EU, 2 JP, other AS, 0x556660); only Europe ships here.
    static int TerritoryId()
    {
        if (_stricmp(g.territory.c_str(), "Usa") == 0 || _stricmp(g.territory.c_str(), "America") == 0) return 0;
        if (_stricmp(g.territory.c_str(), "Europe") == 0) return 1;
        if (_stricmp(g.territory.c_str(), "Japan") == 0) return 2;
        return 3;
    }

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
    // the editor then goes back one screen and sends keymap_changed. Joystick
    // and mouse input (the alternative column) are not read here.
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
        for (int row = 0; row < km_.count; ++row) {
            int half = 0;
            KeyRecord* r = KeyRow(g.keymap, km_.group, row, half);
            if (!r) continue;
            KeyBind& b = r->bind[half];
            if (b.key == code) b.key = 0x4000;
            if (!b.altAxis && b.alt == code) b.alt = 0x4000;
        }
        int half = 0;
        if (KeyRecord* r = KeyRow(g.keymap, km_.group, km_.sel, half)) r->bind[half].key = code;
        Log("Key map: group %d row %d = 0x%02X", km_.group, km_.sel, code);
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
    TextTable controlsText_;
};

// ============================================================ window ========
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
        if (g.cur) g.cur->OnKey(wp);
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_F4) { g.Quit(); return 0; }
        break;
    case WM_CLOSE:
        g.Quit();
        return 0;
    case WM_DESTROY:
        g.running = false;
        g.hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void Present()
{
    if (!g.hwnd) return;
    std::fill(g.canvas.px, g.canvas.px + size_t(kWidth) * kHeight, 0xff000000u);
    if (g.cur) g.cur->Render(g.backDC, g.canvas);
    GdiFlush();
    HDC dc = GetDC(g.hwnd);
    BitBlt(dc, 0, 0, kWidth, kHeight, g.backDC, 0, 0, SRCCOPY);
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

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int showCmd)
{
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

    g.onAudioLanguageChanged.push_back([] { g_sounds.Load(); });  // AUDIO: sounds follow the audio language
    g.SetLanguage(0);
    g.ResetKeyMap();
    g.modes = EnumDisplayModes(g.antialiasOk);
    g.soundDrivers = EnumSoundDrivers();
    g.res = DefaultMode(g.modes);
    if (g_profile.Load(g.gameDir)) {
        Log("Settings: loaded %s", g_profile.Path().c_str());
        ApplyProfile();
    } else {
        Log("Settings: no profile in Saves\\, using defaults");
        SaveProfile(false);  // defaults become the saved block for settings_restore
    }

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
        if (g.cur) g.cur->Update();
        DWORD now = GetTickCount();
        if (now - lastDraw >= 15) { Present(); lastDraw = now; }
        Sleep(1);
    }

    if (g.cur) g.cur->Leave();
    if (g.hwnd) DestroyWindow(g.hwnd);
    UnregisterClassA("HamsterMainWnd", inst);
    DeleteDC(g.backDC);
    DeleteObject(g.backBmp);
    Log("Exit");
    if (g_log) fclose(g_log);
    return 0;
}
