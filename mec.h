// Reflections .MEC menu files (Driv3r PC) and the FONT.BNK bitmap font.
//
// File layout (recovered from Driv3r.exe, see driv3r-re/BOOT_TO_MENU.md):
//   CHNK { RMDP: CHNK { RMDL: menu layout, MDPC: materials + DDS textures } }
//
// RMDL  (FUN_00467ca0 / FUN_00469bd0 / FUN_00466e90)
//   u32 version (>= 0x134), u32 screenCount, u32 sizes[7]
//   per screen: header[sizes[1]] {char name[8]; i32; u32 focus; u8 overlay;
//                                 char onEnter[32]; char onExit[32]; ...}
//               u32 groups; per group: u32 n; n x { u8 type; element }
//   element = common[sizes[2]] {float x,y,w,h,r,g,b,a} + per type:
//     1 sprite  extra[sizes[4]] {float u0,v0,u1,v1; u32 material}
//     2 text    extra[sizes[3]] {u32 textId; u32; u32 align; float sx,sy;
//                                char colourFn[32]; char textFn[32]}
//     3 button  u32 rows; per row: u32 n; n x {u8 type(1|2); element}
//               tail[sizes[5]] {u8 enabled; {u8 kind; i32 target}[8];
//                               char actions[8][32]}
//     4 fmv     (common only)
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Premultiplied BGRA image.
struct Texture
{
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    bool additive = false;
};

bool DecodeDds(const uint8_t* dds, size_t size, Texture& out);

struct Canvas
{
    uint32_t* px = nullptr;
    int w = 0, h = 0;

    // Draws texture region (u0,v0)-(u1,v1) into the destination rectangle,
    // modulated by rgba (bilinear filtering).
    void Draw(const Texture& t, float u0, float v0, float u1, float v1, float dx, float dy,
              float dw, float dh, const float rgba[4], bool additive = false);
    void Fill(float dx, float dy, float dw, float dh, uint32_t bgra);
};

class Font
{
public:
    bool Load(const std::string& path);
    float Width(const std::wstring& s, float sx) const;
    void Draw(Canvas& c, const std::wstring& s, float x, float y, float sx, float sy,
              const float rgba[4]) const;
    bool Loaded() const { return !pages_.empty(); }

private:
    struct Glyph { int x, y, w, h, page, yoff; };
    int Kern(wchar_t a, wchar_t b) const;

    std::vector<Texture> pages_;
    std::map<wchar_t, Glyph> glyphs_;
    std::vector<uint8_t> kern_;  // 52x52 for A-Z, a-z
    int spaceWidth_ = 8;
};

enum ElemType { EL_SPRITE = 1, EL_TEXT = 2, EL_BUTTON = 3, EL_FMV = 4 };

struct MecLink { int kind = -1; int target = -1; };  // kind 0 = item, 1 = screen

// Button action slots (tail strings).
enum { ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_SELECT, ACT_STATUS };

struct Element
{
    int type = 0;
    float x = 0, y = 0, w = 0, h = 0;
    float rgba[4] = { 1, 1, 1, 1 };
    // sprite
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    int material = 0;
    // text
    int textId = 0, align = 0;
    float sx = 1, sy = 1;
    std::string textFn;
    // button
    std::vector<std::vector<Element>> rows;
    bool enabled = false;
    MecLink links[8];
    std::string actions[8];
    int state = 1;  // row drawn: 0 focused, 1 normal, 2 disabled, 3 flagged
};

struct MecScreen
{
    std::string name, onEnter, onExit;
    int focus = 0;
    bool overlay = false;
    std::vector<Element> items;
};

struct MecFile
{
    std::vector<MecScreen> screens;
    std::vector<Texture> materials;
    bool Load(const std::string& path);
    int Find(const std::string& name) const;
};
