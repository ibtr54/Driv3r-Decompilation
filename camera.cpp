// The player's chase camera (camera.h, BOOT_TO_MENU.md section 68): a port
// of 0x5e19e0 and its parts, in the game's operation order where it was
// read from the listings.
#include "camera.h"

#include <cmath>
#include <cstring>

#include "carsim.h"

namespace {

// 0x417ea0: v / |v| over four components (1 / sqrt, then multiplied).
void Normalise4(const float* v, float* o)
{
    const float s = 1.0f / std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
    for (int k = 0; k < 4; ++k) o[k] = s * v[k];
}
// 0x41ea80 (ecx = p, arg q): q x p, w 0.
void CrossQP(const float* p, const float* q, float* o)
{
    const float x = q[1] * p[2] - q[2] * p[1];
    const float y = q[2] * p[0] - q[0] * p[2];
    const float z = q[0] * p[1] - q[1] * p[0];
    o[0] = x; o[1] = y; o[2] = z; o[3] = 0;
}
float Length4(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]); }
// 0x52ad60 (ecx = eye, eax = at, arg up): row 2 = normalise(eye - at), row 0
// = normalise(up x row 2), row 1 = normalise(row 2 x row 0), row 3 = eye.
void LookAt(const float* eye, const float* at, const float* up, float* m)
{
    const float d[4] = { eye[0] - at[0], eye[1] - at[1], eye[2] - at[2], eye[3] - at[3] };
    float c[4];
    Normalise4(d, &m[8]);
    CrossQP(&m[8], up, c);
    Normalise4(c, &m[0]);
    CrossQP(&m[0], &m[8], c);
    Normalise4(c, &m[4]);
    for (int k = 0; k < 4; ++k) m[12 + k] = eye[k];
}
// 0x605119 (the SSE D3DXVec4Transform on this machine): (x m0 + y m1) + (z m2 + w m3).
void Transform4(const float* v, const float* m, float* o)
{
    float r[4];
    for (int k = 0; k < 4; ++k) r[k] = (v[0] * m[k] + v[1] * m[4 + k]) + (v[2] * m[8 + k] + v[3] * m[12 + k]);
    memcpy(o, r, sizeof(r));
}
// 0x5363f0: the angle into [-pi, pi].
float Wrap(float a)
{
    while (a > 3.14159274f) a = a - 6.28318548f;
    while (a < -3.14159274f) a = a + 6.28318548f;
    return a;
}
const float kUp[4] = { 0, 1, 0, 0 };

}  // namespace

// 0x534790 (ecx = the chase matrix C+0xb0): the direction's pitch kept
// within +-10 degrees (0x534af0), the frame R = look-at(origin -> dir), the
// eye vector (0, (height - lookHeight) / distance, 1) through R, normalised
// and scaled by the distance, from the look point target + (0, lookHeight,
// 0); the matrix = look-at(eye -> look point). 0x534770 keeps the old one.
void ChaseCamera::ChaseMatrix(const float target[4], const float dir[4])
{
    memcpy(chasePrev_, chase_, sizeof(chase_));
    float d[4] = { dir[0], dir[1], dir[2], dir[3] };
    const bool above = d[1] > 0.17363999f, below = d[1] < -0.17363999f;
    if (above || below) {
        d[1] = 0;
        Normalise4(d, d);
        for (int k = 0; k < 4; ++k) d[k] = d[k] * 0.98480773f;  // 0x3f7c1bda
        d[1] = above ? 0.173648f : -0.173648f;                   // 0x3e31ceaf
    }
    const float origin[4] = { 0, 0, 0, 0 };
    float r[16];
    LookAt(origin, d, kUp, r);
    const float v[4] = { 0, (height - lookHeight) / distance, 1, 0 };
    float e[4];
    Transform4(v, r, e);
    Normalise4(e, e);
    for (int k = 0; k < 4; ++k) e[k] = e[k] * distance;  // 0x4195f0
    const float look[4] = { 0 + target[0], lookHeight + target[1], 0 + target[2], 0 + target[3] };
    const float eye[4] = { look[0] + e[0], look[1] + e[1], look[2] + e[2], look[3] + e[3] };
    LookAt(eye, look, kUp, chase_);
}

