// The player's car as a physics system: see carworld.h. The code was
// main.cpp's (InitCar's physics part, StepCar's tick, CarImpact,
// SpawnLooseWheel, SyncPanels, DamageState), moved here unchanged.
#include "carworld.h"

#include <cmath>
#include <cstring>

void WorldRays::Cast(WheelRay* rays, WheelHit* hits, int count)
{
    for (int i = 0; i < count; ++i) {
        const WheelRay& r = rays[i];
        hits[i] = WheelHit();
        CollisionWorld::Hit h;
        if (!world || !world->Cast(ground, r.start, r.dir, r.length, h)) continue;
        hits[i].type = 2;
        hits[i].distance = h.distance;
        hits[i].f8 = -1;
        hits[i].surface = h.surface;
        hits[i].normal[0] = h.normal[0];
        hits[i].normal[1] = h.normal[1];
        hits[i].normal[2] = h.normal[2];
        hits[i].normal[3] = 0;
    }
}

void CarWorld::Init(const VehicleData& v, int id, const float* T, float wheelOffset, const float pos[3], float heading)
{
    car = CarSim();
    car.table = T;
    car.vehicleId = id;
    for (int s = 0; s < 6; ++s) car.wheelOffset[s][1] = wheelOffset;  // 0x511780: every slot's +0x92c
    for (const VehicleWheel& w : v.wheels) {
        if (w.slot < 0 || w.slot > 5) continue;
        car.slots[w.slot] = 1;
        for (int k = 0; k < 3; ++k) car.hardpoint[w.slot][k] = w.hardpoint[k];
        car.hardpoint[w.slot][3] = 1.0f;
        car.radius[w.slot] = w.radius;
        car.width[w.slot] = w.width;
        // The model's hardpoint (+0x89c) is the UPVH one moved out by the
        // wheel's width (the PDL shape's +4) along x (0x512cc3..0x512dad):
        // s = -width for x < -0.2, +width for x > 0.2, else 0; then (s + x,
        // 0 + y, 0 + z). Vehicle 6: +-0.75399 -> +-0.87410 (BOOT_TO_MENU.md
        // section 94: the crash replay departed in the first turn without it).
        {
            const float x = w.hardpoint[0];
            const float s = x < -0.2f ? -w.width : x > 0.2f ? w.width : 0.0f;
            car.hardpoint[w.slot][0] = s + x;
            car.hardpoint[w.slot][1] = 0.0f + w.hardpoint[1];
            car.hardpoint[w.slot][2] = 0.0f + w.hardpoint[2];
        }
    }
    car.altDrop = 0;  // model +0x98c (0.241211 for vehicle 6, source not decoded; only wheels with flag 8 use it)
    car.Create(&body, pos, heading);
    car.SetAutomatic(true);
    rays.world = collision;
    rays.ground = ground;
    shapes = v.collision;
    contacts.vehicle = !shapes.empty();  // the surface rename 0x4e9160
    contacts.vehicleBottom = shapes.empty() ? 0.0f : VehicleBottom(shapes[0]);
    contacts.props = props;
    debrisContacts.props = props;
    contactCount = 0;
    contactLog = 0;
    // The slot's damage records (damage.h): the slot's sizes are twice
    // the bounding shape's half sizes (PDL z-up: x, z, y).
    vehicle = &v;
    if (!shapes.empty()) {
        const float* h = shapes[0].size;
        const float size[3] = { h[0] + h[0], h[2] + h[2], h[1] + h[1] };
        damage = VehicleDamage();
        damage.Init(v, size, T);
        // the damage marks (marks.h): the slot's marks object and the
        // world's buffers, all free
        marks.Init(v);
        markPool = MarkPool();
        // the car's box that dents (0x5091b0; damage.h)
        damage.InitDent(id, &shapes, T);
    }
    damageLog = 0;
    body.group = nullptr;
    body.member = -1;
    group.group.members.clear();
    group.bodies.clear();
    group.recs.clear();
    group.records.clear();
    group.hinged = 0;
    group.torn.clear();
    debrisLog = wheelLog = 0;
    panelLog = panelTicks = panelHitLog = 0;
    contacts.onImpulse = [this](const BodyContact& c, const float J[3]) { Impact(c, J); };
    wheelVis = WheelVisuals();
    wheelVis.Update(car);  // the slot's set-up (0x4fadc0 / 0x4fba70 run 0x503e00 too)
    wheelPrev = wheelVis;
    tyres = WheelTyres();
    for (float& x : wheelDamageSeen) x = 0;
    looseWheels.clear();
}

