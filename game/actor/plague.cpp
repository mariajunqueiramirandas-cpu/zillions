// The plague doctor's animation state machine: clip selection, crossfades,
// skinning matrices and the right-hand tool anchor. Headless and deterministic.
#include "actor/plague.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace zl {
namespace {

constexpr f32 kCrossfadeSeconds = 0.18f;
constexpr f32 kActionTau = 0.12f;
constexpr f32 kIdleSpeed = 0.15f;
constexpr f32 kWalkSpeed = 2.2f;

const char* const kClipNames[(i32)PlagueClip::Count] = {
    "Idle", "Walking", "Running", "Heavy_Hammer_Swing", "Right_Hand_Sword_Slash"};

f32 wrap(f32 value, f32 period) {
    if (period <= 0.0f) return 0.0f;
    f32 result = std::fmod(value, period);
    if (result < 0.0f) result += period;
    return result;
}

f32 wrap01(f32 value) {
    f32 result = std::fmod(value, 1.0f);
    if (result < 0.0f) result += 1.0f;
    return result;
}

Quat quat_from_columns(const Mat4& m) {
    const f32 m00 = m.m[0], m10 = m.m[1], m20 = m.m[2];
    const f32 m01 = m.m[4], m11 = m.m[5], m21 = m.m[6];
    const f32 m02 = m.m[8], m12 = m.m[9], m22 = m.m[10];
    const f32 trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return quat_normalize(q);
}

void decompose(const Mat4& m, Vec3& t, Quat& r, Vec3& s) {
    t = m.translation();
    const Vec3 cx{m.m[0], m.m[1], m.m[2]};
    const Vec3 cy{m.m[4], m.m[5], m.m[6]};
    const Vec3 cz{m.m[8], m.m[9], m.m[10]};
    s = {length(cx), length(cy), length(cz)};
    Mat4 rotation = m;
    if (s.x > 1.0e-20f) {
        rotation.m[0] /= s.x;
        rotation.m[1] /= s.x;
        rotation.m[2] /= s.x;
    }
    if (s.y > 1.0e-20f) {
        rotation.m[4] /= s.y;
        rotation.m[5] /= s.y;
        rotation.m[6] /= s.y;
    }
    if (s.z > 1.0e-20f) {
        rotation.m[8] /= s.z;
        rotation.m[9] /= s.z;
        rotation.m[10] /= s.z;
    }
    r = quat_from_columns(rotation);
}

void build_globals(const GltfModel& model, const std::vector<i32>& order, const std::vector<Mat4>& locals,
                   std::vector<Mat4>& globals) {
    for (const i32 index : order) {
        const i32 parent = model.nodes[(std::size_t)index].parent;
        globals[(std::size_t)index] = parent >= 0 && (std::size_t)parent < locals.size()
                                          ? globals[(std::size_t)parent] * locals[(std::size_t)index]
                                          : locals[(std::size_t)index];
    }
}

void build_bones(const GltfSkin& skin, const std::vector<Mat4>& globals, std::vector<Mat4>& bones) {
    bones.assign(skin.joints.size(), Mat4::identity());
    const std::size_t count = skin.joints.size() < skin.inverse_bind.size() ? skin.joints.size()
                                                                           : skin.inverse_bind.size();
    for (std::size_t i = 0; i < count; ++i) {
        const i32 joint = skin.joints[i];
        if (joint >= 0 && (std::size_t)joint < globals.size()) {
            bones[i] = globals[(std::size_t)joint] * skin.inverse_bind[i];
        }
    }
}

// Drops the armature's 0.01 scale from the hand's basis, keeping its translation
// in metres so the tool anchor matches the model's world scale.
Mat4 normalized_rigid(const Mat4& m) {
    Mat4 result = m;
    for (i32 column = 0; column < 3; ++column) {
        Vec3 axis{result.m[column * 4], result.m[column * 4 + 1], result.m[column * 4 + 2]};
        const f32 len = length(axis);
        if (len > 1.0e-20f) {
            axis = axis / len;
            result.m[column * 4] = axis.x;
            result.m[column * 4 + 1] = axis.y;
            result.m[column * 4 + 2] = axis.z;
        }
    }
    return result;
}

}  // namespace

