#include "actor/player.h"

#include <algorithm>
#include <cmath>

namespace zl {

namespace {

constexpr f32 kTau = 2.0f * kPi;

// The signed shortest way round from angle `from` to angle `to`.
f32 angle_between(f32 from, f32 to) {
    f32 d = std::fmod(to - from, kTau);
    if (d > kPi) d -= kTau;
    if (d < -kPi) d += kTau;
    return d;
}

}  // namespace

bool can_stand(const TileMap& map, Vec2 position, f32 radius) {
    const i32 cx = static_cast<i32>(std::floor(position.x)), cy = static_cast<i32>(std::floor(position.y));
    if (!map.in_bounds(cx, cy) || !map.walkable(cx, cy)) return false;
    const i32 centre = map.index(cx, cy);
    const i32 x0 = static_cast<i32>(std::floor(position.x - radius)), x1 = static_cast<i32>(std::floor(position.x + radius));
    const i32 y0 = static_cast<i32>(std::floor(position.y - radius)), y1 = static_cast<i32>(std::floor(position.y + radius));
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            if (x == cx && y == cy) continue;
            // Does the body reach into this tile at all?
            const f32 nx = clamp(position.x, static_cast<f32>(x), static_cast<f32>(x + 1));
            const f32 ny = clamp(position.y, static_cast<f32>(y), static_cast<f32>(y + 1));
            const f32 dx = position.x - nx, dy = position.y - ny;
            if (dx * dx + dy * dy >= radius * radius) continue;
            if (!map.in_bounds(x, y)) return false;
            // can_step() speaks of neighbours; the same rule (both walkable,
            // and level with each other unless one is a ramp) is what keeps a
            // body from hanging over a cliff edge at a corner.
            if (!map.can_step(centre, map.index(x, y))) return false;
        }
    }
    return true;
}

bool Player::spawn(const TileMap& map) {
    return spawn(map, {static_cast<f32>(map.start.x) + 0.5f, static_cast<f32>(map.start.y) + 0.5f});
}

bool Player::spawn(const TileMap& map, Vec2 where) {
    velocity_ = drive_ = {0.0f, 0.0f};
    motion_ = previous_motion_ = HumanMotion{};
    const i32 cx = clamp(static_cast<i32>(std::floor(where.x)), 0, std::max(map.width - 1, 0));
    const i32 cy = clamp(static_cast<i32>(std::floor(where.y)), 0, std::max(map.height - 1, 0));
    if (can_stand(map, where, kRadius)) {
        position_ = previous_position_ = where;
        motion_.wade = previous_motion_.wade = map.terrain[static_cast<size_t>(map.index(cx, cy))] == Terrain::Shallows ? kWadeDepth : 0.0f;
        return true;
    }
    // Otherwise the nearest tile with room, searching outwards in rings.
    const i32 reach = std::max(map.width, map.height);
    for (i32 ring = 0; ring <= reach; ++ring) {
        f32 best = 1.0e30f;
        Vec2 found{0.0f, 0.0f};
        for (i32 y = cy - ring; y <= cy + ring; ++y) {
            for (i32 x = cx - ring; x <= cx + ring; ++x) {
                if (std::max(std::abs(x - cx), std::abs(y - cy)) != ring) continue;
                const Vec2 centre{static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f};
                if (!can_stand(map, centre, kRadius)) continue;
                const Vec2 d = centre - where;
                if (dot(d, d) < best) {
                    best = dot(d, d);
                    found = centre;
                }
            }
        }
        if (best < 1.0e30f) {
            position_ = previous_position_ = found;
            const i32 tile = map.index(static_cast<i32>(found.x), static_cast<i32>(found.y));
            motion_.wade = previous_motion_.wade = map.terrain[static_cast<size_t>(tile)] == Terrain::Shallows ? kWadeDepth : 0.0f;
            return true;
        }
    }
    position_ = previous_position_ = where;
    return false;
}

