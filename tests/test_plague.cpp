// Headless checks for the plague doctor's animation state machine: clip
// selection, crossfades, the skinning matrices and the right-hand tool anchor.
#include <cmath>
#include <cstdio>

#include "actor/plague.h"
#include "asset/gltf.h"

using namespace zl;

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

bool finite(f32 v) { return std::isfinite(v); }

bool finite_matrix(const Mat4& m) {
    for (const f32 v : m.m) {
        if (!finite(v)) return false;
    }
    return true;
}

bool matrices_differ(const Mat4& a, const Mat4& b, f32 epsilon) {
    for (i32 i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) > epsilon) return true;
    }
    return false;
}

bool bones_differ(const PlaguePose& a, const PlaguePose& b, f32 epsilon) {
    if (a.bones.size() != b.bones.size()) return true;
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        if (matrices_differ(a.bones[i], b.bones[i], epsilon)) return true;
    }
    return false;
}

bool pose_finite(const PlaguePose& pose) {
    for (const Mat4& m : pose.globals) {
        if (!finite_matrix(m)) return false;
    }
    for (const Mat4& m : pose.bones) {
        if (!finite_matrix(m)) return false;
    }
    return finite_matrix(pose.hand) && finite_matrix(pose.left_hand);
}

f32 distance(Vec3 a, Vec3 b) { return length(a - b); }

}  // namespace

