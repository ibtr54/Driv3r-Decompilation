#include "mec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> d;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        d.resize(size_t(n));
        if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    }
    fclose(f);
    return d;
}

uint32_t U32(const std::vector<uint8_t>& d, size_t p)
{
    if (p + 4 > d.size()) return 0;
    uint32_t v;
    memcpy(&v, &d[p], 4);
    return v;
}

// Recursive CHNK container: {"CHNK", size, count, version, {tag, offset, flags, size}[count]}
bool FindChunk(const std::vector<uint8_t>& d, size_t base, const char* tag, size_t& off, size_t& size)
{
    if (base + 16 > d.size() || memcmp(&d[base], "CHNK", 4) != 0) return false;
    uint32_t count = U32(d, base + 8);
    for (uint32_t k = 0; k < count; ++k) {
        size_t e = base + 16 + k * 16;
        size_t o = base + U32(d, e + 4);
        if (memcmp(&d[e], tag, 4) == 0) {
            off = o;
            size = U32(d, e + 12);
            return true;
        }
        if (FindChunk(d, o, tag, off, size)) return true;
    }
    return false;
}

std::string FixedString(const uint8_t* p, size_t n)
{
    size_t len = 0;
    while (len < n && p[len]) ++len;
    std::string s(reinterpret_cast<const char*>(p), len);
    return s == "<NoAction>" ? std::string() : s;
}

// Textures of a MDPC chunk. PCMP header: tables {count, offset}[5] (materials,
// substance refs, substances, texture refs, textures), data offset/size.
// Returns one texture per material (its first texture).
std::vector<Texture> LoadMaterials(const std::vector<uint8_t>& d, size_t mdpc, size_t mdpcSize)
{
    std::vector<Texture> out;
    size_t pc = mdpc;
    while (pc + 4 <= mdpc + mdpcSize && memcmp(&d[pc], "PCMP", 4) != 0) pc += 0x10;
    if (pc + 0x38 > d.size()) return out;
    uint32_t matCount = U32(d, pc + 8), matOff = U32(d, pc + 12);
    uint32_t dataOff = U32(d, pc + 0x30);
    for (uint32_t m = 0; m < matCount; ++m) {
        Texture t;
        uint32_t subRefs = U32(d, pc + matOff + m * 0x18);
        uint32_t substance = U32(d, pc + subRefs);
        uint32_t flags = U32(d, pc + substance);
        uint32_t texRefs = U32(d, pc + substance + 0x10);
        uint32_t texCount = U32(d, pc + substance + 0x14);
        if (texCount) {
            uint32_t entry = U32(d, pc + texRefs);
            uint32_t off = U32(d, pc + entry + 8), size = U32(d, pc + entry + 12);
            size_t p = pc + dataOff + off;
            if (p + size <= d.size() && DecodeDds(&d[p], size, t)) t.BuildMips();
        }
        t.additive = flags == 11;
        out.push_back(std::move(t));
    }
    return out;
}

struct Reader
{
    const std::vector<uint8_t>& d;
    size_t p, end;
    bool ok = true;

    const uint8_t* Take(size_t n)
    {
        if (p + n > end) { ok = false; p = end; return nullptr; }
        const uint8_t* r = &d[p];
        p += n;
        return r;
    }
    uint32_t U32()
    {
        const uint8_t* b = Take(4);
        uint32_t v = 0;
        if (b) memcpy(&v, b, 4);
        return v;
    }
    uint8_t U8()
    {
        const uint8_t* b = Take(1);
        return b ? *b : 0;
    }
};

float F32(const uint8_t* p, int i)
{
    float f;
    memcpy(&f, p + i * 4, 4);
    return f;
}

