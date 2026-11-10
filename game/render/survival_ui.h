#pragma once

#include "gfx/sprite_batch.h"
#include "platform/window.h"
#include "survival/survival.h"

namespace zl {

// An immediate-mode HUD drawn in framebuffer pixels. Gameplay remains in the
// headless Survival model; all mouse hit boxes share the drawing's UI scale.
class SurvivalUI {
public:
    bool open = false;
    bool placing = false;
    int chest = -1;
    Stack hand;
    void init();
    bool close(Survival& game);
    bool blocks_world(Vec2 mouse, Vec2 viewport) const;
    void update(const Input& input, Vec2 viewport, Survival& game);
    void draw(Vec2 viewport, const Input& input, const Survival& game, const char* prompt, bool follow);
    void marker(Vec2 screen, Vec2 viewport, bool valid, float progress = 0);
    void impact(Vec2 screen, Vec2 viewport, Item item, float age);
private:
    Texture atlas_;
    SpriteBatch batch_;
    std::vector<SpriteInstance> sprites_;
    float scale_ = 1;
    Vec2 mouse_;
    void rect(float x, float y, float w, float h, u32 color);
    void text(float x, float y, const std::string& value, u32 color, float size = 2);
    void icon(float x, float y, Item item, float size = 30);
    bool hit(float x, float y, float w, float h) const;
    void flush(Vec2 viewport);
};

} // namespace zl
