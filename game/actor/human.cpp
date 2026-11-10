#include "actor/human.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "core/rng.h"

namespace zl {

namespace {

constexpr f32 kTau = 2.0f * kPi;
constexpr f32 rad(f32 degrees) { return degrees * (kPi / 180.0f); }

// --- measurements, in metres ---------------------------------------------------

constexpr f32 kHipStanding = 0.915f;  // height of the hip joint when standing
constexpr f32 kHipWidth = 0.092f;     // from the spine out to each hip joint
constexpr f32 kShoulderWidth = 0.185f;
constexpr f32 kAnkleHeight = 0.075f;
constexpr f32 kHeelBack = 0.020f;    // sole: heel behind the ankle,
constexpr f32 kBallAhead = 0.105f;    // ball of the foot ahead of it,
constexpr f32 kToeLength = 0.045f;    // and the toes beyond that
constexpr f32 kHeadRadius = 0.098f;

// --- frames ---------------------------------------------------------------------

// A position and three perpendicular unit axes: right, forward, up.
struct Frame {
    Vec3 o;
    Vec3 r{1, 0, 0}, f{0, 1, 0}, u{0, 0, 1};

    Vec3 at(f32 side, f32 forward, f32 up) const { return o + r * side + f * forward + u * up; }
};

// Turns to the right.
Frame yaw(Frame m, f32 angle) {
    const f32 c = std::cos(angle), s = std::sin(angle);
    const Vec3 f = m.f * c + m.r * s, r = m.r * c - m.f * s;
    m.f = f;
    m.r = r;
    return m;
}

// Leans forward.
Frame pitch(Frame m, f32 angle) {
    const f32 c = std::cos(angle), s = std::sin(angle);
    const Vec3 u = m.u * c + m.f * s, f = m.f * c - m.u * s;
    m.u = u;
    m.f = f;
    return m;
}

// Leans to the right.
Frame roll(Frame m, f32 angle) {
    const f32 c = std::cos(angle), s = std::sin(angle);
    const Vec3 u = m.u * c + m.r * s, r = m.r * c - m.u * s;
    m.u = u;
    m.r = r;
    return m;
}

f32 fract(f32 v) { return v - std::floor(v); }

// Smooth noise in 0..1 along one axis: something that wanders.
f32 wander(f32 t, u32 seed) {
    const f32 cell = std::floor(t), u = t - cell;
    const i32 i = static_cast<i32>(cell);
    return lerp(hash_to_unit(hash2(i, 0, seed)), hash_to_unit(hash2(i + 1, 0, seed)), u * u * (3.0f - 2.0f * u));
}

// --- gait -----------------------------------------------------------------------

// Where one ankle is, in the plane the leg swings in.
struct FootPose {
    f32 forward = 0.0f;   // ahead of the pelvis
    f32 height = kAnkleHeight;
    f32 pitch = 0.0f;     // toes up when positive
    f32 toes_flat = 0.0f; // 1 while the toes stay on the ground as the heel lifts
    bool planted = false;
};

// One leg at `phase` of its own cycle. For the first `duty` of the cycle the
// foot is on the ground, rolling from the heel over the ball to the toes while
// the body passes over it at exactly walking speed, so a planted foot does not
// slide. For the rest it swings forward to the next heel strike.
FootPose gait_foot(f32 phase, const HumanGait& gait) {
    const f32 travel = gait.duty * gait.cycle;  // distance the body covers over one planted foot
    const f32 ahead = travel * lerp(0.42f, 0.33f, gait.run), behind = travel - ahead;
    const f32 strike = lerp(rad(18.0f), rad(5.0f), gait.run);  // toes up as the heel lands
    const f32 push = lerp(rad(50.0f), rad(66.0f), gait.run);   // heel up as the toes leave

    const auto stance = [&](f32 u) {
        const f32 flat = lerp(ahead, -behind, u);  // where the ankle stands with the foot flat
        const f32 land = 0.16f, lift = lerp(0.50f, 0.36f, gait.run);
        FootPose foot;
        foot.forward = flat;
        foot.planted = true;
        if (u < land) {
            // Pivoting on the heel.
            foot.pitch = strike * (1.0f - u / land);
            const f32 c = std::cos(foot.pitch), s = std::sin(foot.pitch);
            foot.forward = flat - kHeelBack + kHeelBack * c - kAnkleHeight * s;
            foot.height = kHeelBack * s + kAnkleHeight * c;
        } else if (u > lift) {
            // Pivoting on the ball of the foot.
            foot.pitch = -push * smoothstep(0.0f, 1.0f, (u - lift) / (1.0f - lift));
            const f32 c = std::cos(foot.pitch), s = std::sin(foot.pitch);
            foot.forward = flat + kBallAhead - kBallAhead * c - kAnkleHeight * s;
            foot.height = -kBallAhead * s + kAnkleHeight * c;
            foot.toes_flat = 1.0f;
        }
        return foot;
    };

    if (phase < gait.duty) return stance(phase / gait.duty);

    const f32 t = (phase - gait.duty) / (1.0f - gait.duty);
    const FootPose from = stance(1.0f), to = stance(0.0f);
    // A runner's foot trails behind for a moment and kicks up towards the
    // thigh; a walker's skims the ground.
    const f32 reach = smoothstep(lerp(0.0f, 0.12f, gait.run), 1.0f, t);
    const f32 clearance = lerp(0.045f, 0.24f, gait.run);
    FootPose foot;
    foot.forward = lerp(from.forward, to.forward, reach);
    foot.height = lerp(from.height, to.height, reach) + clearance * std::sin(kPi * std::pow(t, lerp(1.0f, 0.72f, gait.run)));
    foot.pitch = lerp(from.pitch, to.pitch, smoothstep(0.05f, lerp(0.75f, 0.95f, gait.run), t));
    foot.toes_flat = 1.0f - smoothstep(0.0f, 0.15f, t);
    return foot;
}

// Two-bone reach from `hip` to `ankle`, bending towards `bend`. If the ankle
// is out of reach it is pulled in along the line to the hip, and the returned
// offset says by how much so the rest of the foot can follow.
Vec3 solve_leg(Vec3 hip, Vec3& ankle, Vec3 bend, Vec3& knee) {
    const f32 upper = kThighLength, lower = kShinLength;
    Vec3 line = ankle - hip;
    f32 reach = length(line);
    Vec3 shift{0, 0, 0};
    const f32 longest = (upper + lower) * 0.9985f;
    if (reach > longest) {
        const Vec3 pulled = hip + line * (longest / reach);
        shift = pulled - ankle;
        ankle = pulled;
        line = ankle - hip;
        reach = longest;
    }
    reach = std::max(reach, 0.05f);
    const Vec3 along = line / reach;
    const f32 a = (upper * upper - lower * lower + reach * reach) / (2.0f * reach);
    const f32 out = std::sqrt(std::max(upper * upper - a * a, 0.0f));
    Vec3 side = bend - along * dot(bend, along);
    side = normalize(side);
    knee = hip + along * a + side * out;
    return shift;
}

// The posed body: the public skeleton plus the frames the skin is hung on.
struct Rig {
    HumanSkeleton s;
    HumanGait gait;
    Frame pelvis, waist, chest, head;
    Frame foot[2];
    Vec3 toe_dir[2];
    Vec3 fore_dir[2], hand_dir[2];
    Vec3 thumb_dir[2];
    f32 breath = 0.0f;    // -1..1
    Vec3 swing;           // where loose things are thrown, metres in the body frame
    bool blink = false;
    f32 arm_grip = 0.0f;
};

Rig pose(const HumanLook& look, const HumanMotion& m) {
    Rig rig;
    const HumanGait gait = human_gait(m.speed);
    rig.gait = gait;
    const f32 w = m.body_activity < 0.0f ? gait.weight : saturate(m.body_activity);
    const f32 run = m.body_run < 0.0f ? gait.run : saturate(m.body_run);
    const f32 still = 1.0f - w;
    const f32 sprint = smoothstep(3.4f, 5.6f, m.speed);
    const f32 p = fract(m.phase);
    const f32 stride = std::cos(kTau * p);        // +1 as the left heel strikes
    const f32 mid = p - gait.duty * 0.5f;         // 0 when the left foot is under the body
    const f32 bw = look.build;

    rig.breath = std::sin(m.time * kTau / lerp(3.8f, 1.1f, run * w));
    // Eyes close for a moment every few seconds.
    {
        const f32 beat = m.time / 3.4f;
        const f32 offset = hash_to_unit(hash32(static_cast<u32>(std::floor(beat)) + 97u)) * 0.6f;
        rig.blink = std::fabs(fract(beat) - 0.2f - offset) < 0.02f;
    }

    // --- pelvis: rises over each planted foot when walking, drops onto it when
    // running, shifts over it, and swings the forward hip ahead.
    f32 hip_height = lerp(kHipStanding, lerp(0.900f, 0.872f, run), w);
    hip_height += w * lerp(0.018f, -0.038f, run) * std::cos(2.0f * kTau * mid);
    hip_height += still * 0.003f * rig.breath;
    const f32 shift = w * lerp(-0.022f, -0.008f, run) * std::cos(kTau * mid) + still * 0.012f * std::sin(m.time * 0.55f);
    const f32 pelvis_yaw = w * lerp(rad(6.0f), rad(11.0f), run) * stride;
    const f32 hip_drop = w * (1.0f - run) * rad(3.0f) * std::cos(kTau * mid);
    const f32 lean = w * (rad(3.0f) + rad(8.0f) * run + rad(7.0f) * sprint) + clamp(m.lag.y * 0.012f, -0.16f, 0.16f);
    const f32 bank = clamp(m.turn * m.speed * 0.010f, -0.18f, 0.18f) + clamp(m.lag.x * 0.006f, -0.08f, 0.08f);
    const f32 chest_yaw = -w * lerp(rad(7.0f), rad(14.0f), run) * stride + still * rad(3.0f) * std::sin(m.time * 0.4f);

    const Frame base;
    Frame& P = rig.pelvis;
    P = pitch(roll(yaw(base, pelvis_yaw), hip_drop + bank * 0.4f), lean * 0.35f);
    P.o = {shift, 0.0f, hip_height + 0.03f};
    Frame& W = rig.waist;
    W = pitch(roll(yaw(base, pelvis_yaw * 0.3f + chest_yaw * 0.2f), bank * 0.7f), lean * 0.7f);
    W.o = P.at(0.0f, 0.0f, 0.135f);
    Frame& C = rig.chest;
    C = pitch(roll(yaw(base, chest_yaw), bank), lean);
    C.o = W.at(0.0f, 0.0f, 0.165f);

    // --- head: held level and facing the way the body is going while the
    // shoulders swing beneath it; left to look about when standing.
    const f32 glance = still * (wander(m.time * 0.23f, 11u) - 0.5f) * 2.0f * rad(34.0f);
    const f32 nod = still * (wander(m.time * 0.17f, 23u) - 0.5f) * rad(12.0f) + rad(3.0f) * run * w;
    const Vec3 neck = C.at(0.0f, -0.012f, 0.235f);
    Frame& H = rig.head;
    H = pitch(roll(yaw(base, chest_yaw * 0.25f + glance), bank * 0.5f), lean * 0.3f + nod);
    H.o = neck + normalize(lerp(C.u, base.u, 0.5f)) * 0.160f + C.f * 0.012f;

    HumanSkeleton& s = rig.s;
    s.pelvis = P.o;
    s.waist = W.o;
    s.chest = C.o;
    s.neck = neck;
    s.head = H.o;

    // --- legs
    for (int i = 0; i < 2; ++i) {
        const f32 sign = i == 0 ? -1.0f : 1.0f;
        const FootPose step = gait_foot(fract(p + (i == 0 ? 0.0f : 0.5f)), gait);
        // Standing: feet apart and turned out, the left a little ahead.
        const Vec3 standing{sign * 0.105f * bw, -sign * 0.018f, kAnkleHeight};
        const Vec3 walking{sign * lerp(0.085f, 0.055f, gait.run) * bw, step.forward, step.height};
        Vec3 ankle = lerp(standing, walking, gait.weight);
        const f32 turnout = sign * lerp(rad(7.0f), rad(4.0f), gait.weight);

        Frame flat = yaw(base, turnout);
        Frame F = pitch(flat, -step.pitch * gait.weight);
        F.o = ankle;
        Vec3 heel = F.at(0.0f, -kHeelBack, -kAnkleHeight), ball = F.at(0.0f, kBallAhead, -kAnkleHeight);
        const Vec3 toe_dir = normalize(lerp(F.f, flat.f, lerp(1.0f, step.toes_flat, gait.weight)));
        Vec3 toe = ball + toe_dir * kToeLength;
        // Nothing goes through the ground.
        const f32 low = std::min({heel.z, ball.z, toe.z});
        if (low < 0.0f) {
            const Vec3 up{0.0f, 0.0f, -low};
            ankle += up;
            heel += up;
            ball += up;
            toe += up;
        }

        s.hip[i] = P.at(sign * kHipWidth * bw, 0.0f, -0.03f);
        // The knee points where the toes point.
        const Vec3 bend = normalize(base.f + flat.f * 0.4f + base.r * (sign * 0.06f));
        const Vec3 pulled = solve_leg(s.hip[i], ankle, bend, s.knee[i]);
        heel += pulled;
        ball += pulled;
        toe += pulled;
        F.o = ankle;

        s.ankle[i] = ankle;
        s.heel[i] = heel;
        s.ball[i] = ball;
        s.toe[i] = toe;
        s.planted[i] = gait.weight < 0.5f || step.planted;
        rig.foot[i] = F;
        rig.toe_dir[i] = toe_dir;
    }

    // Loose things lag behind a change of pace and bounce with each step.
    // Compute this before posing the carrying arm so it can leave room for
    // the satchel's outward/forward swing as well as its resting position.
    rig.swing = {clamp(-m.lag.x * 0.0035f, -0.05f, 0.05f), clamp(-m.lag.y * 0.0035f, -0.05f, 0.05f),
                 w * lerp(0.004f, 0.012f, run) * std::sin(2.0f * kTau * (mid - 0.08f))};

    // --- arms: each swings against the leg on its own side; straight and loose
    // when walking, folded and driving when running.
    const f32 arm_weight = w;
    const f32 arm_run = run;
    rig.arm_grip = arm_weight * arm_run;
    const f32 arm = std::cos(kTau * (p - 0.02f));
    for (int i = 0; i < 2; ++i) {
        const f32 sign = i == 0 ? -1.0f : 1.0f;
        const f32 beat = sign * arm;  // +1 with this arm fully forward
        const f32 theta = arm_weight * lerp(rad(0.0f), rad(6.0f), arm_run) + (1.0f - arm_weight) * rad(3.0f) + arm_weight * lerp(rad(17.0f), rad(32.0f), arm_run) * beat;
        const f32 walk_flex = rad(14.0f) + rad(3.0f) * beat;
        const f32 run_flex = rad(88.0f) + rad(6.0f) * beat;
        const f32 flex = lerp(rad(10.0f), lerp(walk_flex, run_flex, arm_run), arm_weight);
        // A bag carrier gives that hip a little extra room at a walk. Running
        // already brings the hands forward, away from the satchel behind it.
        const f32 bag_swing = std::max(-rig.swing.x, 0.0f) + 0.65f * std::max(rig.swing.y, 0.0f);
        const f32 bag_clearance = look.satchel && i == 0 ? (rad(7.0f) + bag_swing / (kUpperArmLength + kForearmLength)) * (1.0f - rig.arm_grip) : 0.0f;
        const f32 spread = lerp(rad(7.0f), lerp(rad(7.0f), rad(10.0f), arm_run), arm_weight) + bag_clearance;

        const Vec3 down = C.u * -std::cos(theta) + C.f * std::sin(theta);
        const Vec3 forward = C.f * std::cos(theta) + C.u * std::sin(theta);
        const Vec3 out = C.r * sign;
        const Vec3 upper = normalize(down * std::cos(spread) + out * std::sin(spread));
        // Bend about one anatomical hinge. Only a small inward sweep is
        // needed; independently pulling the wrist inward drove it into the
        // tunic and twisted the hand frame as the elbow passed through 90°.
        const Vec3 wanted = forward - out * (0.08f * arm_run * arm_weight);
        const Vec3 bend = normalize(wanted - upper * dot(wanted, upper));
        const Vec3 hinge = normalize(cross(upper, bend));
        const Vec3 fore = upper * std::cos(flex) + bend * std::sin(flex);
        const f32 wrist_flex = arm_weight * lerp(rad(2.0f), rad(5.0f), arm_run);
        const Vec3 hand = upper * std::cos(flex + wrist_flex) + bend * std::sin(flex + wrist_flex);
        const Vec3 thumbward = normalize(cross(hinge, hand));
        const Vec3 palmward = normalize(cross(hand, thumbward)) * -sign;
        const f32 grip = rig.arm_grip;

        s.shoulder[i] = C.at(sign * kShoulderWidth * bw, 0.0f, 0.175f + still * 0.004f * rig.breath);
        s.elbow[i] = s.shoulder[i] + upper * kUpperArmLength;
        s.wrist[i] = s.elbow[i] + fore * kForearmLength;
        const Vec3 knuckles = s.wrist[i] + hand * lerp(0.052f, 0.045f, grip);
        s.fingertip[i] = knuckles + hand * lerp(0.035f, 0.012f, grip) + palmward * lerp(0.010f, 0.028f, grip);
        const Vec3 thumb = s.wrist[i] + hand * 0.022f + thumbward * 0.023f;
        s.thumb[i] = thumb + hand * lerp(0.030f, 0.016f, grip) + palmward * 0.011f;
        rig.fore_dir[i] = fore;
        rig.hand_dir[i] = hand;
        rig.thumb_dir[i] = thumbward;
    }

    return rig;
}

// --- colour -----------------------------------------------------------------------

constexpr Color hex(u32 rgb) { return {static_cast<u8>(rgb >> 16), static_cast<u8>(rgb >> 8), static_cast<u8>(rgb), 255}; }

Color toned(Color c, f32 k) {
    const auto channel = [k](u8 v) { return static_cast<u8>(clamp(static_cast<f32>(v) * k, 0.0f, 255.0f) + 0.5f); };
    return {channel(c.r), channel(c.g), channel(c.b), 255};
}

Color blend(Color a, Color b, f32 t) {
    const auto channel = [t](u8 x, u8 y) { return static_cast<u8>(lerp(static_cast<f32>(x), static_cast<f32>(y), t) + 0.5f); };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), 255};
}

