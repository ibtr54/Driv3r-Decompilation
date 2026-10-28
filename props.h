// The smashable props (BOOT_TO_MENU.md section 81, driv3r-re\PHYSICS.md):
// the city's instances of GEBI ranges 3 and 4 (the models of the GEPD /
// HRDT 'AWHF' records: bins, posts, benches, fences) are static objects of
// group 6 in the game's physics world, met by the body contacts but not by
// the rays (collision.h). Before an impulse, the contact callback 0x4e9160
// asks 0x4e8f70 for a body's contact with one:
//   - a broken prop (its bit in the cell's set +0x20) drops the contact;
//   - the hit's momentum, |v| x mass of the moving body (a grouped body's
//     root) x 1/21000 ([0x78f1c8]), is stored in the set (+0x10c);
//   - 0x4e00f0 -> 0x4e0230: the prop's type ([0x8ac378] vtbl+0x14 by the
//     GEBI model: the AWHF record in place); none: solid (10); momentum not
//     above the type's +0x20: solid; above it the prop breaks (9, the
//     contact dropped) through 0x4e0fd0 (several parts and above +0x1c:
//     breaks apart), 0x4e0980 (range 4: a physics body; a type whose first
//     part has no model, +0x42 = 0xffff, stays solid) or 0x4e0470 (range 3:
//     debris); each marks the prop broken;
//   - a broken prop's linked pieces (GEBI +0x0e, a ring of indices in the
//     same GEBI) are broken with it (0x4e0230 forced, momentum 0).
// Not ported (stage 1): the debris and physics bodies the break makes
// (debris manager [0x8ac450] vtbl+0x1c / +0x28, [0x8ac454] vtbl+0x48), its
// collision event (0x4a94f0), the hit memory of bullets and explosions
// (0x4e0230 with +0x115: ten recent props whose damage adds up and decays).
#pragma once

#include <cstdint>
#include <vector>

#include "city.h"

class Props
{
public:
    void Init(const CityPlacement& city);

    // 0x4e8f70 for a body's contact with the prop of city instance
    // instance at momentum (|v| x mass / 21000): true when the contact is
    // dropped (broken before, or broken now: the prop and its linked pieces
    // marked broken).
    bool Hit(int instance, float momentum);
    bool Broken(int instance) const { return instance >= 0 && size_t(instance) < broken_.size() && broken_[instance]; }
    // Instances broken since the last call (for the log and the drawing).
    std::vector<int> TakeBroken();

    // The momentum 0x4e8f70 measures for one body (its velocity x mass x
    // [0x78f1c8], the game's order).
    static float Momentum(const float vel[3], float mass);

private:
    const CityPlacement::PropType* TypeOf(uint32_t model) const;
    // 0x4e0230 for one prop: true when it breaks.
    bool Breaks(int instance, float momentum, bool forced) const;
    const CityPlacement* city_ = nullptr;
    std::vector<uint8_t> broken_;
    std::vector<int> fresh_;
};
