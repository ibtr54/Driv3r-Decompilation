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
// (0x5dfe20). Textures (0x5e0970 for ordinary objects): g_TextureDiffuse
// = the first texture of the group's substance (0x5e0af0), g_TextureNormalMap
// = the second when substance +6 is 1 or 4 (0x5e0e70); the other texture
// parameters keep a white texture. Render states: the effects' passes set
// none, the game sets them from presets (table [0x8afaac], built by
// 0x5d9500): per group the device defaults (0x5d4b90), the preset of the
// substance type (0x5cb9d0, applied by 0x5d4440 after the texture binder's
// request 0x5e0900) and the presets of its byte 1 / flags (0x5d8340), with
// their sampler states for stages 0..3; set only where the device's value
// differs, like the game's cache. Checked against a trace of the original
// (driv3r-re\tools\presets\d3d9proxy.cpp). The mood (mood.h): per frame
// the shared effect gets s_LightAmbientColour, s_LightDirectionalColour /
// Direction and s_FogParams, and the device FOGCOLOR (0x5dc1e0, 0x5cefb0,
// 0x5dc3f0); the presets switch fog on and off. The sky dome (0x539cb0) is
// drawn first: model 0 of the sky package (model id 0xd000) with the mood's
// sky matrix moved to (camera x, -5, camera z), and the light of a kind
// 0x10001 object (0x5de330): ambient = SKY_COLOUR, directional colour 0,
// direction (0, -1, 0); its substance (type 0x15 -> preset 4) keeps z
// writes and fog off. All checked against a trace of the original
// (shader constants c8..c14 and the sky's World matrix). Stand-in: blended
// groups drawn after the others (the game's draw order is not traced).
//
// Shadows (0x5d2ca0, with the profile's Shadows setting +0xc8c -> render
// context +0x148): the scene's lists are 0 (most groups), 1 (shadow
// volumes) and 2 (groups whose substance type is 0x11, 0x1d or 0x1e,
// 0x5d2ae8). Drawn: list 0 in mode 2; the shadow volumes in mode 4
// (preset 0x11: no colour or z writes, ZFUNC GREATER, stencil DECR, cull
// CW) and again in mode 5 (0x12: INCR, cull CCW); then the directional
// colour set to 0 and list 0 again in mode 7 (0x1c: stencil >= 1, colour
// writes RGB, blend DESTALPHA / INVDESTALPHA), mode 6 (0x1b: SRCALPHA /
// INVSRCALPHA) for the alpha-tested and blended types (0x5d4330); then
// list 2 in mode 2. A shadow volume (0x5d2b90) is a copy of the object
// drawn with effect 0x2b and its model's slot of mask 0x20 (slot 5, or 6
// when 5 is empty: 0x5ca020); the shader moves the vertices whose normal
// faces away from the light by g_fExtrusion (20, set by 0x5dedf0) along
// it; its World is the object's matrix moved 0.1 away from the camera.
// Which substance types each mode draws: 0x5e06f0. Checked against a trace
// of the original (stencil states, c9, the order of the modes).
// Not done yet: draw distances. The camera is the ride's start camera or
// the free one (main.cpp); the game's chase cameras need the vehicle.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "city.h"
#include "mdpc.h"
#include "mood.h"

// The camera's view-projection (+0xc0 = view +0x40 x projection +0x80,
// 0x57ce50): right-handed look-at (y up) times the game's projection, row
// vector (Direct3D convention). fov is the horizontal field of view
// (camera +0x180; the chase cameras' +0xc4 / +0xcc = pi / 3, 0x567430..),
// aspect the camera's +0x184. The traced view-projection of the original is
// right-handed (it looks along -z with +x to the right), which is why the
// game culls clockwise triangles (preset default CULL 2) with data whose
// front faces are clockwise in a left-handed view.
void CameraMatrix(const float eye[3], const float at[3], float fov, float aspect, float out[16]);

struct ID3DXEffect;
struct ID3DXEffectPool;