// --- skin: the parts hung on the bones ----------------------------------------------

// Surfaces that melt together share a group (see FigurePart::group).
enum Group : u8 {
    kLegLeft = 1, kLegRight, kBootLeft, kBootRight,
    kTunic, kSleeveLeft, kSleeveRight, kHandLeft, kHandRight,
    kMantle, kHead, kHair, kBeard, kFace,
    kBelt, kTrim, kStrap, kSatchel, kPouch, kKnife, kBuckle,
};

struct Shape {
    Vec3 a, b;
    f32 ra, rb;
};

// The outermost point of a set of shapes along a ray from inside them, pushed
// `lift` further out. Used to lay straps and features on a surface.
Vec3 surface_point(const Shape* shapes, int count, Vec3 origin, Vec3 dir, f32 lift) {
    const auto inside = [&](f32 t) {
        const Vec3 point = origin + dir * t;
        for (int k = 0; k < count; ++k) {
            if (round_cone_distance(point, shapes[k].a, shapes[k].b, shapes[k].ra, shapes[k].rb) <= 0.0f) return true;
        }
        return false;
    };
    constexpr f32 kStep = 0.01f;
    f32 t = 0.36f;
    while (t > 0.0f && !inside(t)) t -= kStep;
    if (t <= 0.0f) return origin + dir * lift;
    f32 lo = t, hi = t + kStep;
    for (int k = 0; k < 7; ++k) {
        const f32 middle = (lo + hi) * 0.5f;
        if (inside(middle)) lo = middle;
        else hi = middle;
    }
    return origin + dir * (lo + lift);
}

