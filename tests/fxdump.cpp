// Lists the compiled effects inside Driv3r.exe (the table of 0x5d3140):
// for each, the techniques, passes and parameters, loaded the way the game
// loads them (one effect pool, the shared effect first; D3DXCreateEffect
// with flags 0x100). Uses d3dx9_43.dll next to the program.
// Build: tests\build_fxdump.bat. Usage: fxdump <Driv3r.exe> [id]
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct FxEntry { uint32_t va, size; int id; };
// 0x5d3140: the shared effect (0x5d30b0 / 0x5dc0e0) and then one per id
// at [manager + 0x2c + id * 4].
static const FxEntry kFx[] = {
    { 0x71d038, 0x830, -1 },
    { 0x71d868, 0x20dc, 0x01 }, { 0x71f948, 0x244c, 0x02 }, { 0x721d98, 0x2464, 0x03 }, { 0x724200, 0x287c, 0x05 },
    { 0x726a80, 0x28a8, 0x06 }, { 0x729328, 0x3170, 0x09 }, { 0x72c498, 0x2a84, 0x07 }, { 0x72ef20, 0x1920, 0x04 },
    { 0x730840, 0x1900, 0x10 }, { 0x732140, 0x285c, 0x11 }, { 0x739038, 0x1840, 0x0a }, { 0x73a878, 0x19c4, 0x0b },
    { 0x73c240, 0x1840, 0x16 }, { 0x73da80, 0x1860, 0x0c }, { 0x73f2e0, 0x19d4, 0x0d }, { 0x740cb8, 0x1840, 0x15 },
    { 0x76d548, 0x2a84, 0x17 }, { 0x76ffd0, 0x2a84, 0x18 }, { 0x778188, 0x2ad8, 0x19 }, { 0x77ac60, 0x2ad8, 0x1a },
    { 0x762c30, 0x19d4, 0x0e }, { 0x764608, 0x18d0, 0x08 }, { 0x76a0a8, 0x16dc, 0x2b }, { 0x7424f8, 0x26ac, 0x1e },
    { 0x744ba8, 0x3a28, 0x1f }, { 0x7485d0, 0x3a20, 0x20 }, { 0x74bff0, 0x2c34, 0x21 }, { 0x74ec28, 0x2d44, 0x1d },
    { 0x751970, 0x2200, 0x22 }, { 0x753b70, 0x36f0, 0x23 }, { 0x757260, 0x3860, 0x24 }, { 0x75aac0, 0x388c, 0x25 },
    { 0x75e350, 0x2200, 0x26 }, { 0x765ed8, 0x227c, 0x27 }, { 0x768158, 0x1f4c, 0x28 }, { 0x7349a0, 0x18e4, 0x12 },
    { 0x736288, 0x16d4, 0x13 }, { 0x737960, 0x16d4, 0x14 }, { 0x772a58, 0x2b94, 0x1b }, { 0x7755f0, 0x2b94, 0x1c },
    { 0x761e50, 0x0ddc, 0x2c }, { 0x76b788, 0x0c00, 0x2d }, { 0x760550, 0x18fc, 0x2e }, { 0x76c388, 0x11bc, 0x2f },
    { 0x77d738, 0x1368, 0x29 }, { 0x77eaa0, 0x1378, 0x2a },
};

typedef HRESULT(WINAPI* CreateEffectFn)(LPDIRECT3DDEVICE9, LPCVOID, UINT, const D3DXMACRO*, LPD3DXINCLUDE, DWORD,
                                        LPD3DXEFFECTPOOL, LPD3DXEFFECT*, LPD3DXBUFFER*);
typedef HRESULT(WINAPI* CreatePoolFn)(LPD3DXEFFECTPOOL*);
typedef HRESULT(WINAPI* DisassembleFn)(LPD3DXEFFECT, BOOL, LPD3DXBUFFER*);

static const char* TypeName(D3DXPARAMETER_TYPE t)
{
    switch (t) {
    case D3DXPT_FLOAT: return "float";
    case D3DXPT_BOOL: return "bool";
    case D3DXPT_INT: return "int";
    case D3DXPT_TEXTURE: case D3DXPT_TEXTURE2D: case D3DXPT_TEXTURECUBE: return "texture";
    case D3DXPT_SAMPLER: case D3DXPT_SAMPLER2D: case D3DXPT_SAMPLERCUBE: return "sampler";
    case D3DXPT_VERTEXSHADER: return "vs";
    case D3DXPT_PIXELSHADER: return "ps";
    case D3DXPT_STRING: return "string";
    default: return "?";
    }
}