void Player::step(const TileMap& map, const PlayerInput& input, f32 dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;
    previous_position_ = position_;
    previous_motion_ = motion_;

    const f32 metre = kTilesPerMetre * look.height;  // tiles per metre of this body
    const i32 tx = static_cast<i32>(std::floor(position_.x)), ty = static_cast<i32>(std::floor(position_.y));
    const bool wading = map.in_bounds(tx, ty) && map.terrain[static_cast<size_t>(map.index(tx, ty))] == Terrain::Shallows;

    // --- steering: ease towards the wanted velocity, quicker to stop than to start
    Vec2 wish = input.move;
    const f32 wish_length = length(wish);
    const bool moving = wish_length > 1.0e-3f;
    f32 pace = input.sprint ? kSprintSpeed : (input.walk ? kWalkSpeed : kJogSpeed);
    if (wading) pace = std::min(pace, kJogSpeed) * kWadeSlowdown;
    const Vec2 target = moving ? wish * (pace * metre / wish_length) : Vec2{0.0f, 0.0f};
    drive_ += (target - drive_) * (1.0f - std::exp(-dt * (moving ? 9.0f : 13.0f)));

    // --- movement: in short hops, sliding along whatever is in the way
    const Vec2 before = position_;
    const Vec2 travel = drive_ * dt;
    const int hops = std::max(1, static_cast<int>(std::ceil(length(travel) / 0.08f)));
    const Vec2 hop = travel / static_cast<f32>(hops);
    for (int k = 0; k < hops; ++k) {
        const Vec2 both = position_ + hop;
        const Vec2 across{position_.x + hop.x, position_.y}, down{position_.x, position_.y + hop.y};
        if (can_stand(map, both, kRadius)) position_ = both;
        else if (hop.x != 0.0f && can_stand(map, across, kRadius)) position_ = across;
        else if (hop.y != 0.0f && can_stand(map, down, kRadius)) position_ = down;
        else break;
    }
    const Vec2 old_velocity = velocity_;
    velocity_ = (position_ - before) / dt;

    // --- facing: turn towards where the player is steering
    HumanMotion& m = motion_;
    const f32 old_heading = m.heading;
    if (moving) {
        const f32 want = std::atan2(wish.y, wish.x);
        const f32 limit = lerp(3.0f * kPi, 2.0f * kPi, human_gait(pace).run) * dt;
        m.heading += clamp(angle_between(m.heading, want) * (1.0f - std::exp(-dt * 13.0f)), -limit, limit);
        m.heading = std::fmod(m.heading, kTau);
    }
    const f32 turn = angle_between(old_heading, m.heading) / dt;
    m.turn += (turn - m.turn) * (1.0f - std::exp(-dt * 10.0f));

    // --- gait: driven by the ground actually covered, so a body pushing
    // against a cliff marks time instead of running on the spot
    const Vec2 forward{std::cos(m.heading), std::sin(m.heading)}, right{-forward.y, forward.x};
    const f32 ground_speed = std::max(dot(velocity_, forward), 0.0f) / metre;
    // The figure is drawn foreshortened front to back while the ground is not,
    // so a stride towards or away from the viewer looks shorter than the
    // ground it covers. Quickening the step a little splits the difference.
    const f32 toward = forward.y * forward.y;
    m.speed = ground_speed * (1.0f + 0.28f * toward);
    // Smooth torso and arm transitions without smoothing the travelled
    // distance that drives the feet. This also lets a blocked player settle.
    const HumanGait previous_gait = human_gait(previous_motion_.speed);
    const f32 previous_activity = previous_motion_.body_activity < 0.0f ? previous_gait.weight : previous_motion_.body_activity;
    const f32 previous_run = previous_motion_.body_run < 0.0f ? previous_gait.run : previous_motion_.body_run;
    m.body_activity = lerp(previous_activity, human_gait(m.speed).weight, 1.0f - std::exp(-dt * 6.0f));
    m.body_run = lerp(previous_run, moving ? human_gait(pace).run : 0.0f, 1.0f - std::exp(-dt * 6.0f));
    m.phase += m.speed / human_gait(m.speed).cycle * dt;
    m.phase -= std::floor(m.phase);
    m.time += dt;

    // What the body feels: its change of velocity, in its own frame.
    const Vec2 accel = (velocity_ - old_velocity) / (dt * metre);
    Vec2 felt{dot(accel, right), dot(accel, forward)};
    // A blocked movement step can report an arbitrarily large impulse. Let
    // collision stop the body immediately, but keep visual lean/gear inertia
    // within a human acceleration range instead of snapping the rig upright.
    const f32 felt_length = length(felt);
    if (felt_length > 10.0f) felt = felt * (10.0f / felt_length);
    m.lag += (felt - m.lag) * (1.0f - std::exp(-dt * 7.0f));

    m.wade += ((wading ? kWadeDepth : 0.0f) - m.wade) * (1.0f - std::exp(-dt * 8.0f));
    if (m.wade < 1.0e-3f) m.wade = 0.0f;
}

Vec2 Player::position_at(f32 alpha) const { return lerp(previous_position_, position_, alpha); }