// Converts from the body's frame in metres to figure space in tiles and
// collects the parts.
class Painter {
public:
    Painter(Figure& figure, const HumanLook& look, f32 heading)
        : figure_(figure), scale_(kTilesPerMetre * look.height), forward_{std::cos(heading), std::sin(heading)}, right_{-std::sin(heading), std::cos(heading)} {}

    void cone(Vec3 a, Vec3 b, f32 ra, f32 rb, Color color, FigureMaterial material, u8 group, u8 flags = 0, f32 roundness = 1.0f) {
        if (figure_.parts.size() >= static_cast<size_t>(kMaxFigureParts)) return;
        FigurePart part;
        part.a = place(a);
        part.b = place(b);
        part.ra = ra * scale_;
        part.rb = rb * scale_;
        part.color = color;
        part.material = material;
        part.group = group;
        part.flags = static_cast<u8>(flags | (group == kFace ? FigurePartFlag::Unshadowed : 0));
        part.roundness = roundness;
        figure_.parts.push_back(part);
    }

    void ball(Vec3 centre, f32 radius, Color color, FigureMaterial material, u8 group, f32 roundness = 1.0f) {
        cone(centre, centre, radius, radius, color, material, group, 0, roundness);
    }

    // A band around the cone a..b: the stretch from t0 to t1, `extra` proud of
    // the surface and cut square.
    void band(Vec3 a, Vec3 b, f32 ra, f32 rb, f32 t0, f32 t1, f32 extra, Color color, FigureMaterial material, u8 group, f32 roundness = 1.0f) {
        cone(lerp(a, b, t0), lerp(a, b, t1), lerp(ra, rb, t0) + extra, lerp(ra, rb, t1) + extra, color, material, group, FigurePartFlag::FlatCaps, roundness);
    }