// The world tick's order (0x4a7410): the vehicles' controls and car tick
// (0x50e5f0), then the physics step (one 1/120 s sub-step: 0x5af710 and
// the matrix 0x5a9bf0; a sleeping body is not integrated).
int CarWorld::Step(const float controls[5], bool handbrake)
{
    if (vehicle) {
        // the slot's pre-tick 0x4fbcc0: the records' cool-downs, the
        // bonnet's timer (open beyond 0.687 for 4 s: torn off)
        SyncPanels();
        std::vector<DamageRequest> due;
        damage.Tick(*vehicle, &body.matrix[0][0], due);
        for (const DamageRequest& q : due) DamageState(q, true);
    }
    wheelPrev = wheelVis;  // 0x503e00, in the slot's pre-tick before the car's tick
    wheelVis.Update(car);
    // 0x503e00 also keeps the car's velocity and spin (slot +0x1f80 /
    // +0x1f90) for the next tear-off
    memcpy(damage.prevVel, body.vel, sizeof(damage.prevVel));
    memcpy(damage.prevW, body.w, sizeof(damage.prevW));
    car.Tick(rays);
    // The controls reach the car after its tick (BOOT_TO_MENU.md
    // section 70: run 9 replays exactly only so; before the tick a
    // change shows one 0.04 s step early).
    car.SetControls(controls, handbrake);
    // the car tick's effects block (every 1/60 s): the exhausts on while
    // slower than 5 mph, the rear tyres' smoke while slipping (smoke.h)
    {
        CarSmokeState smoke;
        const bool ran = car.Effects(body.vel, smoke);
        if (onEffects) onEffects(ran, smoke);
    }
    // The physics step's contacts, then the integration (0x5a9880): every
    // body's pairs first (0x5a31f0), then every body's ground (0x5a0020;
    // BOOT_TO_MENU.md section 98: run 36's car met its loose wheel before
    // the wheel met the ground). Within a phase: the hinged parts (the
    // car's group members; their pairs with the car are skipped, one
    // group, 0x5a31f0, not those with the world, section 97), the car, the
    // loose parts and wheels (with their pairs with the car in the first
    // phase).
    int touched = 0, panelHits = 0, carGround = 0, carPairs = 0;
    float carDeepest = 0;
    std::vector<int> debrisHits(group.bodies.size(), 0), debrisWorld(group.bodies.size(), 0);
    // the hinged parts as the step begins (one hinged by a hit in this
    // step waits for the next: run 36's hit, call 157)
    std::vector<RigidBody*> hingedNow;
    for (size_t k = 1; k < group.group.members.size(); ++k) hingedNow.push_back(group.group.members[k].body);
    for (int phase = 1; phase <= 2; ++phase) {
        debrisContacts.mode = contacts.mode = phase;
        for (RigidBody* b : hingedNow) {
            if (!b->group) continue;  // torn off in this step
            int rec = -1;
            for (size_t i = 0; i < group.bodies.size(); ++i)
                if (&group.bodies[i] == b) rec = group.records[i];
            if (rec < 0 || b->state == 0x78) continue;
            const std::vector<VehicleData::Shape>& partShapes = vehicle->parts[damage.recordPart[rec]].collision;
            // the slot's listener hears them with A the part's body (0x503fb0
            // finds the record whose body it is)
            const float massA = b->mass;
            debrisContacts.onImpulse = [this, rec, massA](const BodyContact& c, const float J[3]) { Impact(c, J, rec, massA); };
            // its surface 0xb renamed in the car's frame (0x4e9160 through
            // the part's +0x78 = the slot's listener); its ground contacts
            // only with body +0x74 = 2 (the bumpers and part 4, the joint
            // record's +0x368); from the code (Ghidra), no traced contact
            // has shown it yet (the parts' shapes are mostly not 0xb)
            debrisContacts.vehicle = contacts.vehicle;
            debrisContacts.vehicleBottom = contacts.vehicleBottom;
            debrisContacts.vehicleMatrix = &body.matrix[0][0];
            debrisContacts.vehicleGround =
                reinterpret_cast<const int*>(group.group.members[b->member].rec)[0x368 / 4] != 0;
            const int hit = debrisContacts.Step(*b, partShapes, *collision, ground);
            debrisContacts.onImpulse = nullptr;
            debrisContacts.vehicle = false;
            debrisContacts.vehicleMatrix = nullptr;
            debrisContacts.vehicleGround = true;
            if (hit && panelHitLog < 30 && log) {
                ++panelHitLog;
                log("Hinged part (record %d): %d world / %d ground contacts, deepest %.4f; car at (%.4f %.4f %.4f)", rec,
                    debrisContacts.contacts, debrisContacts.ground, debrisContacts.deepest, body.pos[0], body.pos[1],
                    body.pos[2]);
            }
            panelHits += hit;
        }
        const int carHits = contacts.Step(body, shapes, *collision, ground);
        touched += carHits;
        if (phase == 1) carPairs = contacts.pairs;
        else carGround = contacts.ground;
        carDeepest = std::fmax(carDeepest, contacts.deepest);
        // the loose parts (free bodies: the game's debris): their pairs
        // with the world and with the car (whose damage listener hears
        // those too), then their ground
        for (size_t t = 0; t < group.torn.size(); ++t) {  // (a hit can tear off another part)
            const int i = group.torn[t];
            RigidBody& b = group.bodies[i];
            const int rec = group.records[i];
            const std::vector<VehicleData::Shape>& partShapes = vehicle->parts[damage.recordPart[rec]].collision;
            if (b.state != 0x78) debrisWorld[i] += debrisContacts.Step(b, partShapes, *collision, ground);
            if (phase == 1) debrisHits[i] = contacts.Pair(body, shapes, b, partShapes);
            if (phase == 2 && (debrisHits[i] || debrisWorld[i]) && debrisLog < 30 && log) {
                ++debrisLog;
                log("Loose part (record %d): %d world / ground contacts, %d with the car; at (%.2f %.2f %.2f), "
                    "speed %.2f", rec, debrisWorld[i], debrisHits[i], b.pos[0], b.pos[1], b.pos[2],
                    std::sqrt(b.vel[0] * b.vel[0] + b.vel[1] * b.vel[1] + b.vel[2] * b.vel[2]));
            }
        }
        // the loose wheels (0x506cd0): as the loose parts
        for (LooseWheel& lw : looseWheels) {
            if (lw.body.state != 0x78) debrisContacts.Step(lw.body, lw.shapes, *collision, ground);
            if (phase == 1) contacts.Pair(body, shapes, lw.body, lw.shapes);
        }
    }
    debrisContacts.mode = contacts.mode = 0;
    (void)panelHits;
    contactCount += touched;
    if (touched && contactLog < 40 && log) {
        ++contactLog;
        log("Car contact: %d with objects and the ground (%d pairs, %d ground), deepest %.4f, at (%.2f %.2f %.2f), "
            "speed %.2f", touched, carPairs, carGround, carDeepest, body.pos[0], body.pos[1], body.pos[2],
            std::sqrt(body.vel[0] * body.vel[0] + body.vel[1] * body.vel[1] + body.vel[2] * body.vel[2]));
    }
    if (group.Active()) {
        // 0x5a9880: the group's solver moves the car and its parts
        if (onGroupStep) onGroupStep(false);
        group.Step(1.0f / 120.0f);
        if (onGroupStep) onGroupStep(true);
        if (panelLog < 20 && (++panelTicks % 30) == 0 && log) {
            ++panelLog;
            std::string qs;
            for (size_t k = 1; k < group.group.q.size(); ++k) {
                char t[32];
                snprintf(t, sizeof(t), " %.3f/%.3f", group.group.q[k], group.group.qd[k]);
                qs += t;
            }
            log("Car group: %zu members, joints (q/qd):%s; car at (%.2f %.2f %.2f)", group.group.members.size(),
                qs.c_str(), body.pos[0], body.pos[1], body.pos[2]);
        }
    } else if (body.state != 0x78) {
        body.Integrate(1.0f / 120.0f);
        body.UpdateMatrix();
    }
    // then the free bodies (0x5a9880: the groups first, then body by body)
    if (onLooseStep) onLooseStep();
    for (int i : group.torn) {
        RigidBody& b = group.bodies[i];
        if (b.state == 0x78) continue;
        b.Integrate(1.0f / 120.0f);  // 0x5af710 (the debris' sleep test 0x5af3f0 is not ported)
        b.UpdateMatrix();
    }
    for (LooseWheel& lw : looseWheels) {
        if (lw.body.state == 0x78) continue;
        lw.body.Integrate(1.0f / 120.0f);
        lw.body.UpdateMatrix();
        if (wheelLog < 20 && (++lw.ticks % 120) == 0 && log) {
            ++wheelLog;
            const RigidBody& b = lw.body;
            // the axle: the cylinder's axis, the body's x row
            log("Loose wheel %d: at (%.3f %.3f %.3f), axle y %.3f, speed %.2f", lw.wheel, b.pos[0], b.pos[1], b.pos[2],
                b.matrix[0][1], std::sqrt(b.vel[0] * b.vel[0] + b.vel[1] * b.vel[1] + b.vel[2] * b.vel[2]));
        }
    }
    return touched;
}

