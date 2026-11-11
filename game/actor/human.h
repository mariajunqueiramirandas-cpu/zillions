// The human character, built in code: a jointed skeleton, a gait that carries
// it from standing through walking to a sprint, and the body, clothes and face
// laid over the bones as figure parts. Nothing here is drawn or loaded from a
// file, and nothing touches the GPU.
#pragma once

#include "actor/figure.h"
#include "core/base.h"
#include "core/image.h"
#include "math/vec.h"

namespace zl {

inline constexpr f32 kPi = 3.14159265f;

// A person is modelled in metres and stands 1.75 m tall. Like the units of any
// strategy game they are drawn larger than life, so a figure can be read
// against trees that are two tiles high.
inline constexpr f32 kTilesPerMetre = 0.80f;

enum class HairStyle : u8 {
    Cropped,   // short all over
    Shaggy,    // over the ears and down to the collar
    Long,      // to the shoulders
    TiedBack,  // drawn into a tail at the nape
    Shaved,    // stubble
    Count
};

// Everything that makes one person look different from another.
struct HumanLook {
    f32 height = 1.0f;  // scale of the whole body; 1 is 1.75 m
    f32 build = 1.0f;   // breadth of the trunk and limbs

    Color skin, hair;
    Color tunic, hose, hood;   // wool tunic, legwear, hooded shoulder cape
    Color boots, leather;      // boots; belt, straps and satchel
    Color linen, metal;        // shirt cuffs; buckles

    HairStyle hair_style = HairStyle::Shaggy;
    bool beard = false;
    bool hood_up = false;
    bool satchel = true;
};

// The player's character: undyed linen and a hood the colour of dried blood,
// the one colour on the map that the eye goes to first.
HumanLook hero_look();
// A different person for every seed, in clothes dyed from the same palette.
HumanLook random_look(u64 seed);

// Everything that changes from one frame to the next.
enum class HumanTool : u8 { None, Axe, Pickaxe };
struct HumanMotion {
    f32 heading = kPi * 0.5f;  // radians; 0 faces east, pi/2 south (towards the viewer)
    f32 speed = 0.0f;          // metres per second along the heading, at the body's own scale
    f32 phase = 0.0f;          // position in the gait cycle, 0..1; the left heel strikes at 0
    f32 time = 0.0f;           // seconds alive: breathing, blinking, glancing about
    f32 turn = 0.0f;           // radians per second; the body banks into a turn
    // Acceleration felt in the body's frame (x to its right, y forward), in
    // m/s^2, smoothed. Loose things swing against it and the trunk leans with it.
    Vec2 lag{0.0f, 0.0f};
    f32 wade = 0.0f;           // depth of the water stood in, metres
    // Player-driven torso/arm blends settle independently of ground speed. Negative
    // values derive them from speed, for standalone poses and review sheets.
    f32 body_activity = -1.0f;
    f32 body_run = -1.0f;
    HumanTool tool = HumanTool::None;
    f32 work = 0.0f;
    f32 work_phase = 0.0f;
};

// How the legs move at a given speed.
struct HumanGait {
    f32 cycle = 1.0f;   // metres covered by one full cycle (two steps)
    f32 duty = 0.6f;    // fraction of the cycle a foot spends on the ground
    f32 run = 0.0f;     // 0 walking, 1 running (both feet leave the ground)
    f32 weight = 0.0f;  // 0 standing still, 1 fully in the gait
};
HumanGait human_gait(f32 speed);

// Bone lengths in metres.
inline constexpr f32 kThighLength = 0.43f;
inline constexpr f32 kShinLength = 0.43f;
inline constexpr f32 kUpperArmLength = 0.28f;
inline constexpr f32 kForearmLength = 0.25f;

// Joint positions in the body's own frame: metres, x to the figure's right,
// y forward, z up, origin on the ground beneath the pelvis. Index 0 is the
// left limb and 1 the right.
struct HumanSkeleton {
    Vec3 pelvis, waist, chest, neck, head;
    Vec3 hip[2], knee[2], ankle[2];
    Vec3 heel[2], ball[2], toe[2];  // points on the sole
    Vec3 shoulder[2], elbow[2], wrist[2], fingertip[2], thumb[2];
    bool planted[2] = {true, true};  // the foot is bearing weight
};

// Poses the skeleton without building anything to draw.
HumanSkeleton pose_human(const HumanLook& look, const HumanMotion& motion);

// Poses the body and writes its parts into `figure`, replacing what was there.
void build_human(const HumanLook& look, const HumanMotion& motion, Figure& figure);

}  // namespace zl