    f32 scale() const { return scale_; }

private:
    Vec3 place(Vec3 p) const {
        return {(right_.x * p.x + forward_.x * p.y) * scale_, (right_.y * p.x + forward_.y * p.y) * scale_, p.z * scale_};
    }

    Figure& figure_;
    f32 scale_;
    Vec2 forward_, right_;
};

void skin(const HumanLook& look, const HumanMotion& m, const Rig& rig, Figure& figure) {
    using Mat = FigureMaterial;
    Painter paint(figure, look, m.heading);
    const HumanSkeleton& s = rig.s;
    const Frame &P = rig.pelvis, &W = rig.waist, &C = rig.chest, &H = rig.head;
    const f32 bw = look.build;
    const f32 breath = 1.0f + 0.018f * rig.breath;
    const Vec3 swing = rig.swing;
    const Vec3 sway{swing.x, swing.y, 0.0f};

    const Color hem = toned(look.tunic, 0.70f);
    const Color strap = toned(look.leather, 0.72f);
    const Color cuff = toned(look.boots, 1.55f);

    // --- legs: hose, a garter under the knee, and turned-down boots
    for (int i = 0; i < 2; ++i) {
        const u8 leg = static_cast<u8>(kLegLeft + i), boot = static_cast<u8>(kBootLeft + i);
        const Vec3 hip = s.hip[i], knee = s.knee[i], ankle = s.ankle[i];
        paint.cone(hip, knee, 0.080f * bw, 0.058f * bw, look.hose, Mat::Cloth, leg);
        paint.cone(knee, ankle, 0.057f * bw, 0.041f * bw, look.hose, Mat::Cloth, leg);
        // The calf swells behind the shin.
        const Vec3 back = normalize(cross(ankle - knee, rig.foot[i].r));
        paint.cone(lerp(knee, ankle, 0.16f) + back * 0.014f, lerp(knee, ankle, 0.52f) + back * 0.010f, 0.050f * bw, 0.044f * bw, look.hose, Mat::Cloth, leg);
        paint.band(knee, ankle, 0.057f * bw, 0.041f * bw, 0.07f, 0.15f, 0.004f, toned(look.hood, 0.86f), Mat::Cloth, kTrim);

        const Vec3 top = lerp(knee, ankle, 0.50f);
        paint.cone(top, ankle, 0.056f * bw, 0.050f, look.boots, Mat::Leather, boot);
        paint.band(top, ankle, 0.056f * bw, 0.050f, -0.04f, 0.24f, 0.009f, cuff, Mat::Leather, boot);
        const Vec3 up = rig.foot[i].u;
        // The rear cap adds its own radius: keep its outer edge under the
        // ankle shaft instead of extending the heel behind it a second time.
        paint.cone(s.heel[i] + up * 0.030f, s.ball[i] + up * 0.037f, 0.030f, 0.038f, look.boots, Mat::Leather, boot);
        paint.cone(s.ball[i] + up * 0.037f, s.toe[i] + up * 0.031f, 0.038f, 0.031f, look.boots, Mat::Leather, boot);
    }

    // --- trunk, in two halves side by side so it is broader than it is deep.
    // Its shapes are kept so that straps and gear can be laid on the surface.
    Shape trunk[12];
    int trunk_count = 0;
    const auto body = [&](Vec3 a, Vec3 b, f32 ra, f32 rb, Color color, u8 group, f32 roundness = 0.92f) {
        paint.cone(a, b, ra, rb, color, Mat::Cloth, group, 0, roundness);
        trunk[trunk_count++] = {a, b, ra, rb};
    };
    for (int i = 0; i < 2; ++i) {
        const f32 sign = i == 0 ? -1.0f : 1.0f;
        const Vec3 seat = P.at(sign * 0.062f * bw, 0.0f, -0.01f), middle = W.at(sign * 0.058f * bw, 0.0f, 0.0f);
        body(seat, middle, 0.108f * bw, 0.100f * bw, look.tunic, kTunic);
        body(middle, C.at(sign * 0.082f * bw, 0.004f, 0.08f), 0.100f * bw, 0.112f * bw * breath, look.tunic, kTunic);
        // The belt sits on the hips.
        paint.band(seat, middle, 0.108f * bw, 0.100f * bw, 0.62f, 1.05f, 0.007f, strap, Mat::Leather, kBelt);
    }
    body(C.at(-0.125f * bw, 0.0f, 0.165f), C.at(0.125f * bw, 0.0f, 0.165f), 0.078f, 0.078f, look.tunic, kTunic);

    // The skirt of the tunic hangs from the hips and is pushed about by the thighs.
    {
        Vec3 edge[2];
        for (int i = 0; i < 2; ++i) {
            const f32 sign = i == 0 ? -1.0f : 1.0f;
            const Vec3 thigh = normalize(s.knee[i] - s.hip[i]);
            edge[i] = s.hip[i] + normalize(lerp(thigh, Vec3{0, 0, -1}, 0.35f)) * 0.335f + P.r * (sign * 0.012f) + sway * 0.25f;
        }
        // A continuous horizontal hem, even when one thigh lifts a panel.
        const f32 hem_z = (edge[0].z + edge[1].z) * 0.5f;
        for (int i = 0; i < 2; ++i) {
            const f32 sign = i == 0 ? -1.0f : 1.0f;
            edge[i].z = hem_z;
            const Vec3 top = P.at(sign * 0.068f * bw, 0.0f, 0.02f);
            paint.cone(top, edge[i], 0.110f * bw, 0.110f * bw, look.tunic, Mat::Cloth, kTunic, FigurePartFlag::FlatCaps, 0.92f);
            paint.cone(edge[i] + Vec3{0, 0, 0.035f}, edge[i], 0.114f * bw, 0.114f * bw, hem, Mat::Cloth, kTrim, FigurePartFlag::FlatCaps, 0.75f);
        }
        const Vec3 top = P.at(0.0f, 0.0f, 0.0f), bottom = lerp(edge[0], edge[1], 0.5f);
        paint.cone(top, bottom, 0.106f * bw, 0.104f * bw, look.tunic, Mat::Cloth, kTunic, FigurePartFlag::FlatCaps, 0.92f);
        paint.cone(bottom + Vec3{0, 0, 0.035f}, bottom, 0.108f * bw, 0.108f * bw, hem, Mat::Cloth, kTrim, FigurePartFlag::FlatCaps, 0.75f);
    }

    // --- hooded cape over the shoulders
    const auto mantle = [&](Vec3 a, Vec3 b, f32 ra, f32 rb) {
        paint.cone(a, b, ra, rb, look.hood, Mat::Cloth, kMantle, FigurePartFlag::FlatCaps, 0.48f);
        paint.band(a, b, ra, rb, 0.88f, 1.0f, 0.002f, toned(look.hood, 0.68f), Mat::Cloth, kMantle, 0.48f);
        trunk[trunk_count++] = {a, b, ra, rb};
    };
    for (int i = 0; i < 2; ++i) {
        const f32 sign = i == 0 ? -1.0f : 1.0f;
        mantle(C.at(sign * 0.090f, 0.0f, 0.235f), C.at(sign * 0.125f * bw, 0.0f, 0.125f), 0.100f, 0.125f * bw);
    }
    mantle(C.at(0.0f, 0.052f, 0.215f), C.at(0.0f, 0.070f * bw, 0.095f), 0.090f, 0.074f);
    mantle(C.at(0.0f, -0.052f, 0.215f), C.at(0.0f, -0.078f * bw, 0.075f), 0.095f, 0.080f);

    // --- arms
    for (int i = 0; i < 2; ++i) {
        const u8 sleeve = static_cast<u8>(kSleeveLeft + i), hand = static_cast<u8>(kHandLeft + i);
        const Vec3 elbow = s.elbow[i], wrist = s.wrist[i];
        paint.cone(s.shoulder[i], elbow, 0.056f * bw, 0.047f * bw, look.tunic, Mat::Cloth, sleeve);
        paint.cone(elbow, wrist, 0.046f * bw, 0.035f * bw, look.tunic, Mat::Cloth, sleeve);
        paint.band(elbow, wrist, 0.046f * bw, 0.035f * bw, 0.80f, 1.0f, 0.004f, look.linen, Mat::Cloth, kTrim);

        const Vec3 along = rig.hand_dir[i], thumbward = rig.thumb_dir[i];
        const f32 grip = rig.arm_grip;
        const Vec3 knuckles = wrist + along * lerp(0.052f, 0.045f, grip);
        // Relaxed fingers become a loose fist at a run, curling towards the
        // palm rather than lengthening forward with the forearm swing.
        paint.cone(wrist, knuckles, 0.027f, 0.032f, look.skin, Mat::Skin, hand);
        paint.cone(knuckles, s.fingertip[i], 0.030f, lerp(0.021f, 0.024f, grip), look.skin, Mat::Skin, hand);
        const Vec3 thumb = wrist + along * 0.022f + thumbward * 0.023f;
        paint.cone(thumb, s.thumb[i], 0.013f, 0.010f, look.skin, Mat::Skin, hand);
    }

    // --- head and neck
    paint.cone(C.at(0.0f, -0.012f, 0.215f), H.at(0.0f, -0.018f, -0.065f), 0.050f, 0.047f, look.skin, Mat::Skin, kHead);
    const Shape head[2] = {
        {H.at(0.0f, 0.0f, 0.012f), H.at(0.0f, 0.0f, 0.012f), kHeadRadius, kHeadRadius},
        {H.at(0.0f, 0.012f, 0.0f), H.at(0.0f, 0.036f, -0.078f), 0.092f, 0.058f},
    };
    for (const Shape& shape : head) paint.cone(shape.a, shape.b, shape.ra, shape.rb, look.skin, Mat::Skin, kHead);
    // A point on the face, given by the direction from the middle of the head:
    // degrees to the figure's right and degrees up.
    const Vec3 middle = H.at(0.0f, 0.0f, -0.012f);
    const auto face = [&](f32 right, f32 up, f32 lift) {
        const f32 y = rad(right), p = rad(up + 9.0f);
        const Vec3 dir = H.f * (std::cos(p) * std::cos(y)) + H.r * (std::cos(p) * std::sin(y)) + H.u * std::sin(p);
        return surface_point(head, 2, middle, dir, lift);
    };

    const Color ear = toned(look.skin, 0.93f);
    const Color eye = hex(0x66574A), lid = toned(look.skin, 0.72f);
    const Color lips = blend(toned(look.skin, 0.62f), hex(0x8E2C28), 0.35f);
    for (int i = 0; i < 2; ++i) {
        const f32 sign = i == 0 ? -1.0f : 1.0f;
        paint.cone(H.at(sign * 0.097f, -0.008f, 0.004f), H.at(sign * 0.099f, -0.012f, -0.022f), 0.019f, 0.015f, ear, Mat::Skin, kHead);
        if (rig.blink) paint.cone(face(sign * 17.0f, 9.0f, 0.003f), face(sign * 27.0f, 9.0f, 0.003f), 0.003f, 0.003f, lid, Mat::Skin, kFace);
        else paint.cone(face(sign * 19.0f, 9.0f, 0.002f), face(sign * 25.0f, 9.0f, 0.002f), 0.0060f, 0.0055f, eye, Mat::Skin, kFace);
        paint.cone(face(sign * 13.0f, 24.0f, 0.004f), face(sign * 30.0f, 21.0f, 0.004f), 0.0040f, 0.0030f, toned(look.hair, 0.90f), Mat::Hair, kFace);
    }
    paint.cone(face(0.0f, 12.0f, 0.003f), face(0.0f, -9.0f, 0.016f), 0.011f, 0.016f, look.skin, Mat::Skin, kHead);
    paint.cone(face(-9.0f, -29.0f, 0.002f), face(9.0f, -29.0f, 0.002f), 0.0038f, 0.0038f, lips, Mat::Skin, kFace);

    if (look.beard) {
        const f32 across[5] = {-64.0f, -33.0f, 0.0f, 33.0f, 64.0f}, down[5] = {-16.0f, -40.0f, -52.0f, -40.0f, -16.0f};
        const f32 thick[5] = {0.022f, 0.030f, 0.036f, 0.030f, 0.022f};
        for (int k = 0; k < 4; ++k) {
            paint.cone(face(across[k], down[k], 0.004f), face(across[k + 1], down[k + 1], 0.004f), thick[k], thick[k + 1], look.hair, Mat::Hair, kBeard);
        }
        for (const f32 sign : {-1.0f, 1.0f}) {
            paint.cone(face(0.0f, -21.0f, 0.006f), face(sign * 15.0f, -26.0f, 0.005f), 0.010f, 0.007f, look.hair, Mat::Hair, kBeard);
        }
    }

    // --- hair. A cap a little larger than the skull and set back from it meets
    // the face along a natural hairline.
    const Color hair = look.hair;
    const bool shaved = look.hair_style == HairStyle::Shaved;
    if (!look.hood_up) {
        if (shaved) paint.ball(H.at(0.0f, -0.020f, 0.026f), 0.1005f, blend(look.skin, hair, 0.55f), Mat::Skin, kHair);
        else paint.ball(H.at(0.0f, -0.024f, 0.030f), 0.107f, hair, Mat::Hair, kHair);
    }
    if (!shaved) {
        // A fringe falls over the forehead (and shows under a raised hood).
        for (const f32 across : {-31.0f, -2.0f, 28.0f}) {
            paint.cone(face(across, 52.0f, 0.012f), face(across * 1.12f, 32.0f, 0.009f), 0.025f, 0.016f, hair, Mat::Hair, kHair);
        }
    }
    if (!look.hood_up) {
        switch (look.hair_style) {
            case HairStyle::Cropped:
                paint.cone(H.at(0.0f, -0.062f, -0.012f), H.at(0.0f, -0.058f, -0.060f), 0.066f, 0.050f, hair, Mat::Hair, kHair);
                break;
            case HairStyle::Shaggy:
            case HairStyle::Long: {
                const bool is_long = look.hair_style == HairStyle::Long;
                const f32 drop = is_long ? 0.150f : 0.078f;
                for (const f32 sign : {-1.0f, 1.0f}) {
                    paint.cone(H.at(sign * 0.082f, -0.026f, 0.010f), H.at(sign * 0.086f, -0.032f, -drop) + sway * (is_long ? 0.8f : 0.3f),
                               0.043f, is_long ? 0.030f : 0.026f, hair, Mat::Hair, kHair);
                }
                paint.cone(H.at(0.0f, -0.070f, 0.0f), H.at(0.0f, -0.078f, is_long ? -0.215f : -0.110f) + sway * (is_long ? 1.2f : 0.5f),
                           0.068f, is_long ? 0.052f : 0.044f, hair, Mat::Hair, kHair);
                break;
            }
            case HairStyle::TiedBack: {
                paint.cone(H.at(0.0f, -0.062f, -0.012f), H.at(0.0f, -0.060f, -0.055f), 0.066f, 0.050f, hair, Mat::Hair, kHair);
                paint.ball(H.at(0.0f, -0.106f, 0.002f), 0.034f, hair, Mat::Hair, kHair);
                paint.band(H.at(0.0f, -0.112f, -0.010f), H.at(0.0f, -0.124f, -0.040f), 0.024f, 0.022f, 0.0f, 0.5f, 0.003f, look.hood, Mat::Cloth, kTrim);
                paint.cone(H.at(0.0f, -0.114f, -0.012f), H.at(0.0f, -0.140f, -0.165f) + sway * 1.6f + Vec3{0, 0, swing.z}, 0.029f, 0.011f, hair, Mat::Hair, kHair);
                break;
            }
            case HairStyle::Shaved:
            case HairStyle::Count: break;
        }
    }

    // --- the hood itself: worn up around the face, or thrown back on the shoulders
    if (look.hood_up) {
        paint.ball(H.at(0.0f, -0.040f, 0.012f), 0.128f, look.hood, Mat::Cloth, kMantle, 0.95f);
        paint.cone(H.at(0.0f, -0.032f, -0.070f), C.at(0.0f, -0.015f, 0.235f), 0.108f, 0.100f, look.hood, Mat::Cloth, kMantle, 0, 0.95f);
        paint.cone(H.at(0.0f, -0.092f, 0.072f), H.at(0.0f, -0.180f, 0.030f) + sway * 0.8f, 0.056f, 0.016f, look.hood, Mat::Cloth, kMantle);
    } else {
        // A roll of cloth round the back of the neck, the empty hood hanging
        // behind it, and its long tail.
        paint.cone(C.at(-0.074f, -0.062f, 0.262f), C.at(0.074f, -0.062f, 0.262f), 0.057f, 0.057f, look.hood, Mat::Cloth, kMantle);
        for (const f32 sign : {-1.0f, 1.0f}) {
            paint.cone(C.at(sign * 0.074f, -0.062f, 0.262f), C.at(sign * 0.066f, 0.034f, 0.248f), 0.057f, 0.040f, look.hood, Mat::Cloth, kMantle);
        }
        const Vec3 bag = C.at(0.0f, -0.128f, 0.105f) + sway * 0.6f;
        paint.cone(C.at(0.0f, -0.100f, 0.245f), bag, 0.070f, 0.040f, look.hood, Mat::Cloth, kMantle);
        paint.cone(bag, bag + C.u * -0.135f + C.f * -0.018f + sway * 1.8f + Vec3{0, 0, swing.z}, 0.023f, 0.011f, look.hood, Mat::Cloth, kMantle);
    }

    // --- gear on the belt
    {
        // Buckle at the front.
        const Vec3 at = surface_point(trunk, trunk_count, W.at(0.0f, 0.0f, -0.035f), W.f, 0.010f);
        paint.cone(at - W.r * 0.014f, at + W.r * 0.014f, 0.017f, 0.017f, look.metal, Mat::Metal, kBuckle);
        // The loose end of the belt hangs down from it.
        paint.cone(at + W.r * 0.022f, at + W.r * 0.030f - W.u * 0.085f + sway * 0.4f, 0.012f, 0.010f, strap, Mat::Leather, kBelt);

        // A purse on the right hip.
        const Vec3 hip = surface_point(trunk, trunk_count, W.at(0.0f, 0.0f, -0.050f), normalize(W.r * 0.50f + W.f * 0.866f), 0.022f);
        const Vec3 purse = hip - W.u * 0.060f + sway * 0.5f + Vec3{0, 0, swing.z};
        paint.cone(hip - W.u * 0.012f, purse, 0.030f, 0.042f, look.leather, Mat::Leather, kPouch);
        paint.band(hip - W.u * 0.012f, purse, 0.030f, 0.042f, -0.3f, 0.35f, 0.004f, strap, Mat::Leather, kPouch);

        // A knife at the back of the right hip, hilt up.
        const Vec3 sheath = surface_point(trunk, trunk_count, W.at(0.0f, 0.0f, -0.040f), normalize(W.r * 0.72f - W.f * 0.69f), 0.014f);
        const Vec3 point = sheath - W.u * 0.185f + W.r * 0.035f - W.f * 0.030f + sway * 0.3f;
        paint.cone(sheath + W.u * 0.010f, point, 0.017f, 0.009f, toned(look.boots, 0.9f), Mat::Leather, kKnife);
        const Vec3 pommel = sheath + W.u * 0.105f - W.r * 0.016f + W.f * 0.014f;
        paint.cone(sheath + W.u * 0.022f, pommel, 0.011f, 0.012f, hex(0xB8AC8C), Mat::Leather, kKnife);
        paint.ball(pommel, 0.016f, look.metal, Mat::Metal, kBuckle);
    }

    // --- satchel: a strap from the right shoulder to a bag on the left hip
    if (look.satchel) {
        const auto on_trunk = [&](Vec3 origin, Vec3 dir) { return surface_point(trunk, trunk_count, origin, normalize(dir), 0.005f); };
        const Vec3 shoulder = on_trunk(C.at(0.095f * bw, 0.0f, 0.120f), C.u);
        const Vec3 front[3] = {on_trunk(C.at(0.050f * bw, 0.0f, 0.105f), C.f + C.u * 0.25f), on_trunk(W.at(-0.030f * bw, 0.0f, 0.070f), W.f),
                               on_trunk(W.at(-0.060f * bw, 0.0f, -0.020f), W.f * 0.50f - W.r * 0.86f)};
        const Vec3 rear[3] = {on_trunk(C.at(0.050f * bw, 0.0f, 0.105f), C.u * 0.25f - C.f), on_trunk(W.at(-0.030f * bw, 0.0f, 0.070f), W.f * -1.0f),
                              on_trunk(W.at(-0.060f * bw, 0.0f, -0.020f), W.f * -0.50f - W.r * 0.86f)};
        const f32 r = 0.013f;
        paint.cone(shoulder, front[0], r, r, strap, Mat::Leather, kStrap);
        paint.cone(front[0], front[1], r, r, strap, Mat::Leather, kStrap);
        paint.cone(front[1], front[2], r, r, strap, Mat::Leather, kStrap);
        paint.cone(shoulder, rear[0], r, r, strap, Mat::Leather, kStrap);
        paint.cone(rear[0], rear[1], r, r, strap, Mat::Leather, kStrap);
        paint.cone(rear[1], rear[2], r, r, strap, Mat::Leather, kStrap);

        const Vec3 top = lerp(front[2], rear[2], 0.5f) - W.r * 0.030f - W.u * 0.020f - W.f * 0.190f;
        paint.cone(front[2], top, r, r, strap, Mat::Leather, kStrap);
        paint.cone(rear[2], top, r, r, strap, Mat::Leather, kStrap);
        const Vec3 bottom = top - W.u * 0.150f - W.r * 0.012f + sway + Vec3{0, 0, swing.z};
        paint.cone(top, bottom, 0.060f, 0.070f, look.leather, Mat::Leather, kSatchel, 0, 0.88f);
        paint.band(top, bottom, 0.060f, 0.070f, -0.25f, 0.42f, 0.005f, toned(look.leather, 0.82f), Mat::Leather, kSatchel);
        paint.ball(lerp(top, bottom, 0.50f) - W.r * 0.068f, 0.011f, look.metal, Mat::Metal, kBuckle);
    }

    figure.waterline = m.wade * paint.scale();
}

}  // namespace

