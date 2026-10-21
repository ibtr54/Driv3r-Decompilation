// physreplay <phys.txt>: replays traced bodies of the original game through
// physics.cpp (BOOT_TO_MENU.md section 62). The trace (driv3r-re\tools\physics
// proxy, D3PROXY_PHYS) has "I n body h <0x100 dwords>" = the player's body at
// the entry of the integrator 0x5af710 for each sub-step. For each pair n,
// n + 1: load n, Integrate(h), UpdateMatrix and compare with n + 1, field by
// field, bit for bit. Pairs with a 25 Hz impulse in between differ in vel /
// L (reported as "impulse"); everything else must be equal.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../physics.h"

struct Dump { unsigned n; float h; std::vector<unsigned> w; };

static float F(unsigned u) { float f; memcpy(&f, &u, 4); return f; }

static void Load(const std::vector<unsigned>& w, RigidBody& b)
{
    auto f = [&](int off) { return F(w[off / 4]); };
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) b.matrix[r][c] = f(0x10 + r * 16 + c * 4);
    for (int i = 0; i < 4; ++i) {
        b.pos[i] = f(0x80 + 4 * i); b.vel[i] = f(0x90 + 4 * i); b.q[i] = f(0xa0 + 4 * i); b.L[i] = f(0xb0 + 4 * i);
        b.force[i] = f(0xc0 + 4 * i); b.torque[i] = f(0xd0 + 4 * i); b.accel[i] = f(0xe0 + 4 * i);
        b.w[i] = f(0x150 + 4 * i); b.dq[i] = f(0x160 + 4 * i); b.inertia[i] = f(0x170 + 4 * i);
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) { b.R[r][c] = f(0xf0 + r * 16 + c * 4); b.invI[r][c] = f(0x120 + r * 16 + c * 4); }
    b.mass = f(0x1c0);
    b.state = static_cast<int>(w[0x1c4 / 4]);
}

struct Field { const char* name; int off; int count; };
static const Field kFields[] = {
    { "pos", 0x80, 3 }, { "vel", 0x90, 3 }, { "q", 0xa0, 4 }, { "L", 0xb0, 3 }, { "R", 0xf0, 12 },
    { "invI", 0x120, 12 }, { "w", 0x150, 3 }, { "dq", 0x160, 4 }, { "matrix", 0x10, 16 },
};

static float Get(const RigidBody& b, int off)
{
    const float* p = nullptr;
    if (off >= 0x10 && off < 0x50) p = &b.matrix[0][0] + (off - 0x10) / 4;
    else if (off >= 0x80 && off < 0x90) p = b.pos + (off - 0x80) / 4;
    else if (off >= 0x90 && off < 0xa0) p = b.vel + (off - 0x90) / 4;
    else if (off >= 0xa0 && off < 0xb0) p = b.q + (off - 0xa0) / 4;
    else if (off >= 0xb0 && off < 0xc0) p = b.L + (off - 0xb0) / 4;
    else if (off >= 0xf0 && off < 0x120) p = &b.R[0][0] + (off - 0xf0) / 4;
    else if (off >= 0x120 && off < 0x150) p = &b.invI[0][0] + (off - 0x120) / 4;
    else if (off >= 0x150 && off < 0x160) p = b.w + (off - 0x150) / 4;
    else if (off >= 0x160 && off < 0x170) p = b.dq + (off - 0x160) / 4;
    return p ? *p : 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: physreplay phys.txt\n"); return 2; }
    FILE* f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }
    std::vector<Dump> dumps;
    static char line[1 << 16];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != 'I' || line[1] != ' ') continue;
        Dump d;
        char* p = line + 2;
        d.n = strtoul(p, &p, 10);
        strtoul(p, &p, 16);                       // body address
        d.h = F(static_cast<unsigned>(strtoul(p, &p, 16)));
        while (*p && *p != '\n') { d.w.push_back(static_cast<unsigned>(strtoul(p, &p, 16))); while (*p == ' ') ++p; }
        if (d.w.size() >= 0x78) dumps.push_back(std::move(d));
    }
    fclose(f);
    printf("%zu integrator dumps\n", dumps.size());
    int exact = 0, impulse = 0, other = 0;
    std::map<std::string, int> worst;
    for (size_t i = 0; i + 1 < dumps.size(); ++i) {
        if (dumps[i + 1].n != dumps[i].n + 1) continue;
        RigidBody b;
        Load(dumps[i].w, b);
        b.Integrate(dumps[i].h);
        b.UpdateMatrix();
        RigidBody next;
        Load(dumps[i + 1].w, next);
        std::string diff;
        bool velOrL = false;
        for (const Field& fl : kFields)
            for (int k = 0; k < fl.count; ++k) {
                if ((fl.off == 0xf0 || fl.off == 0x120) && k % 4 == 3) continue;
                const int off = fl.off + 4 * k;
                const float a = Get(b, off), e = F(dumps[i + 1].w[off / 4]);
                if (memcmp(&a, &e, 4) != 0) {
                    char t[96];
                    snprintf(t, sizeof(t), " %s[%d] %.9g/%.9g", fl.name, k, a, e);
                    diff += t;
                    if (fl.off == 0x90 || fl.off == 0xb0) velOrL = true;
                    ++worst[fl.name];
                }
            }
        if (diff.empty()) ++exact;
        else if (velOrL) { ++impulse; if (impulse <= 3) printf("step %u impulse?:%s\n", dumps[i].n, diff.substr(0, 300).c_str()); }
        else { ++other; if (other <= 10) printf("step %u differs:%s\n", dumps[i].n, diff.substr(0, 400).c_str()); }
    }
    printf("exact %d, impulse %d, other %d\n", exact, impulse, other);
    for (auto& w : worst) printf("  field %s differed %d times\n", w.first.c_str(), w.second);
    return other ? 1 : 0;
}