bool ParseElement(Reader& r, const uint32_t* sizes, int type, Element& e)
{
    e.type = type;
    const uint8_t* c = r.Take(sizes[2]);
    if (!c) return false;
    e.x = F32(c, 0); e.y = F32(c, 1); e.w = F32(c, 2); e.h = F32(c, 3);
    for (int i = 0; i < 4; ++i) e.rgba[i] = F32(c, 4 + i);

    switch (type) {
    case EL_SPRITE: {  // 0x466b00
        const uint8_t* x = r.Take(sizes[4]);
        if (!x) return false;
        e.u0 = F32(x, 0); e.v0 = F32(x, 1); e.u1 = F32(x, 2); e.v1 = F32(x, 3);
        memcpy(&e.material, x + 16, 4);
        return true;
    }
    case EL_TEXT: {  // 0x466be0
        const uint8_t* x = r.Take(sizes[3]);
        if (!x) return false;
        memcpy(&e.textId, x, 4);
        memcpy(&e.align, x + 8, 4);
        e.sx = F32(x, 3);
        e.sy = F32(x, 4);
        e.colourFn = FixedString(x + 20, 32);
        e.textFn = FixedString(x + 52, 32);
        return true;
    }
    case EL_BUTTON: {  // FUN_00466e90
        uint32_t rows = r.U32();
        for (uint32_t i = 0; i < rows && r.ok; ++i) {
            std::vector<Element> row;
            uint32_t n = r.U32();
            for (uint32_t k = 0; k < n && r.ok; ++k) {
                Element child;
                int t = r.U8();
                if (t != EL_SPRITE && t != EL_TEXT) return false;
                if (!ParseElement(r, sizes, t, child)) return false;
                child.x += e.x;  // children are relative to the button
                child.y += e.y;
                row.push_back(std::move(child));
            }
            e.rows.push_back(std::move(row));
        }
        const uint8_t* t = r.Take(sizes[5]);
        if (!t || sizes[5] < 0x44 + 6 * 32) return false;
        e.enabled = t[0] != 0;
        for (int i = 0; i < 8; ++i) {
            uint8_t kind = t[4 + i * 8];
            int32_t target;
            memcpy(&target, t + 8 + i * 8, 4);
            if (kind != 0xff) { e.links[i].kind = kind; e.links[i].target = target; }
        }
        for (int i = 0; i < 8 && 0x44 + (i + 1) * 32 <= int(sizes[5]); ++i)
            e.actions[i] = FixedString(t + 0x44 + i * 32, 32);
        return true;
    }
    case EL_FMV:  // 0x466a00
        return true;
    }
    return false;
}

}  // namespace

// ------------------------------------------------------------------ DDS ----
bool DecodeDds(const uint8_t* d, size_t size, Texture& out)
{
    if (size < 128 || memcmp(d, "DDS ", 4) != 0) return false;
    uint32_t h, w;
    memcpy(&h, d + 12, 4);
    memcpy(&w, d + 16, 4);
    char fcc[5] = {};
    memcpy(fcc, d + 84, 4);
    bool dxt1 = !strcmp(fcc, "DXT1"), dxt3 = !strcmp(fcc, "DXT3"), dxt5 = !strcmp(fcc, "DXT5");
    if (!dxt1 && !dxt3 && !dxt5) return false;
    size_t block = dxt1 ? 8 : 16;
    if (128 + size_t((w + 3) / 4) * ((h + 3) / 4) * block > size) return false;

    out.w = int(w);
    out.h = int(h);
    out.px.assign(size_t(w) * h, 0);
    const uint8_t* p = d + 128;
    auto rgb = [](uint16_t c, uint32_t& r, uint32_t& g, uint32_t& b) {
        r = (c >> 11 & 31) * 255 / 31; g = (c >> 5 & 63) * 255 / 63; b = (c & 31) * 255 / 31;
    };
    for (uint32_t by = 0; by < (h + 3) / 4; ++by)
        for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx, p += block) {
            uint32_t alpha[16];
            const uint8_t* cb = p;
            if (dxt3) {
                for (int k = 0; k < 16; ++k) alpha[k] = ((p[k / 2] >> (4 * (k & 1))) & 15) * 17;
                cb = p + 8;
            } else if (dxt5) {
                uint32_t a0 = p[0], a1 = p[1], pal[8] = { a0, a1 };
                for (int i = 2; i < 8; ++i)
                    pal[i] = a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7
                                     : (i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : (i == 6 ? 0 : 255));
                uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= uint64_t(p[2 + i]) << (8 * i);
                for (int k = 0; k < 16; ++k) alpha[k] = pal[(bits >> (3 * k)) & 7];
                cb = p + 8;
            } else {
                for (int k = 0; k < 16; ++k) alpha[k] = 255;
            }
            uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
            uint32_t bits = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | cb[7] << 24);
            uint32_t r[4], g[4], b[4];
            rgb(c0, r[0], g[0], b[0]);
            rgb(c1, r[1], g[1], b[1]);
            bool four = !dxt1 || c0 > c1;
            if (four) {
                r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
                r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
            } else {
                r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
                r[3] = g[3] = b[3] = 0;
            }
            for (int k = 0; k < 16; ++k) {
                uint32_t px = bx * 4 + k % 4, py = by * 4 + k / 4;
                if (px >= w || py >= h) continue;
                int i = (bits >> (2 * k)) & 3;
                uint32_t a = (!four && i == 3) ? 0 : alpha[k];
                out.px[py * w + px] = (a << 24) | (r[i] << 16) | (g[i] << 8) | b[i];
            }
        }
    return true;
}

