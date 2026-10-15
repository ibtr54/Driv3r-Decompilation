// A level's mood: Moods\MoodNN.txt, the light, fog and sky of the level.
// Created at the level start (0x52b4f0: object [0x8ac3d8], 0x471870 reads
// the file, 0x4720a0 parses it, 0x472fc0 builds the sky matrix). The game
// hands the values to the scene in 0x473240 (called from 0x539c30): the
// ambient colour to the light state (0x5cede0), the directional light
// (0x57d2d0 -> 0x5cee80) and the fog block (0x57d870); per scene 0x5dc1e0
// sets them on the shared effect (0x5cefb0 for s_FogParams and FOGCOLOR,
// 0x5dc3f0 for the light). The sky dome (SKYDOME_NAME) is one of the
// level loader's files (0x44b48d) and drawn by 0x539cb0.
#pragma once

#include <string>

struct Mood
{
    // Defaults of 0x471870, for keys a file leaves out.
    std::string skydome = "Dev:sky.d3c";            // +0x50 SKYDOME_NAME
    float sunElevation = 0.534f;                     // +0x04 SUN_ELEVATION
    float ambient[4] = { 0.3984375f, 0.3984375f, 0.3984375f, 1 };      // +0x180 LIGHT_AMBIENT_COLOUR
    float directional[4] = { 1.1015625f, 1.1015625f, 1.1015625f, 1 };  // +0x1c0 LIGHT_DIRECTIONAL_COLOUR
    float direction[4] = { 0.579f, -0.573f, -0.579f, 0 };             // +0x1b0 LIGHT_DIRECTIONAL_VECTOR
    float fogNearDistance = 50, fogNearValue = 0;    // +0x230 / +0x234 FOG_NEARDISTANCE / FOG_NEARVALUE
    float fogFarDistance = 1050, fogFarValue = 1350; // +0x238 / +0x23c FOG_FARDISTANCE / FOG_FARVALUE
    float fogColour[4] = { 0.5f, 0.5f, 0.6f, 1 };    // +0x240 FOG_COLOUR
    float skyColour[4] = { 1, 1, 1, 1 };             // +0x160 SKY_COLOUR (no shipped mood sets it)
    // +0x10: the sky dome's rotation (0x472fc0), row vector times matrix:
    // row 0 = the light direction flattened (y = 0) and normalized, row 1 =
    // up, row 2 = row 0 x row 1, row 3 = (0, 0, 0, 1).
    float skyMatrix[16] = {};

    // Reads the file like 0x471870 / 0x4720a0 (at most 0x800 bytes and
    // 0x23 lines; a line's key is compared at its start). False when the
    // file cannot be opened (the game then keeps the defaults, 0x472fc0).
    bool Load(const std::string& path);
    // s_FogParams of 0x5cefb0: (-1 / (far - near), far / (far - near),
    // 1 - clamp(far value / 100, 0, 1), 70). The effects fog with
    // max(distance x .x + .y, .z), 1 = no fog: FOG_NEARVALUE is not used.
    void FogParams(float out[4]) const;
    // D3DRS_FOGCOLOR of 0x5cefb0: 0xff000000 | the colour x 255, truncated.
    unsigned long FogColour() const;

private:
    void BuildSkyMatrix();
};

// The mood of a Take A Ride level (game setup vtbl+0x6c, 0x464270): the
// weather (+0x15b4: 0 dry, 1 overcast, 2 wet) picks a block, the city
// (+0x14: 1 Miami, 3 Nice, 5 Istanbul) an offset in it, the time (+0x34:
// 0 dawn, 1 day, 2 dusk, 3 night) the entry:
//   dry 0x20 + 8 x c + time, overcast 0x38 + 4 x c + time,
//   wet 0x24 + 8 x c + time  (c = 0 Miami, 1 Nice, 2 Istanbul).
// city here: 0 Miami, 1 Nice, 2 Istanbul. Other levels take the mood from
// the setup (+0x18, from the mission; not decoded).
int RideMoodNumber(int city, int time, int weather);
