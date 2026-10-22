// The articulated groups (BOOT_TO_MENU.md section 72, driv3r-re\PHYSICS.md):
// when a crash frees damage panels (door, bonnet, bumper...), the vehicle
// slot hinges each one to the car with a revolute joint (0x5a1890), and the
// car and its panels become one group: a tree of bodies (body +0x1d8 holds
// the member's group record) that the physics step solves together with
// Featherstone's articulated-body algorithm (0x5c2600, called per group from
// 0x5a98a5):
//   - every other call (the list's +8 toggle) the dynamics (0x5c2100):
//     outward velocities (0x5c1040), bias forces and inertias (0x5c1390),
//     inward articulated inertias and forces (0x5c1da0) with the joint's
//     spring, damper and stop torque, outward accelerations (0x5c1ef0);
//   - then the root (the car) is integrated with the root acceleration and
//     the joints with theirs, each joint stopped at its limits (a bounce
//     when slow, an impulse through the group when fast: 0x5c7280,
//     0x5c81b0), and every member's matrix rebuilt (0x5c06d0).
//
// A member's group record ("rec" in the comments) keeps the game's layout
// (the first 0x3c0 bytes) so that the replay test can load the traced ones;
// its body is a RigidBody. The offsets are the game's.
#pragma once

#include <cstdint>
#include <vector>

#include "physics.h"

namespace Articulated
{
// The game's spatial vector (0x20 bytes): angular part, then linear, each
// padded to four floats.
struct SVec
{
    float t[4] = {}, b[4] = {};
};
// The game's 6x6 spatial matrix (0xc0 bytes): four 3x3 blocks of rows of
// four floats. Element (k, i) (0x5bea30, 0x5bead0) lives in A[k][i] for
// k, i < 3, B[k-3][i] for k >= 3, C[k][i-3] for i >= 3, D[k-3][i-3] for both.
struct SMat
{
    float A[3][4] = {}, B[3][4] = {}, C[3][4] = {}, D[3][4] = {};
};

// The primitives, one per game routine (exposed for the replay test).
void M3V(const float m[3][4], const float v[4], float out[4]);                 // 0x5bdef0
void VecRot(const float m[3][4], const float v[4], float out[4]);              // 0x5bf350
void Add(const SVec& a, const SVec& b, SVec& out);                             // 0x5bdff0
void Scale(const SVec& v, float s, SVec& out);                                 // 0x5be0b0
void VecTimesMat(const SVec& v, const SMat& m, SVec& out);                     // 0x5be2b0
void MatTimesVec(const SMat& m, const SVec& v, SVec& out);                     // 0x5be3f0
void Add(const SMat& a, const SMat& b, SMat& out);                             // 0x5be530
void Scale(float s, const SMat& m, SMat& out);                                 // 0x5be7c0
void Sub(const SMat& a, const SMat& b, SMat& out);                             // 0x5c6170
bool IsZero(const SVec& v);                                                    // 0x5c5f40
// (thresholds: |row 0 x row 1 - row 2|^2 and |column - 1|; 0x43e1a0 uses
// 1e-10 and 1e-5)
void QuatFromRows(const float r[][4], float quat[4], double crossEps = 1e-4, double colEps = 1e-5);  // 0x5c06d0 / 0x5abd60
void Outer(const SVec& b, const SVec& c, SMat& out);                           // 0x5bea30
bool Solve(SVec& x, SMat& m, SVec& rhs);                                       // 0x5bead0
void Transform(const float r[4][4], const SVec& v, SVec& out);                 // 0x5bef80
void Transform(const float r[4][4], const SMat& m, SMat& out);                 // 0x5bf640
void QuatRotate(const float q[4], const float v[4], float out[4]);             // 0x58c1f0
// D3DXMatrixMultiply as the game runs it here (the SSE routine 0x66fc51
// its start-up picks, path 2 of 0x60b89d): out = a b.
void MatMul(const float a[16], const float b[16], float out[16]);
// 0x5aa430: the inverse of a rigid matrix (rotation transposed, offset
// through D3DXVec3Transform 0x66cad5).
void RigidInverse(const float m[16], float out[16]);
// The game's own cosine and sine (0x401070 / 0x401160): reduced into
// [-pi/2, pi/2], then series.
float Cos(float x);
float Sin(float x);

// One group: the game's member list (0x30 bytes: +0 count, +8 the dynamics
// toggle, +0x20 joint values, +0x24 joint rates, +0x28 bodies). Member 0 is
// the root (joint type rec +0x330 = 0, the car); the others hang from a
// parent (rec +0x32c in the game, an index here) by a revolute (type 1) or
// prismatic (type 2) joint.
class Group
{
public:
    struct Member
    {
        RigidBody* body = nullptr;
        float* rec = nullptr;       // the group record (0x3c0 bytes, game offsets)
        int parent = -1;
        // the joints hanging from it (rec +0x338 count, arrays +0x33c..+0x34c):
        // the child, the hinge point, axis and reference axis in this body,
        // the rotation between the two hinge frames
        struct Child
        {
            int member;
            float pos[4], axis[4], ref[4], quat[4];
        };
        std::vector<Child> children;
    };
    std::vector<Member> members;
    std::vector<float> q, qd;       // list +0x20 / +0x24
    int toggle = 0;                 // list +8

