// Orthographic 2D camera. World space is measured in tiles with +y pointing
// down the screen, matching tile rows.
#pragma once

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

}  // namespace zl