// The vehicle slot's contact listener (0x503fb0, damage.h) for one of the
// car body's contacts (a static object or a loose part). The parts the
// hit frees get their new state (DamageState: hinged in the car's group,
// or torn off as free bodies, drawn with their bodies).
void CarWorld::Impact(const BodyContact& c, const float J[3], int hitRecord, float massA)
{
    if (!vehicle || damage.records.empty()) return;
    SyncPanels();
    std::vector<DamageRequest> req;
    damage.Impact(*vehicle, &body.matrix[0][0], body.mass, hitRecord, hitRecord >= 0 ? massA : body.mass, J, c.pointA,
                  c.surfaceB, req);
    if (damage.lastPassed && damageLog < 40 && log) {
        ++damageLog;
        log("Car hit: strength %.3f at (%.3f %.3f %.3f) in the car, side %.4f; slot total %.4f, front %.4f / %.4f, "
            "back %.4f / %.4f", damage.lastS, damage.lastPoint[0], damage.lastPoint[1], damage.lastPoint[2],
            damage.lastSide, damage.total, damage.front[0], damage.front[1], damage.back[0], damage.back[1]);
    }
    for (const DamageRequest& q : req) DamageState(q);
    // 0x4f5890 per record of a body mesh (0x503fb0 at 0x504f71, once the
    // slot has its marks object): the marks follow a dent that moved by
    // more than 0.125.
    if (marks.active)
        for (size_t k = 0; k < damage.records.size() && k < vehicle->markFirst.size(); ++k) {
            const int part = damage.recordPart[k];
            if (part < 0 || vehicle->parts[size_t(part)].type != 1) continue;
            marks.Replace(*vehicle, markPool, int(k), damage.records[k], &body.matrix[0][0]);
        }
    // the wheel hits (0x507980): a wheel at 1.5 comes off; the slot's
    // gone byte hides it (0x4fd7f0) and the car's tick takes the wheel
    // as off (vtbl+0x7c, flag 0x80)
    for (int k = 0; k < 6; ++k)
        if (damage.wheels[k].damage != wheelDamageSeen[k]) {
            wheelDamageSeen[k] = damage.wheels[k].damage;
            if (damageLog < 60 && log) {
                ++damageLog;
                log("Wheel hit: wheel %d damage %.4f%s", k, damage.wheels[k].damage,
                    damage.wheels[k].gone ? ", comes off" : "");
            }
        }
    if (damage.wheelOff >= 0) SpawnLooseWheel(damage.wheelOff);
    for (int k = 0; k < 6; ++k) {
        tyres.gone[k] = damage.wheels[k].gone ? 1 : 0;
        car.wheelOff[k] = damage.wheels[k].gone ? 1 : 0;
    }
}