    // 0x5aada0 + 0x5abd60: hangs body (with its group record rec) below
    // member parent by a joint of type 1 (revolute) or 2 (prismatic): the
    // hinge point, axis and a reference axis in the body (cp, cax, cref) and
    // in the parent (pp, pax, pref). The new member goes right after its
    // parent (later members move up); q and qd start at 0, the frames, matrix
    // and velocity follow. Returns its index.
    int AddMember(int parent, RigidBody* body, float* rec, const float cp[3], const float cax[3], const float cref[3],
                  const float pp[3], const float pax[3], const float pref[3], int type);
    // The game's group root (0x5aabe0: a member list with the toggle at 1;
    // 0x5aada0: the record's defaults; 0x5aae70: joint type 0, no parent,
    // +0x334 = -1, q = qd = 0, the frames 0x5ae2b0): body with record rec
    // (0x3c0 bytes, zeroed by the caller) becomes member 0.
    void MakeRoot(RigidBody* body, float* rec);
    // Points every member's body at this group and its index (body +0x1d8).
    void Link();
    // 0x5c2600: one physics sub-step of h.
    void Step(float h);
    // 0x5c2100: one evaluation of the dynamics (the joint accelerations
    // rec +0x378, the root's spatial acceleration rec +0x280).
    void Dynamics();
    // 0x5ae2b0: the member's joint frames from its joint value (rec +0xc0
    // and its inverse +0x100; the root's: its world matrix +0x100 and the
    // inverse +0xc0).
    void Frames(int k);
    // 0x5c06d0: the member's body matrix, position, quaternion and rotation
    // from the frames (the root's matrix only).
    void BodyMatrix(int k);
    // 0x5b11c0 for a member of a group: every member woken (0x65), the
    // impulse J and the angular impulse A (world) turned into the member's
    // frame (by its matrix's rotation transposed, 0x406ce0 / 0x41e510), the
    // impulse inertias (0x5c7280), then BodyImpulse.
    void Impulse(int k, const float J[3], const float A[3]);
    // 0x5c7280: every member's rigid inertia (0x5c7740), then the impulse
    // inertias inward (0x5c7930).
    void ImpulseInertias();
    // 0x5c7390: the spatial impulse p (the member's frame; linear, then
    // angular) on member k through the group: rec +0x220 = -p handed up the
    // chain to the root (0x5c7a70, flagged joints stop it), every member's
    // response (0x5c7b40), the root's velocity and spin and the joint rates
    // changed, the velocities again (0x5c1040).
    void BodyImpulse(int k, const SVec& p);
    // 0x5c72d0: the member's spatial acceleration (rec +0x280) under the
    // test impulse p, nothing moved: rec +0x220 = -p handed up the chain
    // (0x5c7a70, flag 0), the responses from the root down to the member
    // (0x5c7b40, flag 1); the records keep these intermediates.
    void TestImpulse(int k, const SVec& p, SVec& out);
    // 0x5c67c0: the inverse inertia K of member k at a contact (rows C, the
    // contact frame; point, the contact point relative to the body, world
    // axes), in the contact frame: the impulse inertias (0x5c7280), T = R C
    // (R the body's rotation rows, +0xf0), X1 = [[T', 0], [T' [r]x, T']]
    // with r = point T', X2 = [[T, 0], [T [-point]x, T]] (0x5be170), then
    // per axis e_i: K row i = bottom of TestImpulse((e_i, 0) X1) X2. x1:
    // X1, for ContactImpulse (0x5c5fa0).
    void ContactInertia(int k, const float C[3][4], const float point[3], float K[3][4], SMat& x1);
    // The grouped side of 0x5c3110's impulse: (P, 0) (0x5a9da0; times -1
    // for body B, 0x5be0b0) times X1 (0x5be2b0), then BodyImpulse.
    void ContactImpulse(int k, const SMat& x1, const float P[3], bool negate);

private:
    void Velocities(int k);         // 0x5c1040
    void Bias(int k);               // 0x5c1390
    void Inward(int k);             // 0x5c2100's backward loop
    void Propagate(int k);          // 0x5c1da0
    void Accelerations(int k);      // 0x5c1ef0
    void ParentVelocity(int k, SVec& out);
    void Limits(int k);
    // A joint impulse through the group: 0x5c7280 (the articulated inertias
    // again, without bias: rigid inertias 0x5c7740, inward 0x5c7930), then
    // 0x5c81b0 (the joint's response to a unit impulse, the impulse scaled
    // by it and applied, 0x5c7d80).
    void LimitImpulse(int k, float impulse);
    void ImpulseInertia(int k);                    // 0x5c7930
    void ImpulseUp(int k, int flag);               // 0x5c7a70
    void Response(int k, int flag);                // 0x5c7b40
    void Response(int k, float f, int flag);       // 0x5c82f0
    void ApplyImpulse(int k, float s);             // 0x5c7d80
    std::vector<int> Ancestors(int k);
};
}  // namespace Articulated
