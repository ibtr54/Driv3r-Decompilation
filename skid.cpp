#include "skid.h"

#include <cmath>
#include <cstring>

namespace {

// 0x71c2f8: the surface table (0x14 bytes a row); the first dword is the
// skid colour 0xRRGGBBAA (rows 0..11; road = 6: 0x30404060).
const uint32_t kSkidColour[] = {
    0x80808080, 0x80808080, 0xc8c87060, 0x20200060, 0x80808080, 0x80808080,
    0x30404060, 0x80808080, 0x80808080, 0x80808080, 0x80808080, 0x80808080,
};

}  // namespace

void SkidRibbon::Reset()
{
    // 0x56ed50 (the fields) and 0x56ede0 (the colours)
    head = count = 0;
    width = 0.2f;
    intensity = 0;
    on = false;
    fresh = true;
    memset(lastPair, 0, sizeof(lastPair));
    memset(dir, 0, sizeof(dir));
    life = 10;
    segFirst = segSecond = 0;
    memset(v, 0, sizeof(v));
    for (SkidVertex& x : v)
        for (float& c : x.colour) c = 1.0f;
}

// 0x56ef60: the pair at head (a: v 0, b: v 1, both u = length), head + 2
// (wrapping at 1600), count + 2 (at most 1600).
void SkidRibbon::Pair(const float a[3], const float b[3], const float n[3], const float colour[4])
{
    SkidVertex* p = &v[head];
    const float* src[2] = { a, b };
    for (int k = 0; k < 2; ++k) {
        memcpy(p[k].pos, src[k], 12);
        memcpy(p[k].normal, n, 12);
        p[k].uv[0] = length;
        p[k].uv[1] = float(k);
        memcpy(p[k].colour, colour, 16);
    }
    head += 2;
    if (head >= uint32_t(kVertices)) head = 0;
    count += 2;
    if (count >= uint32_t(kVertices)) count = kVertices;
}

// 0x56f070: the last pair once more (from head - 2, or 1598 at the start).
uint32_t SkidRibbon::Repeat()
{
    const uint32_t at = head;
    const uint32_t from = int(head) >= 2 ? head - 2 : 0x63e;
    memcpy(&v[at], &v[from], sizeof(SkidVertex));
    memcpy(&v[at + 1], &v[from + 1], sizeof(SkidVertex));
    head += 2;
    if (head >= uint32_t(kVertices)) head = 0;
    count += 2;
    if (count >= uint32_t(kVertices)) count = kVertices;
    return at;
}

// 0x56f100: fill the ring's end with copies (fewer than 6 left), then one
// more copy whose u (and the length) loses its whole part (floor, 0x5e3230).
void SkidRibbon::Rebase()
{
    while (uint32_t(kVertices) - head < 6) Repeat();
    const uint32_t at = Repeat();
    const float whole = float(std::floor(double(v[at].uv[0])));
    v[at].uv[0] = v[at].uv[0] - whole;
    v[at + 1].uv[0] = v[at + 1].uv[0] - whole;
    length = length - whole;
}

// 0x56eef0 -> 0x56ee40: the last pair (head - 2, or 1598) moved to a / b
// with u.
void SkidRibbon::Move(const float a[4], const float b[4], const float n[3], float u, const float colour[4])
{
    const uint32_t at = head >= 2 ? head - 2 : 0x63e;
    SkidVertex* p = &v[at];
    const float* src[2] = { a, b };
    for (int k = 0; k < 2; ++k) {
        memcpy(p[k].pos, src[k], 12);
        memcpy(p[k].normal, n, 12);
        p[k].uv[0] = u;
        p[k].uv[1] = float(k);
        memcpy(p[k].colour, colour, 16);
    }
}