bool PlagueAnimator::init(const GltfModel* model) {
    model_ = nullptr;
    left_hand_node_ = -1;
    pose_ = PlaguePose{};
    order_.clear();
    base_translations_.clear();
    base_rotations_.clear();
    base_scales_.clear();
    locomotion_ = PlagueClip::Idle;
    previous_ = PlagueClip::Idle;
    action_ = PlagueClip::Idle;
    locomotion_time_ = 0.0f;
    previous_time_ = 0.0f;
    idle_time_ = 0.0f;
    blend_ = 1.0f;
    action_weight_ = 0.0f;
    action_time_ = 0.0f;
    previous_action_time_ = 0.0f;
    action_restart_ = 0.0f;
    last_work_phase_ = -1.0f;
    tool_ = HumanTool::None;

    if (!model || model->nodes.empty() || model->skins.empty()) return false;
    for (i32 i = 0; i < (i32)PlagueClip::Count; ++i) {
        if (!model->animation(kClipNames[i])) return false;
    }
    const i32 skin_index = model->skin_index >= 0 && (std::size_t)model->skin_index < model->skins.size()
                               ? model->skin_index
                               : 0;
    const GltfSkin& skin = model->skins[(std::size_t)skin_index];
    if (skin.joints.empty()) return false;

    model_ = model;
    left_hand_node_ = model->node_by_name("mixamorig:LeftHand");
    const std::size_t count = model->nodes.size();
    base_translations_.resize(count);
    base_rotations_.resize(count);
    base_scales_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const GltfNode& node = model->nodes[i];
        if (node.has_matrix) {
            decompose(node.matrix, base_translations_[i], base_rotations_[i], base_scales_[i]);
        } else {
            base_translations_[i] = node.translation;
            base_rotations_[i] = quat_normalize(node.rotation);
            base_scales_[i] = node.scale;
        }
    }

    // Parents before children; nodes in a broken hierarchy still get visited.
    order_.reserve(count);
    std::vector<u8> placed(count, 0);
    for (;;) {
        bool added = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (placed[i]) continue;
            const i32 parent = model->nodes[i].parent;
            if (parent < 0 || (std::size_t)parent >= count || placed[(std::size_t)parent]) {
                order_.push_back((i32)i);
                placed[i] = 1;
                added = true;
            }
        }
        if (!added) break;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!placed[i]) order_.push_back((i32)i);
    }

    pose_.translations = base_translations_;
    pose_.rotations = base_rotations_;
    pose_.scales = base_scales_;
    pose_.globals.assign(count, Mat4::identity());
    pose_.bones.assign(skin.joints.size(), Mat4::identity());

    std::vector<Mat4> locals(count);
    for (std::size_t i = 0; i < count; ++i) {
        const GltfNode& node = model->nodes[i];
        locals[i] = node.has_matrix ? node.matrix
                                    : Mat4::trs(base_translations_[i], base_rotations_[i], base_scales_[i]);
    }
    build_globals(*model_, order_, locals, pose_.globals);
    build_bones(skin, pose_.globals, pose_.bones);

    pose_.hand = Mat4::identity();
    if (model_->hand_node >= 0 && (std::size_t)model_->hand_node < count) {
        pose_.hand = normalized_rigid(pose_.globals[(std::size_t)model_->hand_node]);
    }
    pose_.has_left_hand = left_hand_node_ >= 0 && (std::size_t)left_hand_node_ < count;
    if (pose_.has_left_hand) pose_.left_hand = normalized_rigid(pose_.globals[(std::size_t)left_hand_node_]);
    pose_.valid = true;
    return true;
}

void PlagueAnimator::update(const HumanMotion& motion, f32 dt) { compute(motion, true, dt); }

void PlagueAnimator::pose(const HumanMotion& motion) { compute(motion, false, 0.0f); }