// 0x506cd0 (BOOT_TO_MENU.md section 87, run 36): the wheel that came off
// becomes a free body of mass 10 and inertia 1 from its part's PDL entry,
// at the wheel's centre (the UPVH hardpoint: run 36 (-0.7539, -0.1774,
// 1.4416) for wheel 0) with gravity (accel -10) and
// the car's rotation; its velocity the car's at that point (v + w x the
// offset with the part along w removed) plus 2.5 along the car's x
// (wheels 2, 3, 5: +, the others -: 0x48aeb0); its spin the car's plus
// the car's x times the front (wheels 0, 2) or rear wheel speed
// (vtbl+0x58 / +0x54; run 36: 18.7213 in all three components).
void CarWorld::SpawnLooseWheel(int slot)
{
    if (!vehicle) return;
    const std::vector<VehicleWheel>& wheels = vehicle->wheels;
    int part = -1, nth = 0, wheel = -1;
    for (size_t i = 0; i < vehicle->parts.size(); ++i) {
        if (vehicle->parts[i].type != 2) continue;
        if (nth < int(wheels.size()) && wheels[size_t(nth)].slot == slot) part = int(i), wheel = nth;
        ++nth;
    }
    if (part < 0) return;
    const VehicleWheel& wh = wheels[size_t(wheel)];
    const bool plus = slot == 3 || slot == 5 || slot == 2;
    const float local[3] = { wh.hardpoint[0], wh.hardpoint[1], wh.hardpoint[2] };  // the UPVH hardpoint
    const float* M = &body.matrix[0][0];
    LooseWheel lw;
    lw.wheel = wheel;
    RigidBody& b = lw.body;
    // 0x506cd0 (BOOT_TO_MENU.md section 98): the body's matrix = (the
    // hardpoint as a translation) x the wheel's matrix x the car's (D3DX
    // 0x6053a5; the wheel's adds no turn here), so the position is
    // D3DXVec4Transform((h, 1), M): (x m0 + y m1) + (z m2 + m3), and the
    // rotation comes from the rows through 0x43e1a0 (its sign may differ
    // from the car's q)
    {
        const float h4[4] = { local[0], local[1], local[2], 1.0f };
        float p4[4];
        Vec4Transform(h4, M, p4);
        for (int k = 0; k < 3; ++k) b.pos[k] = p4[k];
    }
    b.pos[3] = 1.0f;
    {
        float rows[3][4];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) rows[r][c] = M[4 * r + c];
            rows[r][3] = 0;
        }
        Articulated::QuatFromRows(rows, b.q, 1e-10, 1e-5);
    }
    b.mass = 10.0f;
    b.inertia[0] = b.inertia[1] = b.inertia[2] = 1.0f;
    b.accel[1] = -10.0f;  // the world's gravity (+0xe0, as the loose panels)
    b.state = 0x65;
    b.UpdateDerived();
    float v[3], W[3];
    const float* w = body.w;
    // the offset: the hardpoint through the car's matrix with its
    // translation cleared (0x605119; local_d0's +0x30 = (0, 0, 0, 1))
    float d[3];
    {
        float R[16];
        memcpy(R, M, sizeof(R));
        R[12] = R[13] = R[14] = 0.0f;
        R[15] = 1.0f;
        const float h4[4] = { local[0], local[1], local[2], 1.0f };
        float d4[4];
        Vec4Transform(h4, R, d4);
        for (int k = 0; k < 3; ++k) d[k] = d4[k];
    }
    float cross[3] = {};
    // 0x43e890: (z z + y y) + x x below 1e-10 is no spin; 0x41e990: n = w
    // times 1 / sqrt of the same sum; the part along n, (x + z) + y, off
    // (0x41e460); w x d as 0x407640 (eax = d, ecx = w)
    if (!((w[2] * w[2] + w[1] * w[1]) + w[0] * w[0] < 9.99999944e-11f)) {
        const float inv = 1.0f / std::sqrt((w[2] * w[2] + w[1] * w[1]) + w[0] * w[0]);
        const float n[3] = { inv * w[0], inv * w[1], inv * w[2] };
        const float along = (d[0] * n[0] + d[2] * n[2]) + d[1] * n[1];
        for (int k = 0; k < 3; ++k) d[k] = d[k] - along * n[k];
        cross[0] = d[2] * w[1] - d[1] * w[2];
        cross[1] = d[0] * w[2] - d[2] * w[0];
        cross[2] = d[1] * w[0] - d[0] * w[1];
    }
    const float push = plus ? 2.5f : -2.5f;
    const float spin = (slot == 0 || slot == 2) ? car.FrontWheelSpeed() : car.RearWheelSpeed();
    for (int k = 0; k < 3; ++k) {
        v[k] = push * M[k] + (cross[k] + body.vel[k]);  // 0x41e4b0: added + to
        W[k] = spin * M[k] + w[k];
    }
    b.SetVelocity(v);
    b.SetSpin(W);
    b.UpdateMatrix();
    // its shapes: the wheel part's PDL entry, centred on the wheel (one
    // cylinder, radius 0.3393, half width 0.1201 for vehicle 6), met by
    // boxes through box / cylinder 0x5b4380 and the ground and other
    // cylinders through the cylinder tests (contact.cpp)
    lw.shapes = vehicle->parts[size_t(part)].collision;
    looseWheels.push_back(lw);
    if (onWheelOff) onWheelOff(looseWheels.back());
    if (log)
        log("Wheel %d came off: body at (%.3f %.3f %.3f), v (%.3f %.3f %.3f), spin (%.3f %.3f %.3f)", slot, b.pos[0],
            b.pos[1], b.pos[2], b.vel[0], b.vel[1], b.vel[2], b.w[0], b.w[1], b.w[2]);
}

