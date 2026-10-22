// The original game's code as a test oracle: Driv3r.exe's sections mapped
// and relocated in this 32-bit process (the system holds 0x400000), so its
// functions can be called directly with chosen registers and stack
// arguments, under the FPU control word the game runs with (0x007f, single
// precision, under Direct3D 9). Only for pure functions: imports are not
// resolved and nothing of the game is initialised beyond its file data.
// Traced heap records go back at their own addresses (MapAt), so a test is
// linked /BASE:0x10000000 /FIXED /DYNAMICBASE:NO to keep its own image out of
// the way.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct GameRegs
{
    uint32_t eax = 0, ecx = 0, edx = 0, ebx = 0, esi = 0, edi = 0, ebp = 0;
};

namespace GameCode
{
inline GameRegs g_in, g_out;
inline uint32_t g_target, g_args[16], g_nargs, g_savedEsp, g_fpuGame = 0x007f, g_fpuSaved;
inline uint32_t g_faultAddr, g_faultData;
inline uint32_t g_base = 0x400000, g_image = 0x400000;

// A game address where the image is mapped.
inline uint32_t At(uint32_t gameAddr) { return gameAddr - g_base + g_image; }
// A game address back from a mapped one (for fault reports).
inline uint32_t Game(uint32_t mapped) { return mapped - g_image + g_base; }
inline LONG CALLBACK FirstChance(EXCEPTION_POINTERS* e);

// Maps Driv3r.exe (anywhere, relocated: At() turns game addresses into
// mapped ones). Returns false when the file cannot be read.
inline bool Load(const char* exePath)
{
    FILE* f = fopen(exePath, "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END);
    d.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    const size_t got = fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (got != d.size()) return false;
    const uint32_t pe = *reinterpret_cast<uint32_t*>(&d[0x3c]);
    const uint32_t base = *reinterpret_cast<uint32_t*>(&d[pe + 24 + 28]);
    const uint32_t size = *reinterpret_cast<uint32_t*>(&d[pe + 24 + 56]);
    uint8_t* image = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!image) return false;
    AddVectoredExceptionHandler(1, FirstChance);
    g_base = base;
    g_image = reinterpret_cast<uint32_t>(image);
    const int n = *reinterpret_cast<uint16_t*>(&d[pe + 6]);
    const int optSize = *reinterpret_cast<uint16_t*>(&d[pe + 20]);
    for (int i = 0; i < n; ++i) {
        const uint8_t* s = &d[pe + 24 + optSize + 40 * i];
        const uint32_t va = *reinterpret_cast<const uint32_t*>(s + 12);
        const uint32_t raw = *reinterpret_cast<const uint32_t*>(s + 16);
        const uint32_t at = *reinterpret_cast<const uint32_t*>(s + 20);
        if (raw && at + raw <= d.size()) memcpy(image + va, &d[at], raw);
    }
    // the imports resolved (the CRT's malloc reaches HeapAlloc, the static
    // D3DX math picks its CPU path after reading the registry); DLLs from
    // the game's folder when the system has none
    std::string dir(exePath);
    dir.resize(dir.find_last_of("\\/") + 1);
    const uint32_t importRva = *reinterpret_cast<uint32_t*>(&d[pe + 24 + 96 + 1 * 8]);
    for (uint32_t at = importRva;; at += 20) {
        const uint32_t* desc = reinterpret_cast<uint32_t*>(image + at);
        if (!desc[3]) break;
        const char* dll = reinterpret_cast<const char*>(image + desc[3]);
        HMODULE lib = LoadLibraryA(dll);
        if (!lib) lib = LoadLibraryA((dir + dll).c_str());
        if (!lib) { fprintf(stderr, "gamecode: %s not loaded, its imports stay unresolved\n", dll); continue; }
        uint32_t* names = reinterpret_cast<uint32_t*>(image + (desc[0] ? desc[0] : desc[4]));
        uint32_t* slots = reinterpret_cast<uint32_t*>(image + desc[4]);
        for (int k = 0; names[k]; ++k) {
            const char* fn = (names[k] & 0x80000000u) ? MAKEINTRESOURCEA(names[k] & 0xffff)
                                                      : reinterpret_cast<const char*>(image + names[k] + 2);
            slots[k] = reinterpret_cast<uint32_t>(GetProcAddress(lib, fn));
        }
    }
    // the CRT heap handle (0x8e483c, set by the game's start-up)
    *reinterpret_cast<HANDLE*>(image + 0x8e483c - base) = GetProcessHeap();
    // the base relocations (type 3, 32-bit) for where the image landed
    const uint32_t relocRva = *reinterpret_cast<uint32_t*>(&d[pe + 24 + 96 + 5 * 8]);
    const uint32_t relocSize = *reinterpret_cast<uint32_t*>(&d[pe + 24 + 96 + 5 * 8 + 4]);
    const uint32_t delta = g_image - base;
    for (uint32_t at = 0; at + 8 <= relocSize;) {
        const uint32_t page = *reinterpret_cast<uint32_t*>(image + relocRva + at);
        const uint32_t block = *reinterpret_cast<uint32_t*>(image + relocRva + at + 4);
        if (block < 8) break;
        for (uint32_t k = 8; k + 2 <= block; k += 2) {
            const uint16_t e = *reinterpret_cast<uint16_t*>(image + relocRva + at + k);
            if ((e >> 12) == 3) *reinterpret_cast<uint32_t*>(image + page + (e & 0xfff)) += delta;
        }
        at += block;
    }
    return true;
}

// Commits read-write memory over [addr, addr + bytes) at those addresses
// (for traced heap records whose pointers must stay valid).
inline bool MapAt(uint32_t addr, uint32_t bytes)
{
    for (uint32_t a = addr & ~0xffffu; a < addr + bytes; a += 0x10000) {
        MEMORY_BASIC_INFORMATION mi;
        VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), &mi, sizeof(mi));
        if (mi.State == MEM_FREE &&
            !VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), 0x10000, MEM_RESERVE | MEM_COMMIT,
                          PAGE_READWRITE))
            return false;
        if (mi.State == MEM_RESERVE &&
            !VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), 0x10000, MEM_COMMIT, PAGE_READWRITE))
            return false;
    }
    return true;
}