class CityRenderer
{
public:
    ~CityRenderer() { Release(); }
    // Loads the effects out of the game's executable.
    bool Init(IDirect3DDevice9* device, const std::string& exePath);
    // The city's packages (packages[first, end)), its placement and the
    // sky dome's package (nullptr: no sky).
    bool Upload(const std::vector<MdpcPackage>& packages, size_t first, size_t end, const CityPlacement& city,
                const MdpcPackage* sky = nullptr);
    // The level's light, fog and sky matrix (used from the next Draw).
    void SetMood(const Mood& mood) { mood_ = mood; }
    // The profile's Shadows setting (+0xc8c).
    void SetShadows(bool on) { shadows_ = on; }
    // viewProj: row-vector view x projection (Direct3D); eye: the camera.
    // The sky first, then the city; instances whose box centre is farther
    // than range are skipped.
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
    // Placed models with a shadow volume, groups in list 2, groups whose
    // type (0..5) keeps them out of mode 2; last frame's shadow volume and
    // shadow redraw draw calls.
    size_t shadowInstances = 0, lateGroups = 0, hiddenGroups = 0, frameShadowDraws = 0, frameRedraws = 0;
    // Textures created from the packages' DDS data, and those left out
    // (a format not handled, or no DDS); groups without a texture.
    size_t textures = 0, texturesSkipped = 0, groupsUntextured = 0;
    // Drawn groups per base preset ("preset x count, ..."), distinct
    // render-state sets.
    std::string presetsUsed;
    // Drawn groups per effect id ("id x count, ...").
    std::string effectsUsed;
    size_t stateSets = 0;
    // The sky's drawn groups and their effect / base preset ("" without sky).
    std::string skyInfo;

private:
    struct Package
    {
        const MdpcPackage* data = nullptr;
        std::vector<IDirect3DVertexBuffer9*> vbs;
        IDirect3DIndexBuffer9* ib = nullptr;
        std::vector<IDirect3DTexture9*> textures;  // per PCMP texture entry
        struct Group
        {
            int fx = -1;  // effect id (-1: not drawn)
            IDirect3DTexture9* diffuse = nullptr;
            IDirect3DTexture9* normal = nullptr;
            int state = 0;  // index into states_ (mode 2)
            int redraw = -1;  // states_ of the shadow redraw (mode 7 / 6), -1: not redrawn
            int8_t type = 8;  // the substance type (byte 0)
            bool late = false;  // list 2
        };
        // Per model, per slot 0 submesh, per group.
        std::vector<std::vector<std::vector<Group>>> groupOf;
        // Per model, its shadow volume's slot (mask 0x20), -1: none.
        std::vector<int> shadowSlot;
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
        const char* diffuse = nullptr;  // 0x5e1080
        const char* normalMap = nullptr;
        const char* envSpec = nullptr;  // g_envSpecColourAndLerpFactor (0x5e1080)
        const char* extrusion = nullptr;  // g_fExtrusion (0x5def70)
    };

    // The device states of one combination of presets: every render state
    // the defaults or a preset touch, sampler states for stages 0..3.
    struct StateSet
    {
        int base = 0;          // preset of the substance type
        unsigned extras = 0;   // bit n: preset 0x2a + n
        int mode = 2;          // the request's mode (its preset: 0x5cdaa0)
        std::vector<std::pair<DWORD, DWORD>> rs, sampler;
        bool blend = false;    // ALPHABLENDENABLE
    };
    int StateFor(const uint8_t s[8], int mode = 2);
    void ApplyState(const StateSet& set);
    void AddPackage(const MdpcPackage& data);
    void ResolveGroups(Package& p, const std::map<uint32_t, int>& byUid);
    void DrawGroup(const Package& p, const MdpcModel& m, const MdpcGroup& g, const Package::Group& info, int state,
                   const float world[16], int& applied);
    void SetLight(const float ambient[4], const float colour[4], const float direction[4]);
    void SetEnvSpec(const float colour[4]);
    void DrawSky(const float eye[3]);

    HMODULE dll_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    ID3DXEffectPool* pool_ = nullptr;
    ID3DXEffect* shared_ = nullptr;
    const char* viewProj_ = nullptr;
    const char* camPos_ = nullptr;
    const char* ambient_ = nullptr;      // s_LightAmbientColour
    const char* lightColour_ = nullptr;  // s_LightDirectionalColour
    const char* lightDir_ = nullptr;     // s_LightDirectionalDirection
    const char* fogParams_ = nullptr;    // s_FogParams
    Mood mood_;
    bool shadows_ = true;
    int shadowState_[2] = { -1, -1 };  // states_ of modes 4 and 5
    int skyPackage_ = -1;  // index into packages_
    Effect fx_[0x30];
    IDirect3DVertexDeclaration9* decl_ = nullptr;
    IDirect3DTexture9* white_ = nullptr;
    std::vector<Package> packages_;
    std::vector<Instance> instances_;
    std::vector<StateSet> states_;
    // What the device has (valid: set by ApplyState since the last reset).
    DWORD rsCache_[256] = {};
    bool rsValid_[256] = {};
    DWORD samplerCache_[4][16] = {};
    bool samplerValid_[4][16] = {};
    std::string error_;
};