int main() {
    GltfModel model;
    std::string error;
    if (!model.load(ZL_ASSET_DIR "/models/plague_character.glb", error)) {
        std::printf("FAIL: cannot load plague_character.glb: %s\n", error.c_str());
        return 1;
    }

    PlagueAnimator animator;
    CHECK(animator.init(&model));
    CHECK(animator.ready());
    CHECK(animator.current().bones.size() == 24);
    CHECK(animator.current().globals.size() == model.nodes.size());

    for (const Mat4& m : animator.current().globals) CHECK(finite_matrix(m));
    for (const Mat4& m : animator.current().bones) CHECK(finite_matrix(m));

    const Vec3 hand = animator.current().hand.translation();
    CHECK(distance(hand, Vec3(-0.44f, 0.97f, 0.07f)) < 0.15f);

    // Idle breathing moves the skeleton between two times.
    HumanMotion idle;
    idle.speed = 0.0f;
    idle.time = 0.0f;
    animator.pose(idle);
    const PlaguePose idle_start = animator.current();
    idle.time = 2.0f;
    animator.pose(idle);
    CHECK(bones_differ(idle_start, animator.current(), 1.0e-4f));
    CHECK(animator.locomotion_clip() == PlagueClip::Idle);

    // Walking phase changes the legs.
    HumanMotion walk;
    walk.speed = 1.5f;
    walk.phase = 0.0f;
    animator.pose(walk);
    const PlaguePose walk_start = animator.current();
    walk.phase = 0.5f;
    animator.pose(walk);
    CHECK(bones_differ(walk_start, animator.current(), 1.0e-4f));
    CHECK(animator.locomotion_clip() == PlagueClip::Walking);
    CHECK(std::fabs(animator.locomotion_time() - 0.5f * 1.0333333f) < 1.0e-3f);

    // Speed selects the locomotion clip.
    HumanMotion fast;
    fast.speed = 5.2f;
    fast.phase = 0.5f;
    animator.pose(fast);
    CHECK(animator.locomotion_clip() == PlagueClip::Running);
    CHECK(std::fabs(animator.locomotion_time() - 0.5f * 0.6333333f) < 1.0e-3f);
    fast.speed = 1.5f;
    animator.pose(fast);
    CHECK(animator.locomotion_clip() == PlagueClip::Walking);
    fast.speed = 0.0f;
    animator.pose(fast);
    CHECK(animator.locomotion_clip() == PlagueClip::Idle);

    // The axe uses the authored one-handed woodcutting slash.
    HumanMotion work;
    work.speed = 0.0f;
    work.tool = HumanTool::Axe;
    work.work = 1.0f;
    work.work_phase = 0.2f;
    animator.pose(work);
    CHECK(animator.action_clip() == PlagueClip::Slash);
    CHECK(std::fabs(animator.action_weight() - 1.0f) < 1.0e-5f);
    const Vec3 axe_early = animator.current().hand.translation();
    work.work_phase = 0.8f;
    animator.pose(work);
    CHECK(animator.action_clip() == PlagueClip::Slash);
    const Vec3 axe_late = animator.current().hand.translation();
    CHECK(distance(axe_early, axe_late) > 1.0e-3f);
    CHECK(pose_finite(animator.current()));

    const Mat4 tool = animator.tool_transform(HumanTool::Axe);
    CHECK(finite_matrix(tool));
    CHECK(distance(tool.transform_point({0, -0.55f, 0}),
                   animator.current().hand.transform_point({0, 0.09f, 0})) < 1.0e-5f);
    CHECK(finite_matrix(animator.tool_transform(HumanTool::Pickaxe)));

    // Check the complete wind-up, strike and recovery. The axe's right-palm
    // socket must not depend on the free left hand; the pickaxe's shaft must
    // pass through both palms without scaling or shearing with hand spacing.
    for (const HumanTool held : {HumanTool::Axe, HumanTool::Pickaxe}) {
        for (i32 frame = 0; frame < 100; ++frame) {
            work.tool = held;
            work.work_phase = static_cast<f32>(frame) / 100.0f;
            animator.pose(work);
            CHECK(animator.action_clip() == (held == HumanTool::Axe ? PlagueClip::Slash : PlagueClip::Hammer));
            const PlaguePose& p = animator.current();
            const Mat4 attachment = animator.tool_transform(held);
            const Vec3 palm = p.hand.transform_point({0, 0.09f, 0});
            CHECK(pose_finite(p));
            CHECK(distance(attachment.transform_point({0, -0.55f, 0}), palm) < 1.0e-5f);
            const Vec3 x = attachment.transform_vector({1, 0, 0});
            const Vec3 y = attachment.transform_vector({0, 1, 0});
            const Vec3 z = attachment.transform_vector({0, 0, 1});
            const f32 scale = held == HumanTool::Axe ? 0.34f : 0.55f;
            CHECK(std::fabs(length(x) - scale) < 1.0e-5f);
            CHECK(std::fabs(length(y) - scale) < 1.0e-5f);
            CHECK(std::fabs(length(z) - scale) < 1.0e-5f);
            CHECK(std::fabs(dot(x, y)) < 1.0e-5f);
            CHECK(dot(cross(x, y), z) > 0);
            if (held == HumanTool::Pickaxe) {
                const Vec3 support = p.left_hand.transform_point({0, 0.09f, 0}) - palm;
                CHECK(length(cross(normalize(y), support)) < 1.0e-5f);
                CHECK(dot(normalize(y), support) > 0);
                CHECK(length(support) < (0.75f + 0.55f) * scale);
            } else {
                PlaguePose moved_left = p;
                moved_left.left_hand = Mat4::translation({10, 20, 30});
                CHECK(!matrices_differ(attachment, plague_tool_transform(moved_left, held), 1.0e-6f));
            }
        }
    }

    // Axe poses must come directly from the authored slash, including the
    // free arm. Compare its local arm rotation against the original keys.
    work.tool = HumanTool::Axe;
    work.work_phase = 0.30f;
    animator.pose(work);
    const GltfAnimation* slash = model.animation("Right_Hand_Sword_Slash");
    CHECK(slash != nullptr);
    CHECK(std::fabs(animator.action_time() - 0.45f) < 1.0e-5f);
    const i32 left_arm = model.node_by_name("mixamorig:LeftArm");
    CHECK(left_arm >= 0);
    if (slash && left_arm >= 0) {
        const GltfChannel* channel = slash->channel(left_arm, GltfPath::Rotation);
        CHECK(channel != nullptr);
        if (channel) {
            size_t next = 1;
            while (next + 1 < channel->times.size() && channel->times[next] < animator.action_time()) ++next;
            const f32 alpha = (animator.action_time() - channel->times[next - 1]) /
                              (channel->times[next] - channel->times[next - 1]);
            const Quat expected = quat_slerp(channel->quat[next - 1], channel->quat[next], alpha);
            const Quat actual = animator.current().rotations[(size_t)left_arm];
            const f32 alignment = expected.x * actual.x + expected.y * actual.y + expected.z * actual.z + expected.w * actual.w;
            CHECK(std::fabs(alignment) > 0.9999f);
        }
    }

    // Without a tool the sword slash plays instead.
    work.tool = HumanTool::None;
    work.work_phase = 0.5f;
    animator.pose(work);
    CHECK(animator.action_clip() == PlagueClip::Slash);
    CHECK(std::fabs(animator.action_weight() - 1.0f) < 1.0e-5f);

    // Walk for two seconds, then switch to a sprint and watch the crossfade.
    PlagueAnimator runner;
    CHECK(runner.init(&model));
    HumanMotion motion;
    motion.speed = 1.5f;
    for (i32 step = 0; step < 120; ++step) runner.update(motion, 1.0f / 60.0f);
    CHECK(runner.locomotion_clip() == PlagueClip::Walking);
    CHECK(runner.crossfade() >= 1.0f - 1.0e-5f);
    CHECK(runner.locomotion_time() >= 0.0f);
    CHECK(runner.locomotion_time() < 1.0333333f);
    const f32 before = runner.locomotion_time();
    runner.update(motion, 1.0f / 60.0f);
    CHECK(std::fabs(runner.locomotion_time() - before) > 1.0e-6f);
    CHECK(pose_finite(runner.current()));

    motion.speed = 5.2f;
    runner.update(motion, 1.0f / 60.0f);
    CHECK(runner.locomotion_clip() == PlagueClip::Running);
    CHECK(runner.crossfade() < 1.0f);
    for (i32 step = 0; step < 30; ++step) runner.update(motion, 1.0f / 60.0f);
    CHECK(runner.crossfade() >= 1.0f - 1.0e-5f);
    CHECK(pose_finite(runner.current()));

    // Clip durations, as authored in the GLB.
    struct ClipCheck {
        const char* name;
        f32 duration;
    };
    const ClipCheck clips[] = {{"Idle", 4.0f},
                               {"Walking", 1.0333333f},
                               {"Running", 0.6333333f},
                               {"Heavy_Hammer_Swing", 1.8333334f},
                               {"Right_Hand_Sword_Slash", 1.5f}};
    for (const ClipCheck& clip : clips) {
        const GltfAnimation* animation = model.animation(clip.name);
        CHECK(animation != nullptr);
        if (animation) CHECK(std::fabs(animation->duration - clip.duration) < 1.0e-3f);
    }

    // The toe's forward travel per cycle gives the clip's natural ground speed.
    // The animator scales playback by speed / reference, so these should sit
    // near the game's walk and sprint speeds.
    const i32 toe = model.node_by_name("mixamorig:LeftToeBase");
    CHECK(toe >= 0);
    if (toe >= 0) {
        for (const char* name : {"Walking", "Running"}) {
            HumanMotion stride;
            stride.speed = name[0] == 'W' ? 1.5f : 5.2f;
            f32 lo = 1.0e9f, hi = -1.0e9f;
            f32 planted_lo = 1.0e9f, planted_hi = -1.0e9f;
            i32 planted_frames = 0;
            constexpr i32 kSamples = 256;
            for (i32 i = 0; i < kSamples; ++i) {
                stride.phase = static_cast<f32>(i) / static_cast<f32>(kSamples);
                animator.pose(stride);
                const Vec3 p = animator.current().globals[static_cast<size_t>(toe)].translation();
                lo = std::min(lo, p.z);
                hi = std::max(hi, p.z);
                if (p.y < 0.12f) {
                    planted_lo = std::min(planted_lo, p.z);
                    planted_hi = std::max(planted_hi, p.z);
                    ++planted_frames;
                }
            }
            const GltfAnimation* animation = model.animation(name);
            if (!animation) continue;
            const f32 stance_travel = planted_hi - planted_lo;
            const f32 duty = static_cast<f32>(planted_frames) / static_cast<f32>(kSamples);
            const f32 natural = duty > 1.0e-3f ? stance_travel / duty / animation->duration : 0.0f;
            std::printf("  %s toe z %.3f..%.3f stance %.3f m duty %.2f, natural %.2f m/s\n", name, lo, hi,
                        stance_travel, duty, natural);
            const f32 reference = name[0] == 'W' ? kPlagueWalkReference : kPlagueRunReference;
            CHECK(std::fabs(natural - reference) < reference * 0.15f);
            CHECK(hi - lo > 0.2f);
            CHECK(stance_travel > 0.2f);
            CHECK(natural > 0.6f && natural < 9.0f);
        }
    }

    // A repeated strike blends across the wrap instead of snapping.
    PlagueAnimator chopper;
    CHECK(chopper.init(&model));
    HumanMotion chop;
    chop.speed = 0.0f;
    chop.tool = HumanTool::Axe;
    chop.work = 1.0f;
    chop.work_phase = 0.90f;
    for (i32 i = 0; i < 10; ++i) chopper.update(chop, 1.0f / 60.0f);
    const Vec3 before_wrap = chopper.current().hand.translation();
    chop.work_phase = 0.02f;
    chopper.update(chop, 1.0f / 60.0f);
    const Vec3 after_wrap = chopper.current().hand.translation();
    std::printf("  strike wrap hand jump %.3f m\n", distance(before_wrap, after_wrap));
    CHECK(distance(before_wrap, after_wrap) < 0.20f);
    CHECK(pose_finite(chopper.current()));

    std::printf("plague animator checks: %d failures\n", failures);
    return failures ? 1 : 0;
}
