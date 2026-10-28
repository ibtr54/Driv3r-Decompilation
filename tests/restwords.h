// A body's resting-contact record (physics.h, 0x5af440) to and from a traced
// body's 32-bit words: the support directions +0x180 / +0x190 and the
// counters +0x1a0 / +0x1a4 / +0x1b4 as they are; the sleeping neighbours
// (+0x1a8..+0x1b0, pointers of the game's process) are taken as null.
#pragma once

#include <cstdint>
#include <cstring>

#include "../physics.h"

template <class Words>
void LoadRest(const Words& w, RigidBody& b)
{
    for (int i = 0; i < 2; ++i)
        for (int k = 0; k < 4; ++k) {
            const uint32_t v = w[(0x180 + 16 * i) / 4 + k];
            memcpy(&b.rest[i][k], &v, 4);
        }
    b.restCount = static_cast<int>(w[0x1a0 / 4]);
    b.restHold = static_cast<int>(w[0x1a4 / 4]);
    b.restNear[0] = b.restNear[1] = b.restNear[2] = nullptr;
    b.restMiss = static_cast<int>(w[0x1b4 / 4]);
}

inline void StoreRest(const RigidBody& b, uint32_t* w)
{
    for (int i = 0; i < 2; ++i)
        for (int k = 0; k < 4; ++k) memcpy(&w[(0x180 + 16 * i) / 4 + k], &b.rest[i][k], 4);
    w[0x1a0 / 4] = static_cast<uint32_t>(b.restCount);
    w[0x1a4 / 4] = static_cast<uint32_t>(b.restHold);
    w[0x1a8 / 4] = w[0x1ac / 4] = w[0x1b0 / 4] = 0;
    w[0x1b4 / 4] = static_cast<uint32_t>(b.restMiss);
}
