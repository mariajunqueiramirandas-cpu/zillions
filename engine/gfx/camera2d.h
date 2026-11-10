// Orthographic 2D camera. World space is measured in tiles with +y pointing
// down the screen, matching tile rows.
#pragma once

#include <algorithm>
#include <cmath>

#include "math/vec.h"

namespace zl {

struct Camera2D {
    Vec2 center{0, 0};      // world position at the middle of the viewport
    f32 zoom = 32.0f;       // pixels per world unit
    Vec2 viewport{1, 1};    // viewport size in pixels

    // Screen pixels have their origin at the top-left corner.
    Vec2 screen_to_world(Vec2 s) const { return {center.x + (s.x - viewport.x * 0.5f) / zoom, center.y + (s.y - viewport.y * 0.5f) / zoom}; }
    Vec2 world_to_screen(Vec2 w) const { return {(w.x - center.x) * zoom + viewport.x * 0.5f, (w.y - center.y) * zoom + viewport.y * 0.5f}; }

    Vec2 visible_min() const { return screen_to_world({0, 0}); }
    Vec2 visible_max() const { return screen_to_world(viewport); }

    // clip = world * scale + offset
    Vec2 clip_scale() const { return {2.0f * zoom / viewport.x, -2.0f * zoom / viewport.y}; }
    Vec2 clip_offset() const {
        const Vec2 s = clip_scale();
        return {-center.x * s.x, -center.y * s.y};
    }

    // Zooms by `factor` while keeping the world point under `screen_anchor` fixed.
    void zoom_at(Vec2 screen_anchor, f32 factor, f32 min_zoom, f32 max_zoom) {
        const Vec2 before = screen_to_world(screen_anchor);
        zoom = clamp(zoom * factor, min_zoom, max_zoom);
        const Vec2 after = screen_to_world(screen_anchor);
        center += before - after;
    }
};

// Normalised screen-space velocity [-1, 1] for city-builder border panning.
// Zero inside the safe interior or when the cursor is outside the window.
inline Vec2 edge_pan(Vec2 mouse, Vec2 viewport, bool mouse_inside, f32 margin = 36.0f) {
    if (!mouse_inside || viewport.x <= 0.0f || viewport.y <= 0.0f || margin <= 0.0f) return {0.0f, 0.0f};
    if (mouse.x < 0.0f || mouse.y < 0.0f || mouse.x > viewport.x || mouse.y > viewport.y) return {0.0f, 0.0f};
    const f32 mx = std::min(margin, viewport.x * 0.25f);
    const f32 my = std::min(margin, viewport.y * 0.25f);
    Vec2 dir{0.0f, 0.0f};
    if (mouse.x < mx) dir.x = -smoothstep(0.0f, 1.0f, (mx - mouse.x) / mx);
    else if (mouse.x > viewport.x - mx) dir.x = smoothstep(0.0f, 1.0f, (mouse.x - (viewport.x - mx)) / mx);
    if (mouse.y < my) dir.y = -smoothstep(0.0f, 1.0f, (my - mouse.y) / my);
    else if (mouse.y > viewport.y - my) dir.y = smoothstep(0.0f, 1.0f, (mouse.y - (viewport.y - my)) / my);
    const f32 len = length(dir);
    return len > 1.0f ? dir / len : dir;
}

// Smooth camera transition between character-follow mode and city-builder
// management mode: a tactical pull-back with a forward lead on release, and an
// arcing swoop-in that locks onto the moving character on return.
struct CameraModeSwitch {
    static constexpr f32 kDuration = 0.48f;

    bool active = false;
    bool zoom_active = false;
    bool to_management = false;
    f32 elapsed = 0.0f;
    Vec2 from_center{0, 0};
    Vec2 to_center{0, 0};
    f32 from_zoom = 64.0f;
    f32 to_zoom = 34.0f;

    void begin_to_management(const Camera2D& camera, Vec2 focus, Vec2 look_dir, f32 target_zoom) {
        active = true;
        zoom_active = true;
        to_management = true;
        elapsed = 0.0f;
        from_center = camera.center;
        to_center = focus + look_dir * 1.35f;
        from_zoom = camera.zoom;
        to_zoom = target_zoom;
    }

    void begin_to_character(const Camera2D& camera, f32 target_zoom) {
        active = true;
        zoom_active = true;
        to_management = false;
        elapsed = 0.0f;
        from_center = camera.center;
        to_center = camera.center;
        from_zoom = camera.zoom;
        to_zoom = target_zoom;
    }

    void cancel() { active = zoom_active = false; }
    void cancel_zoom() { zoom_active = false; }

    // Advances the transition and returns a 0..1 pulse for the ground marker ring.
    f32 step(Camera2D& camera, Vec2 live_focus, Vec2 manual_pan_delta, f32 dt, f32 min_zoom, f32 max_zoom) {
        if (!active) return 0.0f;
        if (dt > 0.0f && std::isfinite(dt)) elapsed += dt;
        if (to_management) {
            from_center += manual_pan_delta;
            to_center += manual_pan_delta;
        }
        const f32 u = saturate(elapsed / kDuration);
        const f32 inv = 1.0f - u;
        const f32 smooth = u * u * (3.0f - 2.0f * u);
        const f32 ease_out = 1.0f - inv * inv * inv;
        const Vec2 target_center = to_management ? to_center : live_focus;
        const f32 pos_t = to_management ? (1.0f - inv * inv * inv * inv) : lerp(smooth, ease_out, u);
        camera.center = lerp(from_center, target_center, pos_t);

        const f32 wave = std::sin(u * 3.14159265f);
        if (zoom_active) {
            const f32 log_from = std::log(std::max(from_zoom, 1.0e-3f));
            const f32 log_to = std::log(std::max(to_zoom, 1.0e-3f));
            const f32 dist = to_management ? 0.0f : length(live_focus - from_center);
            const f32 arc = to_management ? 0.0f : clamp(dist / 36.0f, 0.0f, 0.35f) * wave;
            const f32 zoom_t = ease_out + (to_management ? 0.04f : -0.025f) * wave * inv;
            camera.zoom = clamp(std::exp(lerp(log_from, log_to, zoom_t) - arc), min_zoom, max_zoom);
        }
        if (u >= 1.0f) {
            camera.center = target_center;
            if (zoom_active) camera.zoom = clamp(to_zoom, min_zoom, max_zoom);
            active = zoom_active = false;
            return 0.0f;
        }
        return wave * 0.85f;
    }
};

}  // namespace zl
