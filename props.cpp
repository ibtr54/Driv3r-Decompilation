#include "props.h"

#include <cmath>

void Props::Init(const CityPlacement& city)
{
    city_ = &city;
    broken_.assign(city.instances.size(), 0);
    fresh_.clear();
}

float Props::Momentum(const float vel[3], float mass)
{
    // 0x4e9013..: ((z z + y y) + x x), the root, x mass, + 0 (the static
    // side), x [0x78f1c8]
    const float s = std::sqrt((vel[2] * vel[2] + vel[1] * vel[1]) + vel[0] * vel[0]);
    return (s * mass + 0.0f) * 4.76190471e-05f;
}

const CityPlacement::PropType* Props::TypeOf(uint32_t model) const
{
    for (const CityPlacement::PropType& t : city_->propTypes)
        if (t.model == model) return &t;
    return nullptr;
}

bool Props::Breaks(int instance, float momentum, bool forced) const
{
    const CityInstance& in = city_->instances[size_t(instance)];
    const CityPlacement::PropType* t = TypeOf(in.model);
    if (!t) return false;
    // (m < T) == (m == T): above the threshold; a linked piece is forced
    if (!forced && !(momentum > t->threshold)) return false;
    if (t->parts > 1 && t->shatter < momentum) return true;  // 0x4e0fd0
    if (in.range == 4 && t->firstPartModel == 0xffff) return false;  // 0x4e0980's 10
    return true;  // 0x4e0980 (range 4) / 0x4e0470 (range 3)
}

bool Props::Hit(int instance, float momentum)
{
    if (!city_ || instance < 0 || size_t(instance) >= broken_.size()) return false;
    const CityInstance& in = city_->instances[size_t(instance)];
    if (in.range != 3 && in.range != 4) return false;  // no set holds it: 10
    if (broken_[instance]) return true;
    if (!Breaks(instance, momentum, false)) return false;
    broken_[instance] = 1;
    fresh_.push_back(instance);
    // 0x4e00f0: the ring of linked pieces (GEBI +0x0e, indices in the same
    // GEBI), each through 0x4e0230 forced with no momentum
    int next = in.extra == 0xffff ? -1 : in.gebiFirst + int(in.extra);
    for (int guard = 0; next >= 0 && next != instance && size_t(next) < broken_.size() && guard < 64; ++guard) {
        const CityInstance& piece = city_->instances[size_t(next)];
        if (!broken_[next] && Breaks(next, 0.0f, true)) {
            broken_[next] = 1;
            fresh_.push_back(next);
        }
        next = piece.extra == 0xffff ? -1 : piece.gebiFirst + int(piece.extra);
    }
    return true;
}

std::vector<int> Props::TakeBroken()
{
    std::vector<int> out;
    out.swap(fresh_);
    return out;
}
