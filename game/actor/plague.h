// The plague doctor's animation state machine. It turns the player's motion
// (speed, heading, tool work) into blended local bone transforms, skinning
// matrices and the right-hand transform the tool meshes hang from. Headless
// and deterministic, so the loader and the clips can be tested without a GPU.
#pragma once

#include <vector>

#include "actor/human.h"
#include "asset/gltf.h"
#include "math/mat.h"

namespace zl {

enum class PlagueClip : u8 { Idle, Walking, Running, Hammer, Slash, Count };

// Ground speeds at which the authored locomotion clips move their feet without
// sliding. Measured from the toe's planted travel per cycle (see
// tests/test_plague.cpp) and used to scale playback with the player's speed.
inline constexpr f32 kPlagueWalkReference = 1.32f;
inline constexpr f32 kPlagueRunReference = 4.73f;

struct PlaguePose {
    std::vector<Vec3> translations;  // per node, model space
    std::vector<Quat> rotations;
    std::vector<Vec3> scales;
    std::vector<Mat4> globals;       // per node, model space
    std::vector<Mat4> bones;         // per skin joint: global * inverse bind
    Mat4 hand;                       // right hand, rotation normalised, metres
    Mat4 left_hand;                  // support hand, same space as hand
    bool has_left_hand = false;
    bool valid = false;
};

// Raw tool GLB -> character space. Shared by the renderer and animator so
// sockets, shaft alignment, authored grip and scale have one implementation.
Mat4 plague_tool_transform(const PlaguePose& pose, HumanTool tool);

class PlagueAnimator {
public:
    bool init(const GltfModel* model);

    // Advances the clips by one fixed step and rebuilds the pose.
    void update(const HumanMotion& motion, f32 dt);

    // Rebuilds the pose at a fixed state without advancing time. Screenshots
    // use the motion's phase for the locomotion clip and work_phase for the
    // action clip.
    void pose(const HumanMotion& motion);

    const PlaguePose& current() const { return pose_; }
    const GltfModel* model() const { return model_; }
    bool ready() const { return model_ != nullptr && pose_.valid; }

    // Maps the raw tool model to the animated palms, including grip and scale.
    Mat4 tool_transform(HumanTool tool) const;

    // Debug and test accessors.
    PlagueClip locomotion_clip() const { return locomotion_; }
    PlagueClip action_clip() const { return action_; }
    f32 locomotion_time() const { return locomotion_time_; }
    f32 action_time() const { return action_time_; }
    f32 action_weight() const { return action_weight_; }
    f32 crossfade() const { return blend_; }

private:
    void compute(const HumanMotion& motion, bool advance, f32 dt);
    void sample(PlagueClip clip, f32 time, std::vector<Vec3>& t, std::vector<Quat>& r, std::vector<Vec3>& s) const;
    void sample_channel(const GltfChannel& channel, f32 time, Vec3* t, Quat* r, Vec3* s) const;
    f32 clip_duration(PlagueClip clip) const;
    const GltfAnimation* clip_animation(PlagueClip clip) const;
    void action_range(HumanTool tool, f32& begin, f32& end) const;

    const GltfModel* model_ = nullptr;
    PlaguePose pose_;
    std::vector<i32> order_;  // parents before children
    std::vector<Vec3> base_translations_;
    std::vector<Quat> base_rotations_;
    std::vector<Vec3> base_scales_;
    i32 left_hand_node_ = -1;

    PlagueClip locomotion_ = PlagueClip::Idle;
    PlagueClip previous_ = PlagueClip::Idle;
    PlagueClip action_ = PlagueClip::Idle;
    f32 locomotion_time_ = 0.0f;
    f32 previous_time_ = 0.0f;
    f32 idle_time_ = 0.0f;
    f32 blend_ = 1.0f;          // previous -> locomotion crossfade
    f32 action_weight_ = 0.0f;  // action clip over locomotion
    f32 action_time_ = 0.0f;
    f32 previous_action_time_ = 0.0f;  // pose at the end of the previous strike
    f32 action_restart_ = 0.0f;        // 1 -> 0 blend across a strike wrap
    f32 last_work_phase_ = -1.0f;
    HumanTool tool_ = HumanTool::None;
};

}  // namespace zl