// --------------------------------------------------------------- Canvas ----
// D3DX box filter: each level averages 2x2 texels of the one above, colour
// and alpha separately (straight colour).
void Texture::BuildMips()
{
    mips.clear();
    int lw = w, lh = h;
    const std::vector<uint32_t>* src = &px;
    while (lw > 1 || lh > 1) {
        int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
        Level l{ nw, nh, std::vector<uint32_t>(size_t(nw) * nh) };
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                uint32_t sum[4] = {};
                for (int k = 0; k < 4; ++k) {
                    int sx = std::min(lw - 1, 2 * x + (k & 1)), sy = std::min(lh - 1, 2 * y + (k >> 1));
                    uint32_t p = (*src)[size_t(sy) * lw + sx];
                    for (int c = 0; c < 4; ++c) sum[c] += p >> (8 * c) & 255;
                }
                uint32_t q = 0;
                for (int c = 0; c < 4; ++c) q |= ((sum[c] + 2) / 4) << (8 * c);
                l.px[size_t(y) * nw + x] = q;
            }
        mips.push_back(std::move(l));
        src = &mips.back().px;
        lw = nw;
        lh = nh;
    }
}

void Canvas::Draw(const Texture& t, float u0, float v0, float u1, float v1, float dx, float dy,
                  float dw, float dh, const float rgba[4], bool additive)
{
    if (t.px.empty() || dw <= 0 || dh <= 0) return;
    // Sample position inside a pixel (measured); a pixel is drawn when that
    // point is in [d, d + size).
    const float cx = 0.15f, cy = 0.25f;
    int x0 = std::max(0, int(std::ceil(dx - cx))), y0 = std::max(0, int(std::ceil(dy - cy)));
    int x1 = std::min(w, int(std::ceil(dx + dw - cx))), y1 = std::min(h, int(std::ceil(dy + dh - cy)));
    const float ta = std::clamp(rgba[3], 0.f, 1.f);
    const int mr = int(std::clamp(rgba[0], 0.f, 1.f) * ta * 256);
    const int mg = int(std::clamp(rgba[1], 0.f, 1.f) * ta * 256);
    const int mb = int(std::clamp(rgba[2], 0.f, 1.f) * ta * 256);
    const int ma = int(ta * 256);

    // Mip level: log2 of the larger texels-per-pixel ratio, linear between
    // the two nearest levels; magnified sprites use level 0.
    int lvA = 0, lvB = 0;
    int wB = 0;  // weight of level B, 0..256
    if (!t.mips.empty()) {
        float rho = std::max(std::fabs(u1 - u0) * t.w / dw, std::fabs(v1 - v0) * t.h / dh);
        float lod = rho > 1.f ? std::log2(rho) : 0.f;
        lod = std::min(lod, float(t.mips.size()));
        lvA = int(lod);
        lvB = std::min(lvA + 1, int(t.mips.size()));
        wB = int((lod - lvA) * 256);
    }
    auto level = [&](int i, int& lw, int& lh) -> const std::vector<uint32_t>& {
        if (i == 0) { lw = t.w; lh = t.h; return t.px; }
        const Texture::Level& l = t.mips[size_t(i - 1)];
        lw = l.w; lh = l.h;
        return l.px;
    };
    // Bilinear sample of one level at (u, v), channels 0..255 (b, g, r, a),
    // straight colour.
    auto sample = [&](int lv, float u, float v, int out[4]) {
        int lw, lh;
        const std::vector<uint32_t>& p = level(lv, lw, lh);
        float fx = u * lw - 0.5f, fy = v * lh - 0.5f;
        int tx = int(std::floor(fx)), ty = int(std::floor(fy));
        int wx = int((fx - tx) * 256), wy = int((fy - ty) * 256);
        int tx0 = std::clamp(tx, 0, lw - 1), tx1 = std::clamp(tx + 1, 0, lw - 1);
        int ty0 = std::clamp(ty, 0, lh - 1), ty1 = std::clamp(ty + 1, 0, lh - 1);
        uint32_t q[4] = { p[size_t(ty0) * lw + tx0], p[size_t(ty0) * lw + tx1], p[size_t(ty1) * lw + tx0],
                          p[size_t(ty1) * lw + tx1] };
        int ch[4][4];
        for (int n = 0; n < 4; ++n)
            for (int k = 0; k < 4; ++k) ch[n][k] = int(q[n] >> (8 * k) & 255);
        for (int k = 0; k < 4; ++k) {
            int top = ch[0][k] * (256 - wx) + ch[1][k] * wx;
            int bot = ch[2][k] * (256 - wx) + ch[3][k] * wx;
            out[k] = (top * (256 - wy) + bot * wy) >> 16;
        }
    };

    for (int y = y0; y < y1; ++y) {
        float v = v0 + (v1 - v0) * ((y + cy - dy) / dh);
        uint32_t* row = px + size_t(y) * w;
        for (int x = x0; x < x1; ++x) {
            float u = u0 + (u1 - u0) * ((x + cx - dx) / dw);
            int s[4];
            sample(lvA, u, v, s);
            if (wB > 0) {
                int sB[4];
                sample(lvB, u, v, sB);
                for (int k = 0; k < 4; ++k) s[k] = (s[k] * (256 - wB) + sB[k] * wB) >> 8;
            }
            // Straight colour is premultiplied after filtering (SRCALPHA blend).
            for (int k = 0; k < 3; ++k) s[k] = s[k] * s[3] / 255;
            uint32_t sb = uint32_t(s[0]) * mb >> 8, sg = uint32_t(s[1]) * mg >> 8, sr = uint32_t(s[2]) * mr >> 8,
                     sa = uint32_t(s[3]) * ma >> 8;
            uint32_t dpx = row[x];
            uint32_t db = dpx & 255, dg = dpx >> 8 & 255, dr = dpx >> 16 & 255;
            if (additive) {
                db = std::min(255u, db + sb); dg = std::min(255u, dg + sg); dr = std::min(255u, dr + sr);
            } else {
                db = sb + db * (255 - sa) / 255; dg = sg + dg * (255 - sa) / 255; dr = sr + dr * (255 - sa) / 255;
            }
            row[x] = 0xff000000u | (std::min(dr, 255u) << 16) | (std::min(dg, 255u) << 8) | std::min(db, 255u);
        }
    }
}

