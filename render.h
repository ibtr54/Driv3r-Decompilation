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
// Draw distances (0x4e7e30): each cell's quadtree (SCND) is walked with
// the node distances of the profile's setting (0x4ddc50), a 2D frustum and
// the shadow-vector retest; the distance ratio picks the detail slots
// (0x4dff20, 0x5dcc50) and an adaptive scale keeps about 150 objects. All
// cells are walked (the game streams 11 around the player). The camera is
// the ride's start camera or the free one (main.cpp); the game's chase
// cameras need the vehicle.
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
#include "collision.h"
#include "mdpc.h"
#include "mood.h"
#include "skid.h"
#include "smoke.h"
#include "treeshadow.h"

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

// The player's vehicle (vehicle.h, BOOT_TO_MENU.md section 58): its package
// (Vehicles\<City>.vvs; its materials name it as package 0xfffd), the
// shared vehicle materials (CarGlobals<City>.vgt, package uid 0x1d), the
// models drawn with their World (row vector, applied to the vertices as
// stored), and the object's colour (+0x50, g_colourWeightsVec of 0x5df200)
// and each part's damage (+0x70, g_DamageFactor of 0x5defb0: the dent).
struct VehicleDraw
{
    const MdpcPackage* package = nullptr;
    const MdpcPackage* globals = nullptr;
    struct Part
    {
        int model = 0;
        float world[16] = {};
        bool rim = false;  // a burst tyre's rim (hidden unless the tyre bursts)
        // The dent (object +0x70, 0x4fd7f0 / 0x4fe36b): the car shader moves
        // each vertex by its damaged offset (position 1 / normal 1) times
        // this (VehicleDent).
        float damage = 0;
    };
    std::vector<Part> parts;
    float colour[3] = {};
    // The body's World and the exhaust tips in the vehicle's space (x, y, z
    // per tip; VehicleData::exhausts): the exhaust smoke (smoke.h).
    float body[16] = {};
    std::vector<float> exhausts;
};