void PlagueAnimator::compute(const HumanMotion& motion, bool advance, f32 dt) {
    if (!model_ || pose_.translations.size() != model_->nodes.size()) return;
    const std::size_t count = model_->nodes.size();

    const PlagueClip wanted = motion.speed < kIdleSpeed
                                  ? PlagueClip::Idle
                                  : (motion.speed < kWalkSpeed ? PlagueClip::Walking : PlagueClip::Running);

    if (advance) {
        if (wanted != locomotion_) {
            const f32 old_duration = clip_duration(locomotion_);
            const f32 new_duration = clip_duration(wanted);
            const f32 phase = old_duration > 0.0f ? locomotion_time_ / old_duration : 0.0f;
            previous_ = locomotion_;
            previous_time_ = locomotion_time_;
            locomotion_ = wanted;
            locomotion_time_ = phase * new_duration;
            blend_ = 0.0f;
        }
        f32 rate = 1.0f;
        if (locomotion_ == PlagueClip::Walking) {
            rate = motion.speed / kPlagueWalkReference;
        } else if (locomotion_ == PlagueClip::Running) {
            rate = motion.speed / kPlagueRunReference;
        }
        rate = clamp(rate, 0.05f, 3.0f);
        locomotion_time_ = wrap(locomotion_time_ + dt * rate, clip_duration(locomotion_));
        idle_time_ += dt;
        blend_ = clamp(blend_ + dt / kCrossfadeSeconds, 0.0f, 1.0f);
        const f32 target = motion.work > 0.0f ? 1.0f : 0.0f;
        const f32 alpha = 1.0f - std::exp(-dt / kActionTau);
        action_weight_ = clamp(action_weight_ + (target - action_weight_) * alpha, 0.0f, 1.0f);
    } else {
        locomotion_ = wanted;
        previous_ = wanted;
        blend_ = 1.0f;
        const f32 duration = clip_duration(wanted);
        locomotion_time_ = wanted == PlagueClip::Idle ? wrap(motion.time, duration)
                                                      : wrap01(motion.phase) * duration;
        previous_time_ = locomotion_time_;
        action_weight_ = motion.work > 0.0f ? 1.0f : 0.0f;
    }

    // The authored slash is the axe's one-handed woodcutting action.
    const PlagueClip wanted_action = motion.tool == HumanTool::Pickaxe ? PlagueClip::Hammer : PlagueClip::Slash;
    if (wanted_action != action_ || motion.tool != tool_) action_restart_ = 0.0f;
    action_ = wanted_action;
    tool_ = motion.tool;
    const f32 phase = wrap01(motion.work_phase);
    if (advance) {
        if (motion.work > 0.0f) {
            // A new strike restarts the clip. Blend from the previous strike's
            // end pose so the loop does not snap.
            if (last_work_phase_ >= 0.0f && phase < last_work_phase_ - 0.5f) {
                previous_action_time_ = action_time_;
                action_restart_ = 1.0f;
            }
            last_work_phase_ = phase;
        } else {
            last_work_phase_ = -1.0f;
            action_restart_ = 0.0f;
        }
    } else {
        last_work_phase_ = phase;
        action_restart_ = 0.0f;
    }
    f32 begin = 0.0f;
    f32 end = 0.0f;
    action_range(motion.tool, begin, end);
    action_time_ = lerp(begin, end, phase);

    std::vector<Vec3> cur_t;
    std::vector<Quat> cur_r;
    std::vector<Vec3> cur_s;
    sample(locomotion_, locomotion_time_, cur_t, cur_r, cur_s);

    std::vector<Vec3> scratch_t;
    std::vector<Quat> scratch_r;
    std::vector<Vec3> scratch_s;
    if (blend_ < 1.0f) {
        sample(previous_, previous_time_, scratch_t, scratch_r, scratch_s);
        for (std::size_t i = 0; i < count; ++i) {
            cur_t[i] = lerp(scratch_t[i], cur_t[i], blend_);
            cur_r[i] = quat_slerp(scratch_r[i], cur_r[i], blend_);
            cur_s[i] = lerp(scratch_s[i], cur_s[i], blend_);
        }
    }
    if (action_weight_ > 0.0f) {
        sample(action_, action_time_, scratch_t, scratch_r, scratch_s);
        if (action_restart_ > 0.0f) {
            std::vector<Vec3> restart_t;
            std::vector<Quat> restart_r;
            std::vector<Vec3> restart_s;
            sample(action_, previous_action_time_, restart_t, restart_r, restart_s);
            for (std::size_t i = 0; i < count; ++i) {
                scratch_t[i] = lerp(scratch_t[i], restart_t[i], action_restart_);
                scratch_r[i] = quat_slerp(scratch_r[i], restart_r[i], action_restart_);
                scratch_s[i] = lerp(scratch_s[i], restart_s[i], action_restart_);
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            cur_t[i] = lerp(cur_t[i], scratch_t[i], action_weight_);
            cur_r[i] = quat_slerp(cur_r[i], scratch_r[i], action_weight_);
            cur_s[i] = lerp(cur_s[i], scratch_s[i], action_weight_);
        }
    }
    if (advance && action_restart_ > 0.0f) {
        action_restart_ = std::max(0.0f, action_restart_ - dt / 0.10f);
    }

    std::vector<u8> animated(count, 0);
    const PlagueClip clips[3] = {locomotion_, previous_, action_};
    for (const PlagueClip clip : clips) {
        const GltfAnimation* animation = clip_animation(clip);
        if (!animation) continue;
        for (const GltfChannel& channel : animation->channels) {
            if (channel.node >= 0 && (std::size_t)channel.node < count) animated[(std::size_t)channel.node] = 1;
        }
    }

    std::vector<Mat4> locals(count);
    for (std::size_t i = 0; i < count; ++i) {
        const GltfNode& node = model_->nodes[i];
        locals[i] = node.has_matrix && !animated[i] ? node.matrix : Mat4::trs(cur_t[i], cur_r[i], cur_s[i]);
    }

    pose_.translations = cur_t;
    pose_.rotations = cur_r;
    pose_.scales = cur_s;
    build_globals(*model_, order_, locals, pose_.globals);

    const i32 skin_index = model_->skin_index >= 0 && (std::size_t)model_->skin_index < model_->skins.size()
                               ? model_->skin_index
                               : 0;
    build_bones(model_->skins[(std::size_t)skin_index], pose_.globals, pose_.bones);

    pose_.hand = Mat4::identity();
    if (model_->hand_node >= 0 && (std::size_t)model_->hand_node < count) {
        pose_.hand = normalized_rigid(pose_.globals[(std::size_t)model_->hand_node]);
    }
    if (pose_.has_left_hand) pose_.left_hand = normalized_rigid(pose_.globals[(std::size_t)left_hand_node_]);
    pose_.valid = true;
}

void PlagueAnimator::sample(PlagueClip clip, f32 time, std::vector<Vec3>& t, std::vector<Quat>& r,
                            std::vector<Vec3>& s) const {
    t = base_translations_;
    r = base_rotations_;
    s = base_scales_;
    const GltfAnimation* animation = clip_animation(clip);
    if (!animation) return;
    const std::size_t count = t.size();
    for (const GltfChannel& channel : animation->channels) {
        if (channel.node < 0 || (std::size_t)channel.node >= count) continue;
        const std::size_t index = (std::size_t)channel.node;
        sample_channel(channel, time, &t[index], &r[index], &s[index]);
    }
}

void PlagueAnimator::sample_channel(const GltfChannel& channel, f32 time, Vec3* t, Quat* r, Vec3* s) const {
    const std::size_t count = channel.times.size();
    if (count == 0) return;
    std::size_t i0 = 0;
    std::size_t i1 = 0;
    f32 alpha = 0.0f;
    if (time <= channel.times.front()) {
        i0 = i1 = 0;
    } else if (time >= channel.times.back()) {
        i0 = i1 = count - 1;
    } else {
        std::size_t lo = 0;
        std::size_t hi = count;
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (channel.times[mid] <= time) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        i1 = lo;
        i0 = lo - 1;
        const f32 span = channel.times[i1] - channel.times[i0];
        alpha = span > 0.0f ? (time - channel.times[i0]) / span : 0.0f;
    }
    if (channel.step) {
        i1 = i0;
        alpha = 0.0f;
    }

    switch (channel.path) {
        case GltfPath::Translation:
            if (t && channel.vec3.size() > i1) *t = lerp(channel.vec3[i0], channel.vec3[i1], alpha);
            break;
        case GltfPath::Scale:
            if (s && channel.vec3.size() > i1) *s = lerp(channel.vec3[i0], channel.vec3[i1], alpha);
            break;
        case GltfPath::Rotation:
            if (r && channel.quat.size() > i1) {
                *r = quat_normalize(quat_slerp(channel.quat[i0], channel.quat[i1], alpha));
            }
            break;
    }
}

f32 PlagueAnimator::clip_duration(PlagueClip clip) const {
    const GltfAnimation* animation = clip_animation(clip);
    return animation ? animation->duration : 0.0f;
}

const GltfAnimation* PlagueAnimator::clip_animation(PlagueClip clip) const {
    const i32 index = (i32)clip;
    if (!model_ || index < 0 || index >= (i32)PlagueClip::Count) return nullptr;
    return model_->animation(kClipNames[index]);
}

void PlagueAnimator::action_range(HumanTool tool, f32& begin, f32& end) const {
    if (tool == HumanTool::Axe) {
        // Include the authored recovery, so repeated chops loop naturally.
        begin = 0.0f;
        end = clip_duration(PlagueClip::Slash);
    } else if (tool == HumanTool::Pickaxe) {
        begin = 0.25f;
        end = 1.45f;
    } else {
        begin = 0.10f;
        end = 0.55f;
    }
}

Mat4 plague_tool_transform(const PlaguePose& pose, HumanTool tool) {
    if (tool == HumanTool::None) return Mat4::identity();

    // The hand bones end at the wrists. The glove's palm sits 9 cm along
    // local +Y (measured in inverse-bind space), not at the bone origin.
    constexpr Vec3 palm_offset{0.0f, 0.09f, 0.0f};
    const Vec3 grip = pose.hand.transform_point(palm_offset);
    Vec3 shaft = pose.hand.transform_vector(normalize(Vec3{0.52f, -0.13f, -0.85f}));
    if (tool == HumanTool::Axe) shaft = pose.hand.transform_vector({0, 0, -1});
    else if (pose.has_left_hand) {
        const Vec3 between = pose.left_hand.transform_point(palm_offset) - grip;
        if (length(between) > 0.05f) shaft = normalize(between);
    }
    shaft = normalize(shaft);

    // Keep the head's cutting plane tied to the right palm while aiming the
    // handle through the support hand for the pickaxe's hammer clip. The
    // one-handed axe depends only on the right palm.
    Vec3 blade = pose.hand.transform_vector({0, 1, 0});
    blade = blade - shaft * dot(blade, shaft);
    if (length(blade) < 1.0e-4f) {
        blade = pose.hand.transform_vector({1, 0, 0});
        blade = blade - shaft * dot(blade, shaft);
    }
    blade = normalize(blade);
    const Vec3 side = normalize(cross(blade, shaft));
    Mat4 socket;
    socket.m[0] = blade.x; socket.m[1] = blade.y; socket.m[2] = blade.z;
    socket.m[4] = shaft.x; socket.m[5] = shaft.y; socket.m[6] = shaft.z;
    socket.m[8] = side.x; socket.m[9] = side.y; socket.m[10] = side.z;
    socket.m[12] = grip.x; socket.m[13] = grip.y; socket.m[14] = grip.z;

    // Both source tools have a +Y handle and the lower grip at y=-0.55.
    // The one-handed axe is ~0.65 m long; the two-handed pickaxe is ~1.05 m
    // so its support hand (0.5–0.6 m away) stays below the head.
    const f32 scale = tool == HumanTool::Axe ? 0.34f : 0.55f;
    return socket * Mat4::scale({scale, scale, scale}) * Mat4::translation({0, 0.55f, 0});
}

Mat4 PlagueAnimator::tool_transform(HumanTool tool) const {
    return plague_tool_transform(pose_, tool);
}

}  // namespace zl