void Canvas::Fill(float dx, float dy, float dw, float dh, uint32_t bgra)
{
    int x0 = std::max(0, int(dx)), y0 = std::max(0, int(dy));
    int x1 = std::min(w, int(dx + dw)), y1 = std::min(h, int(dy + dh));
    for (int y = y0; y < y1; ++y)
        std::fill(px + size_t(y) * w + x0, px + size_t(y) * w + x1, bgra);
}

// ----------------------------------------------------------------- Font ----
// FONT.BNK: CHNK { FONT: CHNK { UPST, MDPC (glyph pages), FBNK } }
// FBNK: u32 hash; u16 count; u8; u8 lineHeight; ...; glyph[count] at +16:
//   u16 char; u16 w:6 h:6 page:4; u8 x; u8 y; u16 (yoff = bits 5..12)
// followed by a 52x52 spacing table for A-Z a-z (advance = w - table).
bool Font::Load(const std::string& path)
{
    std::vector<uint8_t> d = ReadFile(path);
    size_t fb, fbSize, md, mdSize;
    if (!FindChunk(d, 0, "FBNK", fb, fbSize) || !FindChunk(d, 0, "MDPC", md, mdSize)) return false;
    pages_ = LoadMaterials(d, md, mdSize);
    uint16_t count;
    memcpy(&count, &d[fb + 4], 2);
    if (d[fb + 6]) linePitch_ = d[fb + 6];
    topOffset_ = d[fb + 8];
    size_t p = fb + 16;
    for (int i = 0; i < count && p + 8 <= d.size(); ++i, p += 8) {
        uint16_t ch, a, b;
        memcpy(&ch, &d[p], 2);
        memcpy(&a, &d[p + 2], 2);
        memcpy(&b, &d[p + 6], 2);
        // +6: bits 0..4 left bearing, 5..9 and 10..14 the y offset's two
        // parts (a - b, 0x5754b1; b is 8 in every glyph and is the FBNK
        // top offset subtracted by the callers).
        glyphs_[wchar_t(ch)] = { d[p + 4], d[p + 5], a & 63, (a >> 6) & 63, a >> 12, (b >> 5) & 31, b & 31 };
    }
    if (p + 52 * 52 <= d.size()) kern_.assign(d.begin() + p, d.begin() + p + 52 * 52);
    return !pages_.empty();
}