// 0x531870 (ecx = C+0x1b0; the chase matrix, the radius sqrt(distance^2 +
// lookHeight^2) (5e1d19: offsets [0] and [2]), the thin size), then the
// pull-in of 0x5e19e0 when the eye ends up nearer than 0.7 to the look point.
void ChaseCamera::Collide()
{
    memcpy(camPrev_, cam_, sizeof(cam_));
    const float h = std::sqrt(distance * distance + lookHeight * lookHeight);  // 0x5e1480
    const float* row2 = &chase_[8];
    const float* want = &chase_[12];
    const float look[4] = { want[0] - row2[0] * h, want[1] - row2[1] * h, want[2] - row2[2] * h,
                            want[3] - row2[3] * h };
    memcpy(eye_, want, sizeof(eye_));
    if (--blocked_ < 0) blocked_ = 0;
    bool cast = true;
    if (blocked_ < 1) cast = --rayCountdown_ == 0;
    bool hit = false;
    if (cast) {
        // 0x531650: a ray from the look point to the chase eye (0x5a03f0,
        // radius 0.25); on a hit the point where it stops.
        const float d[4] = { want[0] - look[0], want[1] - look[1], want[2] - look[2], want[3] - look[3] };
        const float len = Length4(d);
        CollisionWorld::Hit ray;
        if (world_ && len > 9.99999975e-06f) {
            const float dir[3] = { d[0] / len, d[1] / len, d[2] / len };
            if (world_->Cast(ground_, look, dir, len, ray, 0.25f)) {
                // Thin obstacles are passed: a cylinder narrower than the
                // thin size, a box whose turned extents in x and z are both
                // within it.
                bool skip = false;
                if (ray.shape >= 0) {
                    const CollisionWorld::Shape& s = world_->ShapeAt(size_t(ray.shape));
                    if (s.type == 1) skip = s.size[0] < thin;
                    else if (s.type == 0) {
                        const float* m = s.world;
                        const float x = s.size[0] * m[0] + s.size[1] * m[4] + s.size[2] * m[8];
                        const float z = s.size[0] * m[2] + s.size[1] * m[6] + s.size[2] * m[10];
                        skip = std::fabs(x) <= thin && std::fabs(z) <= thin;
                    }
                }
                if (!skip) {
                    const float p[4] = { look[0] + dir[0] * ray.distance, look[1] + dir[1] * ray.distance,
                                         look[2] + dir[2] * ray.distance, look[3] };
                    const float q[4] = { p[0] - look[0], p[1] - look[1], p[2] - look[2], p[3] - look[3] };
                    if (Length4(q) > 0.00999999978f) {
                        memcpy(eye_, p, sizeof(eye_));
                        blocked_ = 0x78;
                        hit = true;
                    }
                }
            }
        }
        rayCountdown_ = 4;
    }
    if (!hit) memcpy(eye_, want, sizeof(eye_));
    LookAt(eye_, look, kUp, cam_);
    const float g[4] = { eye_[0] - look[0], eye_[1] - look[1], eye_[2] - look[2], eye_[3] - look[3] };
    if (Length4(g) < 0.699999988f) {
        // 0x531d70: the eye 0.7 from the look point along the chase axis.
        const float t2 = h - 0.699999988f;
        const float e2[4] = { want[0] - t2 * row2[0], want[1] - t2 * row2[1], want[2] - t2 * row2[2],
                              want[3] - t2 * row2[3] };
        LookAt(e2, look, kUp, cam_);
    }
}