static __declspec(naked) void Thunk()
{
    __asm {
        pushad
        mov g_savedEsp, esp
        fnstcw word ptr g_fpuSaved
        fldcw word ptr g_fpuGame
        mov ecx, g_nargs
    push_args:
        test ecx, ecx
        jz pushed
        dec ecx
        push dword ptr [g_args + ecx * 4]
        jmp push_args
    pushed:
        mov eax, g_in.eax
        mov ecx, g_in.ecx
        mov edx, g_in.edx
        mov ebx, g_in.ebx
        mov esi, g_in.esi
        mov edi, g_in.edi
        mov ebp, g_in.ebp
        call dword ptr [g_target]
        mov g_out.eax, eax
        mov g_out.ecx, ecx
        mov g_out.edx, edx
        mov g_out.ebx, ebx
        mov g_out.esi, esi
        mov g_out.edi, edi
        mov g_out.ebp, ebp
        mov esp, g_savedEsp
        fldcw word ptr g_fpuSaved
        popad
        ret
    }
}

// Reports the first fault before any frame handler runs (the game's own
// handlers, if a function installs one, point into code that cannot run).
inline LONG CALLBACK FirstChance(EXCEPTION_POINTERS* e)
{
    const uint32_t at = reinterpret_cast<uint32_t>(e->ExceptionRecord->ExceptionAddress);
    fprintf(stderr, "fault %08lx at %08x (game %08x), data %08x, eax %08lx ecx %08lx edx %08lx esi %08lx edi %08lx\n",
            e->ExceptionRecord->ExceptionCode, at, Game(at),
            e->ExceptionRecord->NumberParameters > 1
                ? static_cast<uint32_t>(e->ExceptionRecord->ExceptionInformation[1])
                : 0,
            e->ContextRecord->Eax, e->ContextRecord->Ecx, e->ContextRecord->Edx, e->ContextRecord->Esi,
            e->ContextRecord->Edi);
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(e->ContextRecord->Esp);
    fprintf(stderr, "  [esp] %08x (game %08x)\n", sp[0], Game(sp[0]));
    return EXCEPTION_CONTINUE_SEARCH;
}

inline int Filter(EXCEPTION_POINTERS* e)
{
    g_faultAddr = reinterpret_cast<uint32_t>(e->ExceptionRecord->ExceptionAddress);
    g_faultData = e->ExceptionRecord->NumberParameters > 1
                      ? static_cast<uint32_t>(e->ExceptionRecord->ExceptionInformation[1])
                      : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Calls the game function at addr with the registers and stack arguments
// (pushed right to left, so args[0] is [esp + 4] on entry). Returns false on
// a fault (g_faultAddr / g_faultData say where).
inline bool Call(uint32_t addr, const GameRegs& r, std::initializer_list<uint32_t> args = {}, GameRegs* out = nullptr)
{
    g_in = r;
    g_target = At(addr);
    g_nargs = 0;
    for (uint32_t a : args) g_args[g_nargs++] = a;
    bool ok = true;
    __try {
        Thunk();
    } __except (Filter(GetExceptionInformation())) {
        ok = false;
        uint16_t cw = 0x027f;
        __asm fninit
        __asm fldcw cw
    }
    if (out) *out = g_out;
    return ok;
}

inline uint32_t Bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline float Float(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
}  // namespace GameCode