HumanMotion Player::motion_at(f32 alpha) const {
    const HumanMotion &a = previous_motion_, &b = motion_;
    HumanMotion m;
    m.heading = a.heading + angle_between(a.heading, b.heading) * alpha;
    m.speed = lerp(a.speed, b.speed, alpha);
    // The phase wraps from 1 back to 0; go the short way round.
    f32 step = b.phase - a.phase;
    if (step < -0.5f) step += 1.0f;
    if (step > 0.5f) step -= 1.0f;
    m.phase = a.phase + step * alpha;
    m.phase -= std::floor(m.phase);
    m.time = lerp(a.time, b.time, alpha);
    m.turn = lerp(a.turn, b.turn, alpha);
    m.lag = lerp(a.lag, b.lag, alpha);
    m.wade = lerp(a.wade, b.wade, alpha);
    const auto pose_blend = [alpha](f32 x, f32 y, f32 fallback_x, f32 fallback_y) {
        return x < 0.0f && y < 0.0f ? -1.0f : lerp(x < 0.0f ? fallback_x : x, y < 0.0f ? fallback_y : y, alpha);
    };
    const HumanGait ga = human_gait(a.speed), gb = human_gait(b.speed);
    m.body_activity = pose_blend(a.body_activity, b.body_activity, ga.weight, gb.weight);
    m.body_run = pose_blend(a.body_run, b.body_run, ga.run, gb.run);
    return m;
}

namespace {

bool clear_line(const TileMap& map, Vec2 from, Vec2 to, f32 radius) {
    if (!can_stand(map, to, radius)) return false;
    const Vec2 delta = to - from;
    const f32 dist = length(delta);
    const int steps = std::max(1, static_cast<int>(std::ceil(dist / 0.32f)));
    for (int i = 1; i < steps; ++i) {
        const Vec2 p = from + delta * (static_cast<f32>(i) / static_cast<f32>(steps));
        if (!can_stand(map, p, radius)) return false;
    }
    return true;
}

f32 waypoint_interest(const TileMap& map, Vec2 pos) {
    const i32 tx = static_cast<i32>(std::floor(pos.x)), ty = static_cast<i32>(std::floor(pos.y));
    if (!map.in_bounds(tx, ty)) return 0.0f;
    const size_t centre = static_cast<size_t>(map.index(tx, ty));
    f32 bonus = centre < map.terrain.size() && map.terrain[centre] != Terrain::Shallows ? 0.45f : 0.0f;
    const i32 neighbours[4][2] = {{tx + 1, ty}, {tx - 1, ty}, {tx, ty - 1}, {tx, ty + 1}};
    for (const auto& n : neighbours) {
        if (!map.in_bounds(n[0], n[1])) continue;
        const size_t idx = static_cast<size_t>(map.index(n[0], n[1]));
        const Terrain t = idx < map.terrain.size() ? map.terrain[idx] : Terrain::Grass;
        const bool has_resource = idx < map.resource.size() && map.resource[idx] != Resource::None;
        const bool is_ramp = idx < map.flags.size() && (map.flags[idx] & TileFlag::Ramp) != 0;
        if (t == Terrain::Forest || t == Terrain::Rocks || has_resource || is_ramp) {
            bonus += 0.35f;
            break;
        }
    }
    return bonus;
}

}  // namespace

void PlayerAutonomy::reset(Vec2 anchor, u64 seed) {
    rng_.reseed(mix64(seed ^ 0xA5704F991C3E6B2Dull));
    anchor_ = target_ = anchor;
    glance_dir_ = {0.0f, 1.0f};
    timer_ = rng_.range(0.25f, 0.55f);
    leg_limit_ = 5.0f;
    glance_time_ = -1.0f;
    curve_ = 0.0f;
    blocked_time_ = 0.0f;
    pace_ = 0;
    chain_left_ = 0;
    moving_ = false;
    initialized_ = true;
}

