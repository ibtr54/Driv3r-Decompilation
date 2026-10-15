// Drawing the city with the game's own effects. Driv3r.exe holds its
// shaders as compiled D3DX effects (fx_2_0) and creates them all at start
// (0x5d3140): one shared effect for the per-frame values, in an effect
// pool, and one effect per id at [manager + 0x2c + id * 4]. Here they are
// read from the game's executable and created the same way through
// d3dx9_43.dll (next to the program; the game links D3DX statically).
//
// Per model, the render id comes from the vertex type (0x5cc3d0); per
// group, the effect from the id and the material's first substance
// (0x5d68e0, table 0x8accc8). An effect draws with its first valid
// technique (0x5d5790), "World" = transpose(instance matrix) x submesh
// matrix (0x5de4e0), "ViewProjection" = transpose(camera matrix) (0x5dc4b0),
// g_fBrightness 0.651 (0x5de330), and the group's DrawIndexedPrimitive
// (0x5dfe20). Not done yet: textures (a white texture stands in for every
// texture parameter), the mood's light and fog (the effects' defaults are
// kept), draw distances, and the game's camera.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <string>
#include <vector>

#include "city.h"
#include "mdpc.h"

// A stand-in camera (the game's camera is not traced): left-handed look-at
// (y up) times a perspective projection, row vector (Direct3D convention).
void CameraMatrix(const float eye[3], const float at[3], float fovY, float aspect, float zNear, float zFar, float out[16]);

struct ID3DXEffect;
struct ID3DXEffectPool;

class CityRenderer
{
public:
    ~CityRenderer() { Release(); }
    // Loads the effects out of the game's executable.
    bool Init(IDirect3DDevice9* device, const std::string& exePath);
    // The city's packages (packages[first, end)) and its placement.
    bool Upload(const std::vector<MdpcPackage>& packages, size_t first, size_t end, const CityPlacement& city);
    // viewProj: row-vector view x projection (Direct3D); eye: the camera.
    // Instances whose box centre is farther than range are skipped.
    void Draw(const float viewProj[16], const float eye[3], float range);
    void OnLostDevice();
    void OnResetDevice();
    void Release();
    const std::string& Error() const { return error_; }
    // Counts for the log: effects loaded; groups that will be drawn and
    // groups left out (no effect for their render id, or a vertex layout
    // that is not created yet); last frame's instances and draw calls.
    int effects = 0;
    std::string failedEffects;  // ids that D3DX did not create
    size_t groupsDrawn = 0, groupsSkipped = 0, frameInstances = 0, frameDraws = 0;

private:
    struct Package
    {
        const MdpcPackage* data = nullptr;
        std::vector<IDirect3DVertexBuffer9*> vbs;
        IDirect3DIndexBuffer9* ib = nullptr;
        // Per model, per slot 0 submesh, per group: the effect id (-1: not drawn).
        std::vector<std::vector<std::vector<int>>> effectOf;
    };
    struct Instance
    {
        const CityInstance* in = nullptr;
        int package = -1;
        uint32_t model = 0;
        float centre[3] = {};
    };
    struct Effect
    {
        ID3DXEffect* fx = nullptr;
        const char* world = nullptr;  // D3DXHANDLEs
        const char* alpha = nullptr;
        const char* brightness = nullptr;
    };

    HMODULE dll_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    ID3DXEffectPool* pool_ = nullptr;
    ID3DXEffect* shared_ = nullptr;
    const char* viewProj_ = nullptr;
    const char* camPos_ = nullptr;
    Effect fx_[0x30];
    IDirect3DVertexDeclaration9* decl_ = nullptr;
    IDirect3DTexture9* white_ = nullptr;
    std::vector<Package> packages_;
    std::vector<Instance> instances_;
    std::string error_;
};