void SkidRibbon::Add(const float p[4], const float vel[4], const float n[4], int surface, const float mood[3])
{
    if (life) --life;
    if (!(1e-10f <= ((vel[0] * vel[0] + vel[1] * vel[1]) + vel[2] * vel[2]) + vel[3] * vel[3])) return;
    if (!on) return;
    // the centre, 0.1 ([0x7a2240]) above the point along the normal
    float c[4];
    for (int k = 0; k < 4; ++k) c[k] = 0.1f * n[k] + p[k];
    // across the path: normalise(n x vel) x width (0x41ea80, 0x4025d0, w 0)
    float s[4] = { n[1] * vel[2] - n[2] * vel[1], n[2] * vel[0] - n[0] * vel[2], n[0] * vel[1] - n[1] * vel[0], 0.0f };
    const float inv = 1.0f / std::sqrt((s[0] * s[0] + s[1] * s[1]) + s[2] * s[2]);
    for (int k = 0; k < 4; ++k) s[k] = inv * s[k];
    for (int k = 0; k < 4; ++k) s[k] = width * s[k];
    float a[4], b[4];
    for (int k = 0; k < 4; ++k) a[k] = s[k] + c[k], b[k] = c[k] - s[k];
    // the colour: the surface's (0..255) x the mood's x 1 / 255, alpha x
    // the intensity
    const uint32_t t = kSkidColour[surface >= 0 && surface < 12 ? surface : 0];
    const float tc[4] = { float(t >> 24), float((t >> 16) & 0xff), float((t >> 8) & 0xff), float(t & 0xff) };
    const float m[4] = { mood[0], mood[1], mood[2], 1.0f };
    float colour[4];
    for (int k = 0; k < 4; ++k) colour[k] = (m[k] * tc[k]) * 0.00392156886f;
    colour[3] = colour[3] * intensity;
    if (fresh) {
        // a new segment: joined to the strip by repeated vertices
        length = 0;
        while (uint32_t(kVertices) - head < 6) Pair(lastPair, a, n, colour);
        segFirst = head;
        if (head != 0) Pair(lastPair, a, n, colour);
        Pair(a, b, n, colour);
        segSecond = head + 1;
        Pair(a, b, n, colour);
        memset(dir, 0, sizeof(dir));
        memcpy(last, b, sizeof(last));
        fresh = false;
    } else {
        float d[4];
        for (int k = 0; k < 4; ++k) d[k] = b[k] - last[k];
        if (std::sqrt((dir[2] * dir[2] + dir[1] * dir[1]) + dir[0] * dir[0]) < 0.001f) memcpy(dir, d, sizeof(dir));
        // |d| (0x56fa02..: the squares w first; the x87 runs at single
        // precision here, run 30's u are bit-exact so) and u = |d| + length
        auto Len = [](const float* x) { return std::sqrt(((x[3] * x[3] + x[2] * x[2]) + x[1] * x[1]) + x[0] * x[0]); };
        const float step = Len(d);
        const float u = step + length;
        // the angle to the segment's direction against 6 degrees ([0x7a2258]
        // x pi / 180, the game's cos 0x401070)
        const float limit = std::cos(6.0f * 0.0174532924f);
        const float dot = ((d[3] * dir[3] + d[2] * dir[2]) + d[1] * dir[1]) + d[0] * dir[0];
        const float cosA = dot / (step * Len(dir));
        if (cosA <= limit || 15.8999996f <= u) {
            if (head == 0 || 15.8999996f <= u) Rebase();
            length = step + length;
            Pair(a, b, n, colour);
            memcpy(last, b, sizeof(last));
        } else {
            Move(a, b, n, u, colour);
        }
    }
    // the ring's join: the vertex at head repeats head - 1, head + 1
    // repeats head + 2 (at the end itself); at head 0 the game reads the 48
    // bytes before the ring (the object's start), the program the last vertex
    if (count > 3) {
        memcpy(&v[head], &v[head ? head - 1 : kVertices - 1], sizeof(SkidVertex));
        const uint32_t from = head < 0x63e ? head + 2 : head;
        memcpy(&v[head + 1], &v[from], sizeof(SkidVertex));
    }
    memcpy(lastPair, b, sizeof(lastPair));
}

void SkidPoint(const float local[3], float lift, const float M[16], float width, int wheel, int model, float out[4])
{
    // half = -row 0 x width ([0x78e904] = -1)
    float half[4];
    for (int k = 0; k < 4; ++k) half[k] = (M[k] * -1.0f) * width;
    const float L[4] = { local[0], local[1] + lift, local[2], 1.0f };
    float P[4];
    for (int k = 0; k < 4; ++k) P[k] = ((L[0] * M[k] + L[1] * M[4 + k]) + L[2] * M[8 + k]) + L[3] * M[12 + k];
    const bool minus = wheel == 3 || wheel == 5 || wheel == 2;
    for (int k = 0; k < 4; ++k) P[k] = minus ? P[k] + half[k] : P[k] + -half[k];
    if (model == 0x23 || model == 0x3b)
        for (int k = 0; k < 4; ++k) P[k] = M[4 + k] * 0.100000001f + P[k];
    memcpy(out, P, sizeof(P));
}