bool PlayerAutonomy::pick_waypoint(const TileMap& map, const Player& player, bool continuation) {
    const Vec2 pos = player.position();
    if (length(pos - anchor_) > kWanderRadius * 1.6f) anchor_ = pos;
    const Vec2 facing{std::cos(player.motion().heading), std::sin(player.motion().heading)};

    f32 best_score = -1.0e30f;
    Vec2 best = pos;
    bool found = false;
    for (int i = 0; i < 14; ++i) {
        const f32 angle = rng_.range(0.0f, kTau);
        const Vec2 dir{std::cos(angle), std::sin(angle)};
        const Vec2 origin = i < 10 ? anchor_ : pos;
        const f32 radius = i < 10 ? rng_.range(1.6f, kWanderRadius) : rng_.range(1.1f, 3.0f);
        const Vec2 cand = origin + dir * radius;
        const f32 dist = length(cand - pos);
        if (dist < 1.0f || length(cand - anchor_) > kWanderRadius + 0.5f) continue;
        if (!clear_line(map, pos, cand, Player::kRadius)) continue;

        const Vec2 step_dir = (cand - pos) / dist;
        const f32 align = dot(step_dir, facing);
        if (continuation && align < -0.15f) continue;
        const f32 score = 1.0f + rng_.range(0.0f, 0.45f) + align * (continuation ? 0.85f : 0.30f) + waypoint_interest(map, cand);
        if (score > best_score) {
            best_score = score;
            best = cand;
            found = true;
        }
    }
    if (!found) return false;

    target_ = best;
    timer_ = 0.0f;
    blocked_time_ = 0.0f;
    const f32 d = length(target_ - pos);
    leg_limit_ = clamp(d * 1.8f + 1.5f, 2.5f, 8.0f);
    curve_ = rng_.range(-0.26f, 0.26f);
    if (!continuation) {
        chain_left_ = (d < 4.8f && rng_.chance(0.45f)) ? 1 : 0;
        if (d > 4.2f) pace_ = rng_.chance(0.25f) ? 2 : 1;
        else pace_ = rng_.chance(0.35f) ? 1 : 0;
    }
    return true;
}

PlayerInput PlayerAutonomy::step(const TileMap& map, const Player& player, f32 dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return {};
    if (!initialized_) reset(player.position(), map.seed);

    if (!moving_) {
        timer_ -= dt;
        if (glance_time_ > 0.0f && timer_ <= glance_time_ && timer_ > glance_time_ - 0.12f) {
            return {glance_dir_, false, true};
        }
        if (timer_ > 0.0f) return {};
        if (!pick_waypoint(map, player, false)) {
            timer_ = 0.8f;
            glance_time_ = -1.0f;
            return {};
        }
        moving_ = true;
    }

    timer_ += dt;
    const Vec2 pos = player.position();
    Vec2 to_target = target_ - pos;
    f32 dist = length(to_target);

    if (chain_left_ > 0 && dist < 0.85f) {
        --chain_left_;
        if (pick_waypoint(map, player, true)) {
            to_target = target_ - pos;
            dist = length(to_target);
        }
    }

    if (dist < 0.28f || timer_ >= leg_limit_) {
        moving_ = false;
        timer_ = rng_.range(0.85f, 2.5f);
        if (timer_ > 1.25f && rng_.chance(0.55f)) {
            glance_time_ = timer_ * rng_.range(0.40f, 0.65f);
            const f32 angle = player.motion().heading + rng_.range(-1.3f, 1.3f);
            glance_dir_ = {std::cos(angle), std::sin(angle)};
        } else {
            glance_time_ = -1.0f;
        }
        return {};
    }

    if (timer_ > 0.25f && length(player.velocity()) < 0.15f) {
        blocked_time_ += dt;
        if (blocked_time_ > 0.35f) {
            if (!pick_waypoint(map, player, true)) {
                moving_ = false;
                timer_ = rng_.range(0.5f, 1.2f);
                glance_time_ = -1.0f;
                return {};
            }
            to_target = target_ - pos;
            dist = length(to_target);
        }
    } else {
        blocked_time_ = std::max(0.0f, blocked_time_ - dt * 2.0f);
    }

    f32 angle = std::atan2(to_target.y, to_target.x) + curve_ * clamp(dist / 3.5f, 0.0f, 1.0f);
    Vec2 steer{std::cos(angle), std::sin(angle)};
    if (dist > 0.6f && !can_stand(map, pos + steer * 0.55f, Player::kRadius)) {
        for (f32 offset : {0.45f, -0.45f, 0.90f, -0.90f, 1.35f, -1.35f}) {
            const Vec2 alt{std::cos(angle + offset), std::sin(angle + offset)};
            if (can_stand(map, pos + alt * 0.55f, Player::kRadius)) {
                steer = alt;
                break;
            }
        }
    }

    PlayerInput input;
    input.move = steer;
    if (pace_ == 2 && timer_ < 0.55f && dist > 2.2f) input.sprint = true;
    else if (pace_ >= 1 && dist > 1.15f) input.walk = false;
    else input.walk = true;
    return input;
}

}  // namespace zl