void ChaseCamera::Init(const float body[16], const float vel[3], const CollisionWorld* world, const CityGround* ground)
{
    world_ = world;
    ground_ = ground;
    for (int k = 0; k < 4; ++k) fwd_[k] = fwdTarget_[k] = body[8 + k];
    look_ = 0;
    spin_ = -0.0f;
    spinRate_ = 0;
    memcpy(dir_, fwd_, sizeof(dir_));
    memcpy(dirPrev_, fwd_, sizeof(dirPrev_));
    lurch_ = 0;
    for (int k = 0; k < 4; ++k) prevPos_[k] = body[12 + k];
    prevSpeed_ = 0;
    const float v[4] = { vel[0], vel[1], vel[2], 0 };
    if (Length4(v) > 0.00999999978f) Normalise4(v, lurchDir_);
    else memset(lurchDir_, 0, sizeof(lurchDir_));
    ChaseMatrix(&body[12], dir_);
    memcpy(chasePrev_, chase_, sizeof(chase_));
    // 0x5317f0: the counters, then 0x531870 twice.
    rayCountdown_ = 4;
    blocked_ = 0x78;
    memcpy(cam_, chase_, sizeof(cam_));
    Collide();
    Collide();
    memcpy(camPrev_, cam_, sizeof(cam_));
}

void ChaseCamera::Tick(const float body[16], const float vel[3])
{
    // The spin (C+0x244 -= C+0x248, wrapped by 2 pi below 0).
    spin_ = spin_ - spinRate_;
    if (spin_ < 0) spin_ = spin_ + 6.28318548f;
    // 0x536480 with no look-around input: the target angle 0; a turn of more
    // than 3 pi / 4 snaps, else the angle follows by 0.052 of the gap.
    const float target = 0.0f;
    if (std::fabs(Wrap(look_ - target)) > 2.3561945f) look_ = target;
    else look_ = Wrap(look_ - Wrap(Wrap(look_ - target) * 0.0520000011f));
    // The car's forward axis, smoothed while the car is not upside down.
    if (body[5] > -1.0f) {
        for (int k = 0; k < 4; ++k) fwdTarget_[k] = body[8 + k];
        float d[4];
        for (int k = 0; k < 4; ++k) d[k] = (fwd_[k] - fwdTarget_[k]) * follow;
        for (int k = 0; k < 4; ++k) fwd_[k] = fwd_[k] - d[k];
    }
    // Turned by the look angle and the spin about y (rows (c 0 -s) (0 1 0)
    // (s 0 c)).
    const float a = look_ + spin_;
    const float c = std::cos(a), s = std::sin(a);
    const float rot[16] = { c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1 };
    float dir[4];
    Transform4(fwd_, rot, dir);
    // 0x5e12d0: the tracker keeps the last direction that was not
    // near-vertical.
    if (std::fabs(dir_[1]) < 0.99000001f) memcpy(dirPrev_, dir_, sizeof(dir_));
    memcpy(dir_, dir, sizeof(dir_));
    // 0x5e1680 (no offset blend in progress, C+0xac = 0): the braking lurch.
    const float* pos = &body[12];
    const float dp[4] = { pos[0] - prevPos_[0], pos[1] - prevPos_[1], pos[2] - prevPos_[2], pos[3] - prevPos_[3] };
    float speed = Length4(dp);
    if (speed < 0.0500000007f) speed = 0;
    float dv = speed - prevSpeed_;
    if (!(dv <= -0.0599999987f)) dv = 0;
    if (dv < lurch_) lurch_ = dv;
    lurch_ = dv * 0.200000003f + (1.0f - 0.200000003f) * lurch_;  // 0x52ac70
    memcpy(prevPos_, pos, sizeof(prevPos_));
    prevSpeed_ = speed;
    const float r = std::sqrt(-lurch_);
    float o[4];
    for (int k = 0; k < 4; ++k) o[k] = r * lurchDir_[k];
    for (int k = 0; k < 4; ++k) o[k] = o[k] + o[k];
    const float tgt[4] = { o[0] + pos[0], o[1] + pos[1], o[2] + pos[2], o[3] + pos[3] };
    ChaseMatrix(tgt, std::fabs(dir_[1]) >= 0.99000001f ? dirPrev_ : dir_);
    // 0x5e14d0: the lurch direction from the velocity.
    const float v[4] = { vel[0], vel[1], vel[2], 0 };
    if (Length4(v) > 0.00999999978f) Normalise4(v, lurchDir_);
    else memset(lurchDir_, 0, sizeof(lurchDir_));
    Collide();
}

void ChaseCamera::Matrix(float f, float out[16]) const
{
    BlendBodyMatrix(camPrev_, cam_, f, out);
}
