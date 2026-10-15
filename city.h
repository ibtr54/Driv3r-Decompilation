// Where the city places its models: the GEBI chunks of a .d3c. Each
// GEBI is 0x40 bytes of header and then 0x20-byte instance records; one
// sits in every non-empty GEIR cell (a square grid of 480-unit cells
// centred on 0) and one in the first GESR (header +4 = -2). A record names a
// model of a package in <city>\SuperRegions.pcs and places it with a
// position and a rotation. Read like the GEBI handler (vtbl 0x780de8,
// load 0x44cf90), the matrix as 0x473830 / 0x59f650 build it and the
// model as 0x5cbdd0 / 0x5dcc50 look it up (package table 0x8b5310,
// filled by uid in 0x57c058).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CityInstance
{
    float position[3] = {};   // +0x00 x, y (up), z
    uint16_t model = 0;       // +0x0c package uid << 10 | model index
                              // (index 0x3ff: no model)
    uint16_t extra = 0;       // +0x0e (0xffff in most records)
    float rotation[4] = {};   // +0x10: cos, sin, -, flag > 100 (yaw form)
                              // or a quaternion x, y, z, w
    // Row-major, row vector times matrix (Direct3D): rows 0-2 the rotation,
    // row 3 the position.
    float world[16] = {};
    int column = 0, row = 0;  // GEBI header +4 / +8 (-2 / -2: the GESR one)
    int range = 0;            // which of the header's six ranges (+0x0c..)

    bool HasModel() const { return (model & 0x3ff) != 0x3ff; }
    uint32_t PackageUid() const { return model >> 10; }
    uint32_t ModelIndex() const { return model & 0x3ff; }
    // A point in model space to the world.
    void Transform(const float* p, float out[3]) const
    {
        for (int k = 0; k < 3; ++k)
            out[k] = p[0] * world[k] + p[1] * world[4 + k] + p[2] * world[8 + k] + world[12 + k];
    }
};

class CityPlacement
{
public:
    // All GEBI chunks of a .d3c file (GEIR cells and GESR regions).
    bool Load(const std::vector<uint8_t>& d3c);
    const std::string& Error() const { return error_; }

    std::vector<CityInstance> instances;
    // GEIR cells per side (12 in Miami, 15 in Nice and Istanbul); a cell
    // is 480 units and the grid is centred on 0, so column c starts at
    // x = 480 * (c - gridSide / 2) and row r ends at z = -480 * (r - gridSide / 2)
    // (the cell's first GEBR row holds that corner).
    int gridSide = 0;

private:
    bool LoadGebi(const std::vector<uint8_t>& d, size_t off, size_t size);
    std::string error_;
};
