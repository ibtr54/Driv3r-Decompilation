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
    std::vector<uint32_t> px;  // ARGB, straight (not premultiplied) colour
    bool additive = false;
    // Smaller levels (1, 2, ...): the original loads its textures with D3DX,
    // which builds a full box-filtered chain even when the DDS has none.
    struct Level { int w, h; std::vector<uint32_t> px; };
    std::vector<Level> mips;
    void BuildMips();
};

bool DecodeDds(const uint8_t* dds, size_t size, Texture& out);

// The whole file (empty if it cannot be read).
std::vector<uint8_t> ReadFile(const std::string& path);
// CHNK container (all of Driv3r's data files): the first chunk with the tag,
// searched depth-first from base; off / size of its data.
bool FindChunk(const std::vector<uint8_t>& d, size_t base, const char* tag, size_t& off, size_t& size);
// The textures of an MDPC chunk, one per material (first texture each).
std::vector<Texture> LoadMaterials(const std::vector<uint8_t>& d, size_t mdpc, size_t mdpcSize);

struct Canvas
{
    uint32_t* px = nullptr;
    int w = 0, h = 0;
    // Texture filter setting (+0xc90), applied by 0x5c9710 to stages 0-3:
    // 0 point (min, mag and mip), 1 bilinear with the nearest mip level,
    // 2 bilinear with linear mip blending.
    int filter = 1;

    // Draws texture region (u0,v0)-(u1,v1) into the destination rectangle,
    // modulated by rgba, as Direct3D draws the original's sprites and
    // glyphs. Colour and alpha are filtered separately (transparent texels
    // still lend their colour to the edge of the opaque ones), the mip
    // level follows the larger texel-per-pixel ratio, and pixels are
    // sampled at +0.15 / +0.25 (x / y): measured on the game's screenshots,
    // not derived.
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
    // FBNK +6: line pitch, +8: offset from the top of a cell to the top of
    // the capitals (both matched against the original's subtitles).
    int LinePitch() const { return linePitch_; }
    int TopOffset() const { return topOffset_; }

private:
    // bear: left bearing (record +6 bits 0..4): the glyph is drawn that far
    // right and the pen advances width + bearing (0x575038, 0x5754b1).
    struct Glyph { int x, y, w, h, page, yoff, bear; };
    int Kern(wchar_t a, wchar_t b) const;

    std::vector<Texture> pages_;
    std::map<wchar_t, Glyph> glyphs_;
    std::vector<uint8_t> kern_;  // 52x52 for A-Z, a-z
    int spaceWidth_ = 8;
    int linePitch_ = 32, topOffset_ = 8;
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
    std::string colourFn;  // also used for "val_*" number queries (Sound screen)
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
    int defaultFocus = 0;  // header focus item; screens shown by name start there (0x467730)
    // Header +8: the screen's background, a full-screen quad drawn by the
    // screen draw 0x469a40: -2 the current movie (0x8ac3bc vtbl+0x14),
    // >= 0 a material of the same MEC file (front.MEC: 1 on TAR_Main,
    // TAR_Start and DG_Main; MUMAIN, NAME, NOCONT: 0), -1 none.
    int background = -1;
    // Header byte +16. Go-to-screen 0x467b70 does not put a screen with it
    // into the back history; it is not drawn over its parent (only the
    // current screen is drawn).
    bool overlay = false;
    // Header byte +81: entering the screen clears the back history
    // (0x467c65 -> 0x468590). Set only on Main in FRONT.MEC.
    bool clearsHistory = false;
    std::vector<Element> items;
    // Runtime (screen object +0x134): set by a mouse move, cleared by
    // keyboard navigation; while set the focus follows the pointer.
    bool hover = false;
};

struct MecFile
{
    std::vector<MecScreen> screens;
    std::vector<Texture> materials;
    bool Load(const std::string& path);
    int Find(const std::string& name) const;
};