class CityRenderer
{
public:
    ~CityRenderer() { Release(); }
    // Loads the effects out of the game's executable.
    bool Init(IDirect3DDevice9* device, const std::string& exePath);
    // The city's packages (packages[first, end)), its placement, the sky
    // dome's package (nullptr: no sky) and sfx.pmu's (uid 0x1f: the tree
    // shadow materials; nullptr: no tree shadows).
    // vehicle: the player's vehicle (nullptr: none).
    bool Upload(const std::vector<MdpcPackage>& packages, size_t first, size_t end, const CityPlacement& city,
                const MdpcPackage* sky = nullptr, const MdpcPackage* sfx = nullptr, const VehicleDraw* vehicle = nullptr);
    // The player's vehicle moved: the parts' World (VehicleDraw::parts order,
    // 16 floats each), from the next Draw; hidden (optional, one per part):
    // nonzero leaves the part out (a burst tyre's wheel, an unused rim, a
    // missing wheel).
    void SetVehiclePose(const std::vector<float>& worlds, const std::vector<uint8_t>* hidden = nullptr);
    // The parts' dents (VehicleDraw::parts order, one each), from the next
    // Draw.
    void SetVehicleDamage(const std::vector<float>& damage);
    // One 120 Hz world update of the car's smoke: the car's effects block's
    // decision when it ran this tick (CarSim::Effects, else null), then the
    // emitters' clock (half a tick). Once called, the smoke no longer
    // follows the draw time.
    void StepSmoke(const CarSmokeState* state)
    {
        if (state) smoke_.Apply(*state);
        smoke_.Advance(1.0f / 120.0f);
        smokeStepped_ = true;
    }
    // The physics world for the tree shadows' ray (0x5a03f0); null: trees
    // without GRTD ground get no shadow.
    void SetCollision(const CollisionWorld* world)
    {
        collision_ = world;
        trees_.Reset();
    }
    // A city instance no longer drawn (a broken prop, props.h).
    void HideCityInstance(int cityInstance)
    {
        if (cityInstance >= 0 && size_t(cityInstance) < instanceOf_.size()) instanceOf_[size_t(cityInstance)] = -1;
    }
    // The level's light, fog and sky matrix (used from the next Draw).
    void SetMood(const Mood& mood) { mood_ = mood; }
    // The profile's Shadows setting (+0xc8c).
    void SetShadows(bool on) { shadows_ = on; }
    // The profile's draw distance (+0xc88, 0..2) and whether the city is a
    // night one (game setup vtbl+0x60 == 2): the cells' distance and detail
    // tables (0x4ddc50, 0x4dde20).
    void SetDrawDistance(int setting, bool night);
    // The ride's time of day (game setup +0x34, vtbl+0x64): 3 (night) picks
    // the sky's material 0 as the environment cube and no shine (0x57c092).
    void SetTimeOfDay(int time) { timeOfDay_ = time; }
    // The game time in seconds ([0x8d6d90]; in the trace it follows the
    // update counter [[0x8ac434] +4] / 120 from the level start): animated
    // materials and the shore waves. Here: seconds since the first frame.
    void SetTime(float seconds) { time_ = seconds; }
    // viewProj: row-vector view x projection (Direct3D); eye: the camera;
    // back: the camera's z axis (it looks along -back); fov / aspect: the
    // camera's +0x180 / +0x184. The sky first, then the city, walked like
    // 0x4e7e30: each cell's quadtree (SCND) with the node distances, a 2D
    // frustum and the detail levels of the draw-distance tables.
    void Draw(const float viewProj[16], const float eye[3], const float back[3], float fov, float aspect);
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
    // Last frame: objects submitted with only their shadow (mask 0x20),
    // objects per detail slot 0..4, the detail scale [0x7a1a68] (0.8..1).
    size_t frameShadowOnly = 0, frameSlots[5] = {};
    float lodScale = 1.0f;
    // Cells with a quadtree, instances not reached by any (logged once).
    size_t cells = 0, unindexed = 0;
    // The sea's groups (all tiles) and last frame's submitted tiles.
    size_t waterGroups = 0, frameWater = 0;
    // Shore lines with at least two points, and last frame's drawn lines.
    size_t shoreLines = 0, frameShore = 0;
    std::string shoreInfo;  // the shore material's effect / preset ("": none)
    // The far quad's material / effect / preset ("": none), last frame drawn.
    std::string farInfo;
    bool frameFar = false;
    // The tree shadows' materials / effects / presets ("": none); the last
    // rebuild ("": none yet, cleared when logged) and last frame's triangles.
    std::string treeInfo, treeBuild;
    int frameTreeTriangles = 0;
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
    // The vehicle's drawn groups per effect / base preset ("": none).
    std::string vehicleInfo;
    // The exhaust smoke's material / effect / preset ("": none) and the
    // particles spawned so far.
    std::string smokeInfo;
    int SmokeSpawned() const { return smoke_.Spawned(); }
    // The rear wheels' skid-mark ribbons (wheels 1 and 3; skid.h): the car's
    // effects block takes and switches them (ApplySkid), the frame adds the
    // points (SkidRibbon::Add), the draw shows every ribbon in use.
    // skidInfo: the material / effect / preset ("": none).
    void ApplySkid(const CarSmokeState& s);
    SkidRibbon* Skid(int k) { return skidTaken_[k] ? &skids_[k] : nullptr; }
    const float* MoodParticle() const { return mood_.particle; }  // +0x210, the ribbons' colour
    std::string skidInfo;

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
            bool normalBound = false;  // the binder sets the normal map (substance +6 is 1 or 4)
            int anim = -1;  // anims_ (a material with several substances), -1: one substance
            // A vehicle's group (draw kind 3 / 0x10003): the textures of
            // 0x5e0b20 (nullptr: the white one, standing in for the global
            // texture 0xf00d0004) and the substance's colour mode (+0x15) and
            // texture count (+0x14) for g_colourWeightsVec (0x5df200).
            bool vehicle = false;
            IDirect3DTexture9* clean = nullptr;
            IDirect3DTexture9* damage = nullptr;
            IDirect3DTexture9* mask = nullptr;
            uint8_t colourMode = 0, textureCount = 0;
        };
        // Per model, per detail slot 0..4, per submesh, per group.
        std::vector<std::vector<std::vector<std::vector<Group>>>> groupOf;
        // Per model, its shadow volume's slot (mask 0x20), -1: none.
        std::vector<int> shadowSlot;
    };
    struct Instance
    {
        const CityInstance* in = nullptr;
        int package = -1;
        uint32_t model = 0;
        float centre[3] = {};
        // Submitted object +0x50 -> render object +8 (0x5ce700): the index
        // of the substance its groups draw with (0x5d92b0; 0 for the city,
        // SEA_COLOUR - 1 for the sea's tiles).
        uint32_t substance = 0;
        // A vehicle's part: its World (no city instance; in is nullptr) and
        // its dent (object +0x70).
        const float* world = nullptr;
        const float* damage = nullptr;
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
        // Class 0x5d3da0 (effects 7 and 9, handles 0x5deaa0): two frames of
        // an animated material blended by g_TextureLerp (0x5de890).
        const char* lerp = nullptr;
        const char* diffuse2[2] = {};  // g_TextureDiffuse1 / 2
        const char* normal2[2] = {};   // g_TextureNormalMap1 / 2
        // The vehicle parameters (0x5e1080 textures, 0x5df370 constants).
        const char* clean = nullptr;      // g_TextureClean
        const char* damageTex = nullptr;  // g_TextureDamage
        const char* mask = nullptr;       // g_TextureColourednessMask
        const char* colourWeights = nullptr;  // g_colourWeightsVec
        const char* damageFactor = nullptr;   // g_DamageFactor
    };
    // A material with several substances: per substance the first and
    // second texture (nullptr: none). Animated (entry +8 bit 0): the
    // substances are frames shown at speed per second; else the drawn
    // object's substance index picks one (0x5d92b0). The substances' other
    // bytes are taken from the first one (in the water materials they are
    // the same).
    struct Anim
    {
        bool animated = false;
        float speed = 0;
        std::vector<IDirect3DTexture9*> tex0, tex1;
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
    // self: the package that material uid 0xfffd names (a vehicle's own);
    // vehicle: the texture binder of draw kind 3 / 0x10003 (0x5e0b20).
    Package::Group GroupFor(uint32_t material, int id, bool modelSubstance, bool layout,
                            const std::map<uint32_t, int>& byUid, int self = -1, bool vehicle = false);
    void ResolveGroups(Package& p, const std::map<uint32_t, int>& byUid, int self = -1, bool vehicle = false);
    // The shore waves (0x454cf0, 0x455840, 0x454820): pick the lines, rebuild
    // the strips that are due, and draw them with the given states.
    void UpdateShore(const float eye[3]);
    void BuildShoreStrip(const ShoreLine& line, char* out, float a, float b, float alpha) const;
    void DrawShore(bool redraw, int& applied);
    // The far quad (0x451030 part 1): a wall 4000 units ahead of the camera.
    void DrawFar(const float eye[3], const float back[3], int& applied);
    // The tree shadows (treeshadow.h): rebuilt around the camera's x / z
    // when it has moved far enough, drawn
    // in the mode 7 pass before the redraw. Ground: the city's GRTD heights
    // (CityGround, 0x4e9da0), else the physics ray (CollisionWorld::Cast).
    void UpdateTreeShadows(const float eye[3]);
    void DrawTreeShadows(int& applied);
    // The two screen quads before the tree shadows (type 2: alpha 0
    // everywhere; type 3: alpha 1 where the volumes left stencil >= 1).
    void DrawShadowAlpha(int& applied);
    // The exhaust emitters' rings (smoke.h), effect 0x2c, after the shadow
    // pass. fov / aspect: s_CameraToScreen.
    void DrawSmoke(float fov, float aspect, int& applied);
    void DrawSkid(int& applied);
    void DrawGroup(const Package& p, const MdpcModel& m, const MdpcGroup& g, const Package::Group& info, int state,
                   const float world[16], int& applied, float alpha = 1.0f, uint32_t substance = 0);
    void BindGroup(const Package::Group& info, int state, const float world[16], int& applied, float alpha,
                   uint32_t substance);
    // A cell's distance / detail table (the cell slot's +0x40..+0xc4 of
    // [0x8ac44c]): distances per quadtree level, then six (mask, ratio
    // limit, fade width) entries.
    struct LodTable
    {
        float dist[16] = {};
        uint32_t mask[6] = {};
        float limit[6] = {}, width[6] = {};
    };
    // A node's or object's detail: object +0x62 mask, +0x64 alpha, and the
    // push away from the camera (0x4dff20's result +0xc).
    struct Lod
    {
        uint32_t mask = 0;
        float alpha = 1.0f, offset = 0.0f;
    };
    struct Submit
    {
        int instance;
        Lod lod;
    };
    struct View;
    void PickLod(const LodTable& t, int mode, float ratio, Lod& lod) const;
    float NodeRatio(const View& v, const LodTable& t, int mode, const float pos[3], float radius, float dist) const;
    bool NodeVisible(const View& v, const CityCell& cell, int mode, const CityNode& n, float size, Lod& lod) const;
    void WalkNode(const View& v, const CityCell& cell, int mode, const CityNode& n, std::vector<Submit>& out) const;
    void SetLight(const float ambient[4], const float colour[4], const float direction[4]);
    // redraw: the shadow redraw's modes 6 / 7 (0x5e0f20 reads the mode).
    void SetEnvSpec(const float colour[4], bool redraw = false);
    void DrawSky(const float eye[3]);
    IDirect3DCubeTexture9* CubeFrom(const Package& p, size_t material);

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
    const char* envTex_ = nullptr;       // s_TextureEnvironment (shared +0x38)
    const char* worldToEnv_ = nullptr;   // s_WorldToEnvMap (shared +0x2c)
    const char* shineTex_ = nullptr;     // s_TextureShine (shared +0x3c)
    IDirect3DCubeTexture9* cubes_[2] = {};  // [0x8afab8]: 0 shine, 1 environment
    int timeOfDay_ = 1;
    float time_ = 0;
    std::vector<Anim> anims_;
    std::map<std::pair<int, uint32_t>, int> animOf_;  // (package, material) -> anims_
    Mood mood_;
    bool shadows_ = true;
    int shadowState_[2] = { -1, -1 };  // states_ of modes 4 and 5
    int alphaState_[2] = { -1, -1 };   // states_ of the screen quads (types 2 and 3)
    int skyPackage_ = -1;  // index into packages_
    Effect fx_[0x30];
    IDirect3DVertexDeclaration9* decl_ = nullptr;
    // Declaration index 5 (0x5d099b, 60 bytes: the vehicles) and the one set.
    IDirect3DVertexDeclaration9* decl5_ = nullptr;
    IDirect3DVertexDeclaration9* decl3_ = nullptr;  // declaration index 3 (the screen quads)
    // The particles' declaration (80 bytes, the traced one; SmokeVertex).
    IDirect3DVertexDeclaration9* smokeDecl_ = nullptr;
    IDirect3DVertexDeclaration9* currentDecl_ = nullptr;
    void SetDecl(IDirect3DVertexDeclaration9* decl)
    {
        if (decl == currentDecl_) return;
        device_->SetVertexDeclaration(decl);
        currentDecl_ = decl;
    }
    // The vehicle: its parts (indices into instances_, submitted every frame
    // with mask 0x22: slot 0 and the shadow volume), Worlds, colour, dents.
    std::vector<int> vehicleParts_;
    std::vector<std::vector<float>> vehicleWorlds_;
    std::vector<size_t> vehicleWorldPart_;  // the VehicleDraw part of each vehicleWorlds_ entry
    std::vector<uint8_t> vehicleHidden_;    // per vehicleParts_ entry (SetVehiclePose)
    std::vector<float> vehicleDamage_;      // per vehicleWorlds_ entry (SetVehicleDamage)
    float vehicleColour_[3] = {};
    float drawDamage_ = 0;  // the drawn object's +0x70 (BindGroup)
    IDirect3DTexture9* white_ = nullptr;
    std::vector<Package> packages_;
    std::vector<Instance> instances_;
    const CityPlacement* city_ = nullptr;
    std::vector<int> instanceOf_;  // per city instance: index into instances_, -1: not drawn
    std::vector<int> waterTiles_;  // the sea's tiles: indices into instances_
    // Shore waves: the material's group (GWTR reference 0, render id 1), a
    // managed vertex buffer of 2 x slots strips of maxVerts vertices
    // (48 bytes: position, normal, uv, colour as the tangent), the slot
    // cache [+0x738] {line, distance, last build} and this frame's count.
    Package::Group shoreGroup_;
    IDirect3DVertexBuffer9* shoreVb_ = nullptr;
    size_t shoreSlots_ = 0, shoreMaxVerts_ = 0, shoreDrawn_ = 0;
    struct ShoreSlot { int line = -1; float dist = 0, last = 0; bool valid = false; };
    std::vector<ShoreSlot> shoreCache_;
    std::vector<uint32_t> shoreVerts_;  // per strip (2 x slots): vertices written
    // The far quad: GWTR reference 6 with render id 1 (0x450d90: a buffer
    // object of the same manager, 1 strip of 4 vertices), refilled per frame.
    Package::Group farGroup_;
    IDirect3DVertexBuffer9* farVb_ = nullptr;
    // Tree shadows: sfx.pmu materials 0x16 (buffers 0 and 1) and 0x15
    // (buffer 2) with render id 1, a managed vertex buffer per buffer
    // object holding its triangle list.
    TreeShadows trees_;
    const CollisionWorld* collision_ = nullptr;
    Package::Group treeGroup_[3];
    IDirect3DVertexBuffer9* treeVb_[3] = {};
    // Exhaust smoke: sfx.pmu material 0x1f0000 with render id 0x2c (the
    // particle manager's draw, 0x56e500 -> 0x5cfd00), the emitters, a
    // managed buffer of every ring refilled per frame, effect 0x2c's
    // s_CameraToScreen / g_TimeAndConstants / g_Compression.
    ExhaustSmoke smoke_;
    Package::Group smokeGroup_;
    Package::Group debrisGroup_;  // sfx 0x1f0003: the spray's debris
    IDirect3DVertexBuffer9* smokeVb_ = nullptr;
    const char* cameraToScreen_ = nullptr;
    const char* timeAndConstants_ = nullptr;
    const char* compression_ = nullptr;
    float smokeTime_ = -1;  // time_ of the last Advance
    bool smokeStepped_ = false;  // StepSmoke drives the smoke (the car's physics)
    // Skid marks: sfx.pmu material 0x1f000d (the ribbons' renderer request,
    // 0x55c6fa), two of the world's four ribbons (the player's rear wheels),
    // one dynamic buffer of 1600 vertices each (the game's 4 x 1600 x 48).
    std::vector<SkidRibbon> skids_ = std::vector<SkidRibbon>(2);  // 77 KB each
    bool skidTaken_[2] = {};
    Package::Group skidGroup_;
    IDirect3DVertexBuffer9* skidVb_ = nullptr;
    LodTable tables_[2];           // mode 1 (0x4ddc50) and mode 3 (0x4dde20)
    std::vector<StateSet> states_;
    // What the device has (valid: set by ApplyState since the last reset).
    DWORD rsCache_[256] = {};
    bool rsValid_[256] = {};
    DWORD samplerCache_[4][16] = {};
    bool samplerValid_[4][16] = {};
    std::string error_;
};