HumanGait human_gait(f32 speed) {
    const f32 v = std::max(speed, 0.0f);
    HumanGait gait;
    // Stride lengthens with speed: a stroll, a walk, a jog, a sprint.
    const f32 speeds[] = {0.0f, 1.5f, 3.0f, 5.5f}, cycles[] = {1.10f, 1.50f, 2.25f, 3.50f};
    if (v >= speeds[3]) {
        gait.cycle = cycles[3] + (v - speeds[3]) * 0.5f;
    } else {
        int k = 0;
        while (v > speeds[k + 1]) ++k;
        gait.cycle = lerp(cycles[k], cycles[k + 1], (v - speeds[k]) / (speeds[k + 1] - speeds[k]));
    }
    gait.run = smoothstep(1.9f, 3.0f, v);
    gait.duty = lerp(0.62f, 0.40f, gait.run) - 0.09f * smoothstep(3.0f, 5.5f, v);
    gait.weight = smoothstep(0.05f, 0.55f, v);
    return gait;
}

HumanSkeleton pose_human(const HumanLook& look, const HumanMotion& motion) { return pose(look, motion).s; }

void build_human(const HumanLook& look, const HumanMotion& motion, Figure& figure) {
    figure.parts.clear();
    skin(look, motion, pose(look, motion), figure);
}

