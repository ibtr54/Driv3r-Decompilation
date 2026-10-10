// Driv3r (PC, 2005) boot sequence + front-end menu, re-implemented from
// Driv3r.exe (sha256 355ad3d5...0f02) analysed with REA/Ghidra.
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

// ====================================================== screens / states ===
// State IDs are the indices in the factory registry FUN_005867f0
// (creator slot = id * 0x14 + 0x10), registered by the static initialisers
// at 0x6df220.. / 0x6dc5b5.
enum StateId
{
    ST_FRONTEND  = 0,     // id not resolved (reached via manager vtbl+0x24 after "bootup_over")
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
    int lang = 0;  // index into kLanguages
    Font font;
    HDC backDC = nullptr;
    HBITMAP backBmp = nullptr;
    Canvas canvas;
    std::map<int, Screen*> screens;
    Screen* cur = nullptr;
    std::vector<std::function<void()>> onLanguageChanged;

    // "TERR:" / "LANG:" prefixes as used by the game's file layer (0x8ac230 vtbl+0x10).
    std::string Resolve(const std::string& p) const
    {
        if (StartsWithI(p, "TERR:")) return "Territory\\" + territory + "\\" + p.substr(5);
        if (StartsWithI(p, "LANG:"))
            return "Territory\\" + territory + "\\Locale\\" + kLanguages[lang].folder + "\\" + p.substr(5);
        if (StartsWithI(p, "AUDIO:"))  // audio language (same as the text language here)
            return "Territory\\" + territory + "\\Locale\\" + kLanguages[lang].folder + "\\" + p.substr(6);
        return p;
    }

    uint32_t AudioTrack() const
    {
        // Driv3r maps language -> Bink sound track 0..4 (0x599520), others use 0.
        uint32_t id = kLanguages[lang].id;
        return id <= 4 ? id : 0;
    }

    void SetLanguage(int i)
    {
        lang = (i % kLanguageCount + kLanguageCount) % kLanguageCount;
        font.Load(Resolve("LANG:FONTS\\FONT.BNK"));  // FUN_00467ca0 loads it per language
        for (auto& f : onLanguageChanged) f();
        Log("Language: %s", kLanguages[lang].folder);
    }

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

static bool IsSkipKey(WPARAM vk) { return vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_SPACE; }

// ========================================================== menu sounds ====
// Menu sound object (global 0x8ac3b8, vtbl 0x781f60, created by 0x46a6c0).
// Load (0x46a4f0 / 0x46a580): SOUNDS\MENU.DAT = "01SM", u8 bank, u8 sample[3].
// Play (0x46a650): id 0/1/2 -> sample[id] of that bank in AUDIO:SOUNDS\SOUND.GSD,
// volume 1.0, pitch 1.0. The GUI plays id 0 when the focus moves
// (FUN_0046a170), 1 on select (FUN_00469d20 input 6) and 2 on back (input 7);
// the front-end handlers play id 0 when a value changes.
//
// SOUND.GSD: "01KB", u32 bankCount, u32 bankOffset[bankCount]. Bank:
// u32 count, u32 dataOffset, u32 dataSize, then count x 20-byte records
// {u32 offset; u32 size; u16 rate; u8; u8; i32 loop; u8; u8; u16} and raw
// 16-bit mono PCM at bank + dataOffset + offset.
class MenuSounds
{
public:
    enum { MOVE = 0, SELECT = 1, BACK = 2 };

