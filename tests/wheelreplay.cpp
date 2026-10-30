// The loose wheel of run 36 (BOOT_TO_MENU.md section 87): the first traced
// state of the wheel's body ("WI 0": the body as 0x506cd0 made it) is
// stepped with the program's physics: the body's contacts with the city
// (BodyContacts::Step on the program's collision world and ground) and the
// integration, one per traced integration ("WI n"), each step compared with
// the traced body: position, velocity and spin. The wheel's shapes: vehicle
// 6's part of wheel slot 0, about the wheel's centre (main.cpp
// SpawnLooseWheel; the UPVH hardpoint is its centre). Free run (no reload): the drift is what the program does.
//   wheelreplay <phys.txt> <city.d3c> <Miami.vvs> [steps]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../city.h"
#include "../collision.h"
#include "../contact.h"
#include "../physics.h"
#include "../vehicle.h"
#include "restwords.h"

namespace {

std::vector<uint8_t> ReadAll(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

float F(unsigned u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void Load(const std::vector<unsigned>& w, RigidBody& b)
{
    auto f = [&](int off) { return F(w[off / 4]); };
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) b.matrix[r][c] = f(0x10 + r * 16 + c * 4);
    for (int i = 0; i < 4; ++i) {
        b.pos[i] = f(0x80 + 4 * i); b.vel[i] = f(0x90 + 4 * i); b.q[i] = f(0xa0 + 4 * i); b.L[i] = f(0xb0 + 4 * i);
        b.w[i] = f(0x150 + 4 * i); b.dq[i] = f(0x160 + 4 * i); b.inertia[i] = f(0x170 + 4 * i);
        b.force[i] = f(0xc0 + 4 * i); b.torque[i] = f(0xd0 + 4 * i); b.accel[i] = f(0xe0 + 4 * i);
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) { b.R[r][c] = f(0xf0 + r * 16 + c * 4); b.invI[r][c] = f(0x120 + r * 16 + c * 4); }
    b.mass = f(0x1c0);
    b.state = int(w[0x1c4 / 4]);
    LoadRest(w, b);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: wheelreplay phys.txt city.d3c Miami.vvs [steps]\n");
        return 2;
    }
    const int steps = argc > 4 ? atoi(argv[4]) : 400;
    // the traced states of the first loose wheel (its address from "WB")
    std::vector<std::vector<unsigned>> wi;
    std::vector<char> hits;  // per WI index: a body pair with the wheel before it
    std::string first;
    {
        FILE* f = fopen(argv[1], "rb");
        if (!f) return 2;
        std::vector<char> buf(1 << 16);
        while (fgets(buf.data(), int(buf.size()), f)) {
            // a body pair with the wheel ("NP n A B": the car's hits) before
            // WI n: that step takes a contact the replay does not make
            if (strncmp(buf.data(), "NP ", 3) == 0 && !first.empty() && strstr(buf.data(), first.c_str()))
                hits.resize(wi.size() + 1, 0), hits[wi.size()] = 1;
            if (strncmp(buf.data(), "WI ", 3) != 0) continue;
            char body[16] = {};
            unsigned n = 0;
            sscanf(buf.data() + 3, "%u %15s", &n, body);
            if (first.empty()) first = body;
            if (first != body) break;
            std::vector<unsigned> w;
            const char* p = buf.data();
            for (int k = 0; k < 4; ++k) p = strchr(p, ' ') + 1;
            for (;;) {
                char* end = nullptr;
                unsigned long v = strtoul(p, &end, 16);
                if (end == p) break;
                w.push_back(unsigned(v));
                p = end;
            }
            wi.push_back(w);
        }
        fclose(f);
    }
    printf("%zu traced states of the wheel %s\n", wi.size(), first.c_str());
    if (wi.size() < 2) return 1;
    CityPlacement city;
    if (!city.Load(ReadAll(argv[2]))) {
        printf("city: %s\n", city.Error().c_str());
        return 1;
    }
    CollisionWorld world;
    world.Build(city);
    std::vector<VehicleData> vehicles;
    if (!ReadVehicles(ReadAll(argv[3]), vehicles)) return 1;
    const VehicleData* v = nullptr;
    for (const VehicleData& x : vehicles)
        if (x.id == 6) v = &x;
    if (!v) return 1;
    // wheel slot 0's part and its place (as SpawnLooseWheel)
    int part = -1, nth = 0;
    const VehicleWheel* wh = nullptr;
    for (size_t i = 0; i < v->parts.size(); ++i) {
        if (v->parts[i].type != 2) continue;
        if (nth < int(v->wheels.size()) && v->wheels[size_t(nth)].slot == 0) part = int(i), wh = &v->wheels[size_t(nth)];
        ++nth;
    }
    if (part < 0) return 1;
    // the UPVH hardpoint is the wheel's centre (run 36: (-0.7539, -0.1774, 1.4416))
    const float local[3] = { wh->hardpoint[0], wh->hardpoint[1], wh->hardpoint[2] };
    // centred on the wheel already (as SpawnLooseWheel; a cylinder: box /
    // cylinder for the city's boxes, its bounding box for the ground)
    const std::vector<VehicleData::Shape> shapes = v->parts[size_t(part)].collision;
    printf("wheel part %d: %zu shapes, centre in the car (%.4f %.4f %.4f)\n", part, shapes.size(), local[0], local[1],
           local[2]);
    for (const VehicleData::Shape& s : shapes)
        printf("  shape type %d surface %d size (%.4f %.4f %.4f) at (%.4f %.4f %.4f), axis y row (%.3f %.3f %.3f)\n",
               s.type, s.surface, s.size[0], s.size[1], s.size[2], s.local[12], s.local[13], s.local[14], s.local[4],
               s.local[5], s.local[6]);
    RigidBody b;
    Load(wi[0], b);
    BodyContacts contacts;
    // WHEEL_RELOAD=1: each step from the traced state before it (one-step
    // differences); WHEEL_KICKS=1: free run, but a step with a body pair
    // (the car's hits, not replayed here) takes the traced state after it;
    // WHEEL_ALL=1: every step printed
    const bool reload = getenv("WHEEL_RELOAD") != nullptr;
    const bool kicks = getenv("WHEEL_KICKS") != nullptr;
    const bool all = getenv("WHEEL_ALL") != nullptr;
    hits.resize(wi.size(), 0);
    int firstOff = 0;
    for (int n = 1; n < int(wi.size()) && n <= steps; ++n) {
        if (reload) Load(wi[size_t(n - 1)], b);
        if (kicks && hits[size_t(n)]) {
            Load(wi[size_t(n)], b);
            continue;
        }
        // WI n is the body at the integrator's entry (after step n's
        // contacts): integrate, then step n + 1's contacts
        if (b.state != 0x78) {
            b.Integrate(1.0f / 120.0f);
            b.UpdateMatrix();
        }
        const int touched = b.state != 0x78 ? contacts.Step(b, shapes, world, &city.ground) : 0;
        RigidBody t;
        Load(wi[size_t(n)], t);
        float dp = 0, dv = 0;
        for (int k = 0; k < 3; ++k) {
            dp = std::fmax(dp, std::fabs(b.pos[k] - t.pos[k]));
            dv = std::fmax(dv, std::fabs(b.vel[k] - t.vel[k]));
        }
        float dw = 0;
        for (int k = 0; k < 3; ++k) dw = std::fmax(dw, std::fabs(b.w[k] - t.w[k]));
        const bool off = dp > 1e-4f || dv > 1e-3f || dw > 1e-3f;
        if (off && !firstOff) firstOff = n;
        if (all || n <= 5 || n % 20 == 0 || (reload && off))
            printf("%4d: port (%.3f %.3f %.3f) v (%.2f %.2f %.2f) | game (%.3f %.3f %.3f) v (%.2f %.2f %.2f) | dpos %.3f dvel %.2f dw %.3f wheel axis y %.3f / %.3f, deepest %.6f%s%s\n",
                   n, b.pos[0], b.pos[1], b.pos[2], b.vel[0], b.vel[1], b.vel[2], t.pos[0], t.pos[1], t.pos[2], t.vel[0],
                   t.vel[1], t.vel[2], dp, dv, dw, b.matrix[0][1], t.matrix[0][1], contacts.deepest, touched ? " contact" : "",
                   hits[size_t(n)] ? " (car pair)" : "");
    }
    printf("first step off (pos 1e-4, vel / spin 1e-3): %d; %d steps with a body pair%s\n", firstOff,
           int(std::count(hits.begin(), hits.end(), 1)), kicks ? " taken from the trace" : "");
    return 0;
}
