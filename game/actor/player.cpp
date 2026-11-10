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
            return true;
        }
    }
    position_ = previous_position_ = where;
    return false;
}

void Player::step(const TileMap& map, const PlayerInput& input, f32 dt) {
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
        m.heading += angle_between(m.heading, want) * (1.0f - std::exp(-dt * 13.0f));
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
    m.phase += m.speed / human_gait(m.speed).cycle * dt;
    m.phase -= std::floor(m.phase);
    m.time += dt;

    // What the body feels: its change of velocity, in its own frame.
    const Vec2 accel = (velocity_ - old_velocity) / (dt * metre);
    const Vec2 felt{dot(accel, right), dot(accel, forward)};
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
    return m;
}

}  // namespace zl
