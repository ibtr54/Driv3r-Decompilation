#include "mood.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

bool Mood::Load(const std::string& path)
{
    // 0x471870: a 0x801-byte buffer, cleared, and one read of 0x800 bytes.
    std::vector<char> buf(0x801, 0);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        BuildSkyMatrix();
        return false;
    }
    size_t n = fread(buf.data(), 1, 0x800, f);
    fclose(f);
    (void)n;
    // 0x4720a0: line starts (every entry starts as the buffer), each '\n'
    // ends a line, at most 0x23 lines.
    char* lines[0x23];
    for (char*& l : lines) l = buf.data();
    int count = 1;
    for (int i = 0; i < 0x800 && count < 0x23 && buf[i]; ++i)
        if (buf[i] == '\n') {
            lines[count++] = &buf[i + 1];
            buf[i] = 0;
        }
    auto is = [](const char* line, const char* key) { return strncmp(key, line, strlen(key)) == 0; };
    auto rgb = [](const char* line, const char* fmt, float out[4], float w) {
        float c[3] = { 1, 1, 1 };
        sscanf(line, fmt, &c[0], &c[1], &c[2]);
        out[0] = c[0];
        out[1] = c[1];
        out[2] = c[2];
        out[3] = w;
    };
    for (int i = 0; i < count; ++i) {
        const char* l = lines[i];
        if (is(l, "SKYDOME_NAME")) {
            char name[64] = {};
            sscanf(l, "SKYDOME_NAME: \"%63s\"", name);
            size_t len = strlen(name);
            if (len && name[len - 1] == '"') name[len - 1] = 0;  // %63s keeps the closing quote
            skydome = name;
        } else if (is(l, "SUN_ELEVATION")) {
            sscanf(l, "SUN_ELEVATION: %f", &sunElevation);
        } else if (is(l, "LIGHT_AMBIENT_COLOUR")) {
            rgb(l, "LIGHT_AMBIENT_COLOUR: %f, %f, %f", ambient, 1);
        } else if (is(l, "LIGHT_DIRECTIONAL_COLOUR")) {
            rgb(l, "LIGHT_DIRECTIONAL_COLOUR: %f, %f, %f", directional, 1);
        } else if (is(l, "LIGHT_DIRECTIONAL_VECTOR")) {
            rgb(l, "LIGHT_DIRECTIONAL_VECTOR: %f, %f, %f", direction, 0);
        } else if (is(l, "SHADOW_VECTOR")) {
            rgb(l, "SHADOW_VECTOR: %f, %f, %f", shadow, 0);
        } else if (is(l, "FOG_NEARDISTANCE")) {
            sscanf(l, "FOG_NEARDISTANCE: %f", &fogNearDistance);
        } else if (is(l, "FOG_NEARVALUE")) {
            sscanf(l, "FOG_NEARVALUE: %f", &fogNearValue);
        } else if (is(l, "FOG_FARDISTANCE")) {
            sscanf(l, "FOG_FARDISTANCE: %f", &fogFarDistance);
        } else if (is(l, "FOG_FARVALUE")) {
            sscanf(l, "FOG_FARVALUE: %f", &fogFarValue);
        } else if (is(l, "FOG_COLOUR")) {
            rgb(l, "FOG_COLOUR: %f, %f, %f", fogColour, 1);
        } else if (is(l, "SKY_COLOUR")) {
            rgb(l, "SKY_COLOUR: %f, %f, %f", skyColour, 1);
        } else if (is(l, "SHORE_WAVE_COLOUR")) {
            rgb(l, "SHORE_WAVE_COLOUR: %f, %f, %f", shoreWave, 1);
        } else if (is(l, "SHORE_WAVE_INTENSITY")) {
            sscanf(l, "SHORE_WAVE_INTENSITY: %f", &shoreWave[3]);
        } else if (is(l, "SEA_COLOUR")) {
            sscanf(l, "SEA_COLOUR: %d", &seaColour);
        } else if (is(l, "CAR_SPECULAR_COLOUR")) {
            rgb(l, "CAR_SPECULAR_COLOUR: %f, %f, %f", carSpecular, 1);
        } else if (is(l, "CAR_SPECULAR_INTENSITY")) {
            sscanf(l, "CAR_SPECULAR_INTENSITY: %f", &carSpecular[3]);
        } else if (is(l, "SHINY_WATER_TINT")) {
            rgb(l, "SHINY_WATER_TINT: %f, %f, %f", shinyWaterTint, 1);
        }
        // The other keys (street / head lights, car specular,
        // rain, shiny road, sun flare, particles,
        // music, light glow, time of day, weather, kerb height, headlight
        // colours) are not used here.
    }
    BuildSkyMatrix();
    return true;
}

void Mood::BuildSkyMatrix()
{
    // 0x472fc0: (dir.x, 0, dir.z, dir.w) normalized by its x, y, z length
    // (0x4025d0), up (0, 1, 0), then their cross product (0x41ea80 ->
    // 0x407640), w = 0.
    float h[4] = { direction[0], 0, direction[2], direction[3] };
    float len = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    if (len > 0)
        for (float& v : h) v /= len;
    const float up[3] = { 0, 1, 0 };
    float c[3] = { h[1] * up[2] - h[2] * up[1], h[2] * up[0] - h[0] * up[2], h[0] * up[1] - h[1] * up[0] };
    len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (len > 0)
        for (float& v : c) v /= len;
    const float m[16] = { h[0], h[1], h[2], h[3], up[0], up[1], up[2], 0, c[0], c[1], c[2], 0, 0, 0, 0, 1 };
    memcpy(skyMatrix, m, sizeof(m));
    // +0x170: the normalized horizontal direction, y = -sin(e) / cos(e)
    // (0x401160 / 0x401070, the game's sine / cosine; e = SUN_ELEVATION),
    // normalized again (0x4025d0): (cos e x h, -sin e).
    memcpy(sun, h, sizeof(sun));
    sun[1] = -std::sin(sunElevation) / std::cos(sunElevation);
    len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    if (len > 0)
        for (float& v : sun) v /= len;
}

void Mood::FogParams(float out[4]) const
{
    float inv = 1.0f / (fogFarDistance - fogNearDistance);
    out[0] = -1.0f * inv;
    out[1] = inv * fogFarDistance;
    float v = fogFarValue * 0.01f;
    if (v > 1.0f) v = 1.0f;
    else if (!(v >= 0.0f)) v = 0.0f;
    out[2] = 1.0f - v;
    out[3] = 70.0f;
}

unsigned long Mood::FogColour() const
{
    // fistp with the rounding control set to truncate.
    unsigned long r = static_cast<unsigned long>(int(fogColour[0] * 255.0f)) & 0xff;
    unsigned long g = static_cast<unsigned long>(int(fogColour[1] * 255.0f)) & 0xff;
    unsigned long b = static_cast<unsigned long>(int(fogColour[2] * 255.0f)) & 0xff;
    return 0xff000000ul | r << 16 | g << 8 | b;
}

int RideMoodNumber(int city, int time, int weather)
{
    int base = 0x20, step = 8;
    if (weather == 1) { base = 0x38; step = 4; }
    else if (weather == 2) base = 0x24;
    int c = city == 1 ? 1 : city == 2 ? 2 : 0;
    return step * c + time + base;
}