// Kerning (0x575750): both characters are first reduced to a plain letter
// (0x5757f0: accented Latin-1 letters to their base letter), then the table
// gives table[previous][current] for A-Z / a-z; any other pair is 0.
int Font::Kern(wchar_t a, wchar_t b) const
{
    static const char kBase[] = "AAAAAA.CEEEEIIIIDNOOOOO.OUUUUY..aaaaaa.ceeeeiiii.nooooo.ouuuuy.y";  // 0xC0..0xFF
    auto idx = [](wchar_t c) {
        if (c >= 0xC0 && c <= 0xFF && kBase[c - 0xC0] != '.') c = wchar_t(kBase[c - 0xC0]);
        if (c >= L'A' && c <= L'Z') return int(c - L'A');
        if (c >= L'a' && c <= L'z') return int(c - L'a') + 26;
        return -1;
    };
    int i = idx(a), j = idx(b);
    if (i < 0 || j < 0 || kern_.empty()) return 0;
    return kern_[size_t(i) * 52 + j];
}

float Font::Width(const std::wstring& s, float sx) const
{
    float x = 0;
    wchar_t prev = 0;
    for (wchar_t c : s) {
        auto it = glyphs_.find(c);
        if (it == glyphs_.end()) { x += spaceWidth_; prev = 0; continue; }
        if (prev) x -= Kern(prev, c);
        x += it->second.w + it->second.bear;
        prev = c;
    }
    return x * sx;
}

void Font::Draw(Canvas& cv, const std::wstring& s, float x, float y, float sx, float sy,
                const float rgba[4]) const
{
    float pen = x;
    wchar_t prev = 0;
    for (wchar_t c : s) {
        auto it = glyphs_.find(c);
        if (it == glyphs_.end()) { pen += spaceWidth_ * sx; prev = 0; continue; }
        const Glyph& g = it->second;
        if (prev) pen -= Kern(prev, c) * sx;
        if (g.page < int(pages_.size())) {
            const Texture& t = pages_[g.page];
            cv.Draw(t, float(g.x) / t.w, float(g.y) / t.h, float(g.x + g.w) / t.w, float(g.y + g.h) / t.h,
                    pen + g.bear * sx, y + g.yoff * sy, g.w * sx, g.h * sy, rgba);
        }
        pen += (g.w + g.bear) * sx;
        prev = c;
    }
}

// -------------------------------------------------------------- MecFile ----
bool MecFile::Load(const std::string& path)
{
    std::vector<uint8_t> d = ReadFile(path);
    size_t rm, rmSize, md, mdSize;
    if (!FindChunk(d, 0, "RMDL", rm, rmSize)) return false;
    if (FindChunk(d, 0, "MDPC", md, mdSize)) materials = LoadMaterials(d, md, mdSize);

    Reader r{ d, rm, std::min(d.size(), rm + rmSize) };
    uint32_t version = r.U32(), count = r.U32();
    uint32_t sizes[7] = { 8, 8, 0x20, 0x114, 0x14, 0x294, 0 };  // pre-0x131 defaults
    if (version >= 0x131) {
        int n = version < 0x134 ? 6 : 7;
        for (int i = 0; i < n; ++i) sizes[i] = r.U32();
    }
    for (uint32_t s = 0; s < count && r.ok; ++s) {
        MecScreen sc;
        const uint8_t* h = r.Take(sizes[1]);
        if (!h || sizes[1] < 81) return false;
        sc.name = FixedString(h, 8);
        uint32_t focus;
        memcpy(&focus, h + 12, 4);
        sc.focus = int(focus);
        sc.overlay = h[16] != 0;
        sc.clearsHistory = sizes[1] > 81 && h[81] != 0;
        sc.onEnter = FixedString(h + 17, 32);
        sc.onExit = FixedString(h + 49, 32);
        uint32_t groups = r.U32();
        for (uint32_t g = 0; g < groups && r.ok; ++g) {
            uint32_t n = r.U32();
            for (uint32_t k = 0; k < n && r.ok; ++k) {
                Element e;
                if (!ParseElement(r, sizes, r.U8(), e)) return false;
                sc.items.push_back(std::move(e));
            }
        }
        if (sc.focus < 0 || sc.focus >= int(sc.items.size())) sc.focus = 0;
        sc.defaultFocus = sc.focus;
        screens.push_back(std::move(sc));
    }
    return r.ok && !screens.empty();
}

// The GUI compares screen names with _strnicmp(.., 7) (0x4677fa, 0x4678a2):
// names are stored with 7 characters, and the code asks for "Checking",
// "LODOyesno", ... (MUMAIN screens "Checkin", "LODOyes").
int MecFile::Find(const std::string& name) const
{
    for (size_t i = 0; i < screens.size(); ++i)
        if (_strnicmp(screens[i].name.c_str(), name.c_str(), 7) == 0) return int(i);
    return -1;
}