HumanLook hero_look() {
    HumanLook look;
    look.skin = hex(0xC9A27E);
    look.hair = hex(0x3C2B1F);
    look.tunic = hex(0xCBBE9C);
    look.hose = hex(0x40453A);
    look.hood = hex(0x8E2C28);
    look.boots = hex(0x45342A);
    look.leather = hex(0x7C6248);
    look.linen = hex(0xD8CFB4);
    look.metal = hex(0x9A968C);
    look.hair_style = HairStyle::Shaggy;
    return look;
}

HumanLook random_look(u64 seed) {
    static constexpr Color kSkin[] = {hex(0xD9B596), hex(0xC9A27E), hex(0xB48A66), hex(0x93694C), hex(0x6E4C39)};
    static constexpr Color kHair[] = {hex(0x2B2119), hex(0x3C2B1F), hex(0x5A4030), hex(0x8A6A3C), hex(0xA68E46), hex(0x8A857A), hex(0x70382A)};
    static constexpr Color kTunic[] = {hex(0xCBBE9C), hex(0xA68E46), hex(0x8A857A), hex(0x7C7E48), hex(0x86705A), hex(0xB09C78)};
    static constexpr Color kHose[] = {hex(0x40453A), hex(0x4A3C30), hex(0x5C5446), hex(0x33403A), hex(0x6A5646)};
    static constexpr Color kHood[] = {hex(0x8E2C28), hex(0x3A4E40), hex(0x6A5646), hex(0xA68E46), hex(0x4A443C), hex(0x7C7E48)};
    const auto pick = [](Rng& rng, const auto& list) { return list[rng.below(static_cast<u32>(std::size(list)))]; };

    Rng rng(seed);
    HumanLook look = hero_look();
    look.height = rng.range(0.93f, 1.06f);
    look.build = rng.range(0.90f, 1.12f);
    look.skin = pick(rng, kSkin);
    look.hair = pick(rng, kHair);
    look.tunic = pick(rng, kTunic);
    look.hose = pick(rng, kHose);
    look.hood = pick(rng, kHood);
    look.hair_style = static_cast<HairStyle>(rng.below(static_cast<u32>(HairStyle::Count)));
    look.beard = rng.chance(0.35f);
    look.hood_up = rng.chance(0.25f);
    look.satchel = rng.chance(0.7f);
    return look;
}

}  // namespace zl