    void Load()
    {
        PlaySoundA(nullptr, nullptr, 0);  // stop before freeing the buffers
        for (auto& w : wav_) w.clear();

        uint8_t dat[8] = {};
        FILE* f = fopen("SOUNDS\\MENU.DAT", "rb");
        size_t n = f ? fread(dat, 1, sizeof(dat), f) : 0;
        if (f) fclose(f);
        if (n < 8 || memcmp(dat, "01SM", 4) != 0) { Log("Sound: SOUNDS\\MENU.DAT missing or invalid"); return; }
        int bank = int8_t(dat[4]);

        std::string gsd = g.Resolve("AUDIO:SOUNDS\\SOUND.GSD");
        f = fopen(gsd.c_str(), "rb");
        if (!f) { Log("Sound: cannot open %s", gsd.c_str()); return; }
        char magic[4] = {};
        uint32_t banks = 0, bankOff = 0, count = 0, dataOff = 0, dataSize = 0;
        bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "01KB", 4) == 0 &&
                  fread(&banks, 4, 1, f) == 1 && bank >= 0 && uint32_t(bank) < banks &&
                  fseek(f, 8 + bank * 4, SEEK_SET) == 0 && fread(&bankOff, 4, 1, f) == 1 &&
                  fseek(f, long(bankOff), SEEK_SET) == 0 && fread(&count, 4, 1, f) == 1 &&
                  fread(&dataOff, 4, 1, f) == 1 && fread(&dataSize, 4, 1, f) == 1;
        for (int id = 0; ok && id < 3; ++id) {
            int s = int8_t(dat[5 + id]);
            if (s < 0 || uint32_t(s) >= count) continue;
            uint8_t rec[20];
            if (fseek(f, long(bankOff + 12 + s * 20), SEEK_SET) || fread(rec, 1, 20, f) != 20) continue;
            uint32_t off, size;
            uint16_t rate;
            memcpy(&off, rec, 4);
            memcpy(&size, rec + 4, 4);
            memcpy(&rate, rec + 8, 2);
            if (size == 0 || off + size > dataSize) continue;
            std::vector<uint8_t> pcm(size);
            if (fseek(f, long(bankOff + dataOff + off), SEEK_SET) || fread(pcm.data(), 1, size, f) != size) continue;
            wav_[id] = MakeWav(pcm, rate);
            Log("Sound: menu id %d = bank %d sample %d (%u Hz, %u ms)", id, bank, s, rate,
                unsigned(size / 2 * 1000 / (rate ? rate : 1)));
        }
        fclose(f);
    }