// The hinged parts' matrices and joint values for the damage's tests
// (the record's body +0x10, the group list's +0x20).
void CarWorld::SyncPanels()
{
    for (size_t k = 0; k < damage.records.size(); ++k) {
        const int m = group.MemberOfRecord(static_cast<int>(k));
        if (m < 0) continue;
        memcpy(damage.records[k].bodyMatrix, &group.group.members[m].body->matrix[0][0], 64);
        damage.records[k].jointQ = group.group.q[m];
    }
}

// 0x4f98b0 for a state the damage asks for: hinged (state 2) parts get
// their body in the car's group (panels.h); a part torn off (state 3,
// BOOT_TO_MENU.md section 76) leaves the group (the last one out ends
// it), takes its loose motion and stays a free body (the game's debris
// manager [0x8ac450]); one torn off the car directly gets its body now.
// timer: the bonnet timer's request (its push follows, 0x4fc1bc).
void CarWorld::DamageState(const DamageRequest& q, bool timer)
{
    DamageRecord& r = damage.records[q.record];
    const VehicleData::Part& p = vehicle->parts[damage.recordPart[q.record]];
    bool ok = true;
    bool made = false;
    if (q.state == 2 && !r.body) {
        ok = group.Hinge(*vehicle, p, body, &body.matrix[0][0], q.matrix, r.half, damage.size[0]);
        if (ok) {
            r.body = 1;
            r.f18 = 20.0f;
            r.f28 = (r.half[0] + r.half[1]) * 0.5f;
            r.flags |= 4;
            made = group.BodyOfRecord(q.record) != nullptr;
        }
    } else if (q.state == 3 && r.state < 3) {
        RigidBody* b = nullptr;
        if (r.state == 2 && r.body) {
            b = group.Tear(q.record, body, damage.prevVel, damage.prevW);  // 0x505c90 .. 0x5a1000
        } else if (!r.body && !(r.flags & 1)) {
            b = group.Loose(*vehicle, p, body, &body.matrix[0][0], q.matrix);  // 0x4f8ef0
            made = b != nullptr;
        }
        damage.TearShare(q.record);  // 0x4f9810
        if (b && timer) TornPanelKick(*b, body);
        r.body = 0;
    }
    // the part drawn with its body from now on (the caller's draw)
    if (made && onPartBody) onPartBody(q);
    if (log)
        log("Car damage: record %d (part type %d id 0x%x) %s%s (+4 %.4f +8 %.4f, dent %.4f, impact %.4f)%s", q.record,
            p.type, p.id, q.state == 2 ? "hinged" : timer ? "torn off by the bonnet timer" : "torn off",
            ok ? "" : " - refused", r.half[0], r.half[1], r.dent, r.impact,
            q.state == 2 && ok ? "; the car's group has its part bodies"
            : q.state == 3 && !group.Active() ? "; the car's group ended" : "");
    if (ok) r.state = q.state;
}
