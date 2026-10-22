// Which D3DX math path the game's start-up picks on this machine (0x60b89d):
// the path id [0x796428] and the routines behind the matrix product
// [0x7961dc] and inverse [0x7961d8].
#include <cstdio>

#include "gamecode.h"

int main(int argc, char** argv)
{
    if (argc < 2 || !GameCode::Load(argv[1])) { fprintf(stderr, "usage: d3dxpath <Driv3r.exe>\n"); return 1; }
    alignas(16) float a[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, o[16];
    GameRegs r;
    GameCode::Call(0x605392, r, { reinterpret_cast<uint32_t>(o), reinterpret_cast<uint32_t>(a), reinterpret_cast<uint32_t>(a) });
    auto rd = [](uint32_t g) { return *reinterpret_cast<uint32_t*>(GameCode::At(g)); };
    printf("path %u, product %08x, inverse %08x\n", rd(0x796428), GameCode::Game(rd(0x7961dc)), GameCode::Game(rd(0x7961d8)));
    return 0;
}
