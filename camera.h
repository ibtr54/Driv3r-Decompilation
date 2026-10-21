// The player's chase camera in a car (BOOT_TO_MENU.md section 68). In the
// game the view (0x539a20) asks a chain of forwarders (vtable 0x788f2c, six
// deep in Take A Ride) for the camera, which ends at the player's camera
// controller (0x788fb8: mode 0x14d, or 0x14e with the profile's
// "defaultcamera" setting, 0x537b40), a wrapper (0x788f78, [+0x14]) and the
// chase camera, class 0x6e02c8, object C:
//   C+0x04  the car's forward axis, smoothed (0x534b60: v -= (v - w) k,
//           w = the body matrix's row 2 while row 1 y > -1, k = C+0x24 0.0328)
//   C+0x28  the look-around angle (0x536480; target -atan2 of the controls
//           0x16 / 0x17 of the input mapper [C+0x240], 0x536550)
//   C+0x40  the tracker (vtable 0x6e02dc): direction, offsets (C+0x6c:
//           distance 6.15457, height 1.28429, look height 1.28429, 0.660135),
//           the braking lurch (C+0x140.., 0x5e1680)
//   C+0xb0  the chase matrix (0x534790) and its parameters (C+0x130..)
//   C+0x1b0 the collision part (0x531870): a ray from the look point to the
//           chase eye (0x5a03f0, radius 0.25) moves the eye in front of an
//           obstacle; its look-at matrix is the camera (the draw blends it
//           with the previous one, 0x531e40)
//   C+0x244 / +0x248 a spin angle and its rate (0 here)
// The update 0x5e19e0 (vtable +8) runs once per 1/120 s step (the program
// runs it after the car's physics step); the draw's matrix is 0x5e1f10 ->
// 0x531e40. The target is the vehicle slot's second interface (vtable
// 0x788698): position = the car body's (0x5028d0), matrix = the body's
// (0x4fd7a0), velocity = the body's (0x502940); the slot's special mode
// (+0x17f0) is off in Take A Ride.
//
// Program stand-ins: the parameters (C+0x24, C+0x6c.., C+0x130..) are the
// values traced at start point 0 with vehicle 6 (the camera template the
// mode's factory fills them from was not decoded); no look-around input
// (the angle stays 0); the water lift of the eye (0x539740) is left out (it
// moves the stored eye after the matrix is built).
#pragma once

#include "collision.h"

class ChaseCamera
{
public:
    // The set-up (0x5e2370 -> 0x5e20a0, 0x5317f0): body = the car body's
    // matrix (row vectors, row 3 the position), vel = its velocity.
    void Init(const float body[16], const float vel[3], const CollisionWorld* world, const CityGround* ground);
    // 0x5e19e0, once per 1/120 s step.
    void Tick(const float body[16], const float vel[3]);
    // The draw's matrix (0x531e40): the collision part's previous and
    // current matrices blended by f, the step fraction (0x52aee0). Rows x,
    // y, z (the camera looks along -z), position.
    void Matrix(float f, float out[16]) const;

    // The traced parameters (see above).
    float follow = 0.0328f;                     // C+0x24
    float distance = 6.15457f;                  // C+0x6c / C+0x130 (+0x80 of the chase matrix)
    float height = 1.28429f;                    // C+0x70 / C+0x134
    float lookHeight = 1.28429f;                // C+0x74 / C+0x138
    float thin = 0.660135f;                     // C+0x78 / C+0x13c: obstacles this thin are ignored

private:
    void ChaseMatrix(const float target[4], const float dir[4]);  // 0x534770 / 0x534790
    void Collide();                                               // 0x531870 and the pull-in

    const CollisionWorld* world_ = nullptr;
    const CityGround* ground_ = nullptr;
    float fwd_[4] = {}, fwdTarget_[4] = {};     // C+0x04, C+0x14
    float look_ = 0;                            // C+0x28
    float spin_ = 0, spinRate_ = 0;             // C+0x244, C+0x248
    float dir_[4] = {}, dirPrev_[4] = {};       // C+0x48, C+0x58
    float lurch_ = 0;                           // C+0x140
    float prevPos_[4] = {}, prevSpeed_ = 0;     // C+0x144, C+0x154
    float lurchDir_[4] = {};                    // C+0x158
    float chase_[16] = {}, chasePrev_[16] = {}; // C+0xb0, C+0xf0
    float camPrev_[16] = {}, cam_[16] = {};     // C+0x1b0, C+0x1f0
    float eye_[4] = {};                         // C+0x220
    int rayCountdown_ = 4, blocked_ = 0x78;     // C+0x230, C+0x234
};