    void Play(int id)
    {
        if (id >= 0 && id < 3 && !wav_[id].empty() &&
            !PlaySoundA(reinterpret_cast<LPCSTR>(wav_[id].data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
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

    std::vector<uint8_t> wav_[3];
};
static MenuSounds g_sounds;

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
            if (s.empty() || !g.font.Loaded()) break;
            s = ExpandIcons(s);
            float tw = g.font.Width(s, e.sx);
            float tx = e.align == 2 ? x + (w - tw) / 2 : e.align == 3 ? x + w - tw : x;
            g.font.Draw(cv, s, tx, y, e.sx, e.sy, e.rgba);
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
class IntroFmvScreen : public Screen
{
public:
    void SetIndex(int i) { index_ = i; }                    // vtbl+0x20 (0x5507f0)
    void SetFile(const std::string& f) { custom_ = f; }     // vtbl+0x24 (0x556380)

    void Enter() override                                    // 0x5563a0
    {
        static const char* kFiles[] = { "TERR:FMV\\LEGAL.XMV", "FMV\\ATARI.XMV", "FMV\\REFLECT.XMV" };
        std::string file = index_ <= 2 ? kFiles[index_] : custom_;
        if (!movie_.Open(g.Resolve(file), g.AudioTrack(), false))
            g.Goto(ST_BOOT_SEQ);  // missing file: carry on with the sequence
    }
    void Leave() override { movie_.Close(); }
    void Update() override
    {
        movie_.Update();
        if (movie_.Done()) g.Goto(ST_BOOT_SEQ);  // listener -> back to the sequencer
    }
    void Render(HDC dc, Canvas&) override { movie_.Draw(dc, 0, 0, kWidth, kHeight, true); }
    void OnKey(WPARAM vk) override
    {
        if (IsSkipKey(vk)) g.Goto(ST_BOOT_SEQ);
    }

private:
    int index_ = 0;
    std::string custom_;
    Movie movie_;
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
                    if (i != g.lang) g.SetLanguage(i);
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
class FrontEnd : public Screen
{
public:
    FrontEnd()
    {
        g.onLanguageChanged.push_back([this] { if (menu_.mec.screens.size()) menu_.ReloadText(); });
    }

    void Enter() override
    {
        menu_.Load("GUI\\FRONT");
        menu_.onAction = [this](const std::string& a) { Action(a); };
        menu_.onStatus = [this](const std::string& s, int st) { return Status(s, st); };
        menu_.drawFmv = [this](HDC dc, int x, int y, int w, int h) { pane_.Draw(dc, x, y, w, h, false); };
        menu_.dynamicText = [](const std::string& fn, std::wstring& out) {
            // Vehicle names come from the game's vehicle database: not loaded here,
            // so hide the "DevOnly: Vehicle txt" placeholder.
            if (fn == "reqvehicletext") { out.clear(); return true; }
            return false;
        };
        menu_.Reset(0);  // "Main"
    }
    void Leave() override
    {
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
    }

    void Render(HDC dc, Canvas& cv) override
    {
        if (extra_.IsOpen()) { extra_.Draw(dc, 0, 0, kWidth, kHeight, true); return; }
        bg_.Draw(dc, 0, 0, kWidth, kHeight, false);
        menu_.Render(dc, cv);
    }

    void OnKey(WPARAM vk) override
    {
        if (extra_.IsOpen()) { if (IsSkipKey(vk)) EndExtra(); return; }
        menu_.OnKey(vk);
    }

private:
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
        if (a == "fe_playvehicle") return PreviewVehicle();
        if (a == "fe_nextcity" || a == "fe_prevcity") { Changed(); Step(city_, 3, a[3] == 'n'); return PreviewRide(); }
        if (a == "nexttime" || a == "prevtime") { Changed(); Step(time_, 4, a[0] == 'n'); return PreviewRide(); }
        if (a == "nextweather" || a == "prevweather") { Changed(); Step(weather_, 3, a[0] == 'n'); return PreviewRide(); }
        if (a == "fe_nextVehicle" || a == "fe_prevVehicle") { Changed(); Step(vehicle_, 20, a[3] == 'n'); return PreviewVehicle(); }
        if (a == "cops_toggle") { Changed(); cops_ = !cops_; return; }

        // Driving Games
        if (a == "fe_setupdg" || a == "fe_playdg") return PreviewDg();
        if (a == "nextdg" || a == "prevdg") { Changed(); Step(dg_, 6, a[0] == 'n'); return PreviewDg(); }
        if (a == "dg_nextcity" || a == "dg_prevcity") { Changed(); Step(dgCity_, 3, a[3] == 'n'); return PreviewDg(); }
        if (a == "nextsubgame" || a == "prevsubgame") { Changed(); Step(subgame_, 3, a[0] == 'n'); return; }

        // Options: language
        if (a == "nextlangtext" || a == "nextlangaudio") { Changed(); return g.SetLanguage(g.lang + 1); }
        if (a == "prevlangtext" || a == "prevlangaudio") { Changed(); return g.SetLanguage(g.lang - 1); }

        // Extras / cutscenes (FMV only, no gameplay)
        if (a == "extra_credits") return PlayExtra("FMV\\CREDITS.XMV");
        if (a == "extra_makingof") return PlayExtra("FMV\\THEMAKINGOF.XMV");
        if (a == "extra_boiling") return PlayExtra("FMV\\BOILINGPOINT.XMV");
        if (a == "extra_pirates") {
            static const char* byLang[] = { "FMV\\PIRATES.XMV", "FMV\\PIRATES_FRE.XMV", "FMV\\PIRATES_GER.XMV",
                                            "FMV\\PIRATES_ITA.XMV", "FMV\\PIRATES_SPA.XMV" };
            return PlayExtra(byLang[g.lang]);
        }
        if (a == "cut recap") return PlayExtra("FMV\\RECAP.XMV");
        if (StartsWithI(a, "cut ")) return PlayExtra("FMV\\SC" + a.substr(4) + ".XMV");  // "cut 05a" -> SC05a

        // Generic on/off settings: "<x>_toggle" <-> "stat_<x>" / "<x>_onoff".
        if (a.size() > 7 && a.compare(a.size() - 7, 7, "_toggle") == 0) {
            std::string key = a.substr(0, a.size() - 7);
            toggles_[key] = !Toggle(key);
            Changed();
        }
    }

    // Button state for "stat_*" queries: 0 = first row (selected / on),
    // 1 = second row, n = row n. Unknown names keep the current state.
    int Status(const std::string& s, int st)
    {
        bool dg = StartsWithI(Top(), "DG");
        if (s == "stat_city") return dg ? dgCity_ : city_;
        if (s == "stat_weather") return weather_;
        if (s == "stat_dawn") return time_ == 0 ? 0 : 1;
        if (s == "stat_day") return time_ == 1 ? 0 : 1;
        if (s == "stat_dusk") return time_ == 2 ? 0 : 1;
        if (s == "stat_night") return time_ == 3 ? 0 : 1;
        if (s == "stat_cops") return cops_ ? 0 : 1;
        if (s == "stat_subgame") return subgame_;
        static const char* kDg[] = { "stat_dg chase", "stat_dg getaway", "stat_dg trail",
                                     "stat_dg survival", "stat_dg check", "stat_dg gate" };
        for (int i = 0; i < 6; ++i)
            if (s == kDg[i]) return dg_ == i ? 0 : 1;
        for (int i = 0; i < kLanguageCount; ++i) {
            std::string l = std::string("stat_") + kLanguages[i].stat;
            if (s == l + "text" || s == l + "audio") return g.lang == i ? 0 : 1;
        }
        if (StartsWithI(s, "stat_")) {
            std::string key = s.substr(5);
            if (toggles_.count(key)) return Toggle(key) ? 0 : 1;
        }
        if (s.size() > 6 && s.compare(s.size() - 6, 6, "_onoff") == 0) {
            std::string key = s.substr(0, s.size() - 6);
            if (toggles_.count(key)) return Toggle(key) ? 0 : 1;
        }
        return st;
    }

    // ---- helpers ---------------------------------------------------------
    std::string Top()
    {
        MecScreen* s = menu_.Top();
        return s ? s->name : std::string();
    }
    // Value changed: the front-end handlers play menu sound 0 (e.g. 0x552ef3).
    static void Changed() { g_sounds.Play(MenuSounds::MOVE); }
    static void Step(int& v, int n, bool next) { v = (v + (next ? 1 : n - 1)) % n; }
    bool Toggle(const std::string& k) { auto it = toggles_.find(k); return it == toggles_.end() || it->second; }

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
    void PreviewVehicle()
    {
        static const char* city[] = { "MIAMI_", "NICE_", "ISTAN_" };
        char f[64];
        snprintf(f, sizeof(f), "FMV\\%sCAR%02d.XMV", city[city_], vehicle_ + 1);
        PlayPane(f);
    }
    // FUN_00555e20: "FMV\" + MIAMI/NICE/BUL + CHASE/GETAWAY/... + ".XMV"
    void PreviewDg()
    {
        static const char* city[] = { "MIAMI", "NICE", "BUL" };
        static const char* game[] = { "CHASE", "GETAWAY", "TRAILBLZ", "SURVIVAL", "CHECKPNT", "GATERACE" };
        PlayPane(std::string("FMV\\") + city[dgCity_] + game[dg_] + ".XMV");
    }

    void PlayExtra(const std::string& file)
    {
        bg_.Close();
        pane_.Close();
        if (!extra_.Open(g.Resolve(file), g.AudioTrack(), false)) EndExtra();
    }
    void EndExtra()
    {
        extra_.Close();
        // Re-run the screen's enter action to restore its background video.
        if (MecScreen* s = menu_.Top()) Action(s->onEnter.empty() ? "Movie=FMV\\FRONTEND.XMV" : s->onEnter);
        if (!bg_.IsOpen()) PlayBackground("FMV\\FRONTEND.XMV");
    }

    // FUN_00557d70: setup object (0x8ac3ac) vtbl+8, then state 0xB3.
    void LaunchGameplay(const std::string& what)
    {
        g_gameplay.request = what;
        g.Goto(ST_GAMEPLAY);
    }

    MecMenu menu_;
    Movie bg_, pane_, extra_;
    int city_ = 0, time_ = 1, weather_ = 0, vehicle_ = 0;
    bool cops_ = true;
    int dg_ = 0, dgCity_ = 0, subgame_ = 0;
    std::map<std::string, bool> toggles_;
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

    g.gameDir = FindGameDir("D:\\l\\gta-vc\\DRIV3R");
    //g.gameDir = FindGameDir(cmdLine);
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

    g.onLanguageChanged.push_back([] { g_sounds.Load(); });  // AUDIO: follows the language
    g.SetLanguage(0);

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
