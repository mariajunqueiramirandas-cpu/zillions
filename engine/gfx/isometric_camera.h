// A 2:1 orthographic isometric camera. Positions stay in simulation tile
// coordinates; only the view rotates. Height is measured in tiles too.
#pragma once

#include "gfx/camera2d.h"

namespace zl {

// Share camera state with CameraModeSwitch; rendering uses plane_camera().
struct IsometricCamera : Camera2D {
    static constexpr f32 kX = 0.7071067812f;
    static constexpr f32 kY = 0.3535533906f;
    static constexpr f32 kZ = 0.8660254038f;
    static constexpr f32 kDepthXY = 0.6123724357f;

    f32 center_height = 0.0f;
    i32 rotation = 0;  // clockwise quarter turns, always normalised by turn()

    Vec2 rotate(Vec2 p) const {
        switch ((rotation % 4 + 4) % 4) {
            case 1: return {-p.y, p.x};
            case 2: return {-p.x, -p.y};
            case 3: return {p.y, -p.x};
            default: return p;
        }
    }

    Vec2 unrotate(Vec2 p) const {
        switch ((rotation % 4 + 4) % 4) {
            case 1: return {p.y, -p.x};
            case 2: return {-p.x, -p.y};
            case 3: return {-p.y, p.x};
            default: return p;
        }
    }

    Vec2 project(Vec2 world, f32 height = 0.0f) const {
        const Vec2 r = rotate(world);
        return {(r.x - r.y) * kX, (r.x + r.y) * kY - height * kZ};
    }

    Vec2 unproject(Vec2 p, f32 height = 0.0f) const {
        const f32 sum = (p.y + height * kZ) / kY;
        const f32 difference = p.x / kX;
        return unrotate({(sum + difference) * 0.5f, (sum - difference) * 0.5f});
    }

    f32 depth(Vec2 world, f32 height) const {
        const Vec2 r = rotate(world);
        return (r.x + r.y) * kDepthXY + height * 0.5f;
    }

    Vec2 world_to_screen(Vec2 world, f32 height = 0.0f) const {
        return (project(world, height) - project(center, center_height)) * zoom + viewport * 0.5f;
    }

    // Inverse on a horizontal plane. Terrain picking intersects the height
    // field instead, so the cursor also works on cliffs and mountain flanks.
    Vec2 screen_to_world(Vec2 screen, f32 height = 0.0f) const {
        return unproject((screen - viewport * 0.5f) / zoom + project(center, center_height), height);
    }

    Vec2 screen_delta_to_world(Vec2 pixels) const { return unproject(pixels / zoom); }

    void turn(i32 quarters) { rotation = ((rotation + quarters) % 4 + 4) % 4; }

    // Screen-aligned billboard coordinates use the ordinary 2D batch.
    Camera2D plane_camera() const {
        Camera2D camera;
        camera.center = project(center, center_height);
        camera.zoom = zoom;
        camera.viewport = viewport;
        return camera;
    }
};

}  // namespace zl