static void Dump(ID3DXEffect* fx)
{
    D3DXEFFECT_DESC d;
    fx->GetDesc(&d);
    for (UINT t = 0; t < d.Techniques; ++t) {
        D3DXHANDLE th = fx->GetTechnique(t);
        D3DXTECHNIQUE_DESC td;
        fx->GetTechniqueDesc(th, &td);
        printf("  technique %u %s (%u passes)%s\n", t, td.Name, td.Passes, SUCCEEDED(fx->ValidateTechnique(th)) ? "" : " [not valid here]");
    }
    for (UINT p = 0; p < d.Parameters; ++p) {
        D3DXHANDLE ph = fx->GetParameter(nullptr, p);
        D3DXPARAMETER_DESC pd;
        fx->GetParameterDesc(ph, &pd);
        printf("  %-7s %ux%u%s %s%s%s%s", TypeName(pd.Type), pd.Rows, pd.Columns, pd.Elements ? "[]" : "", pd.Name,
               pd.Semantic ? " : " : "", pd.Semantic ? pd.Semantic : "", (pd.Flags & D3DX_PARAMETER_SHARED) ? " shared" : "");
        if (pd.Type == D3DXPT_FLOAT && pd.Bytes <= 64 && !pd.Elements) {
            float v[16] = {};
            fx->GetFloatArray(ph, v, pd.Rows * pd.Columns);
            printf(" =");
            for (UINT k = 0; k < pd.Rows * pd.Columns; ++k) printf(" %g", v[k]);
        }
        printf("\n");
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: fxdump <Driv3r.exe> [id]\n"); return 2; }
    int only = argc > 2 ? int(strtol(argv[2], nullptr, 0)) : -2;
    bool disasm = argc > 3 && strcmp(argv[3], "asm") == 0;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> exe;
    fseek(f, 0, SEEK_END);
    exe.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    fread(exe.data(), 1, exe.size(), f);
    fclose(f);
    // The data sections of this exe are mapped at raw offset + 0x400000.
    HMODULE dx = LoadLibraryA("d3dx9_43.dll");
    auto createEffect = dx ? reinterpret_cast<CreateEffectFn>(GetProcAddress(dx, "D3DXCreateEffect")) : nullptr;
    auto createPool = dx ? reinterpret_cast<CreatePoolFn>(GetProcAddress(dx, "D3DXCreateEffectPool")) : nullptr;
    auto disassemble = dx ? reinterpret_cast<DisassembleFn>(GetProcAddress(dx, "D3DXDisassembleEffect")) : nullptr;
    if (!createEffect || !createPool) { printf("d3dx9_43.dll not found\n"); return 1; }
    HWND wnd = CreateWindowA("STATIC", "fxdump", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
        printf("no device\n");
        return 1;
    }
    ID3DXEffectPool* pool = nullptr;
    createPool(&pool);
    int failed = 0;
    for (const FxEntry& e : kFx) {
        uint32_t off = e.va - 0x400000;
        if (off + e.size > exe.size()) { printf("0x%x out of the file\n", e.va); ++failed; continue; }
        ID3DXEffect* fx = nullptr;
        ID3DXBuffer* err = nullptr;
        HRESULT hr = createEffect(dev, &exe[off], e.size, nullptr, nullptr, 0x100, pool, &fx, &err);
        bool show = only == -2 || only == e.id;
        if (show || FAILED(hr)) printf("effect %d at 0x%x (%u bytes): %s\n", e.id, e.va, e.size, SUCCEEDED(hr) ? "ok" : "FAILED");
        if (FAILED(hr)) {
            ++failed;
            if (err) printf("  %s\n", static_cast<const char*>(err->GetBufferPointer()));
        } else if (show && disasm && disassemble) {
            ID3DXBuffer* text = nullptr;
            if (SUCCEEDED(disassemble(fx, FALSE, &text))) {
                fwrite(text->GetBufferPointer(), 1, text->GetBufferSize() - 1, stdout);
                text->Release();
            }
        } else if (show) {
            Dump(fx);
        }
        if (err) err->Release();
        // Kept alive (as in the game) so the pool keeps the shared values.
    }
    printf("%d failed\n", failed);
    return failed ? 1 : 0;
}
