// Prints every PCMP substance of the files given, as stored and after
// ConvertSubstance (mdpc.cpp), one "file -> converted" line each (8 bytes in
// hex), for comparison with the game's own code run in an emulator
// (driv3r-re\tools\presets\subconv.py <file> --all).
// Build: tests\build_subconvtest.bat. Usage: subconvtest <file>...
#include <cstdio>
#include <cstring>
#include <vector>

#include "../mdpc.h"

static std::vector<uint8_t> Read(const char* path)
{
    std::vector<uint8_t> d;
    FILE* f = fopen(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d.resize(size_t(n));
    if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}

static uint32_t U32(const std::vector<uint8_t>& d, size_t p)
{
    uint32_t v = 0;
    if (p + 4 <= d.size()) memcpy(&v, &d[p], 4);
    return v;
}

int main(int argc, char** argv)
{
    for (int a = 1; a < argc; ++a) {
        std::vector<uint8_t> d = Read(argv[a]);
        // Every "PCMP" whose substance table (+0x18 count, +0x1c offset,
        // 0x20 bytes each) lies inside the file, as subconv.py scans.
        for (size_t p = 0; p + 0x20 <= d.size(); ++p) {
            if (memcmp(&d[p], "PCMP", 4) != 0) continue;
            uint32_t n = U32(d, p + 0x18), t = U32(d, p + 0x1c);
            if (n == 0 || n >= 100000 || p + t + size_t(0x20) * n > d.size()) continue;
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t s[8];
                memcpy(s, &d[p + t + size_t(0x20) * i], 8);
                for (uint8_t b : s) printf("%02x", b);
                ConvertSubstance(s);
                printf(" -> ");
                for (uint8_t b : s) printf("%02x", b);
                printf("\n");
            }
        }
    }
    return 0;
}
