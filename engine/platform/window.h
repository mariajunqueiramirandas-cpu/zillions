// Window, OpenGL context and input. The only place that talks to the OS.
#pragma once

#include "core/base.h"
#include "math/vec.h"

struct GLFWwindow;

namespace zl {

// Values match GLFW key codes.
enum class Key : i32 {
    Space = 32,
    Minus = 45,
    Equal = 61,
    Num1 = 49, Num2, Num3, Num4, Num5,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Escape = 256,
    Enter = 257,
    Tab = 258,
    Right = 262, Left, Down, Up,
    F1 = 290, F2, F3, F4, F5, F6,
    LeftShift = 340,
    LeftControl = 341,
    Count = 349,
};

enum class MouseButton : i32 { Left = 0, Right = 1, Middle = 2, Count = 3 };

struct Input {
    Vec2 mouse{0, 0};        // cursor in framebuffer pixels, origin top-left
    Vec2 mouse_delta{0, 0};  // movement since the previous frame
    f32 scroll = 0.0f;       // wheel movement this frame
    bool mouse_inside = false;  // cursor is inside the focused/hovered window
    bool key_down[static_cast<int>(Key::Count)] = {};
    bool key_pressed[static_cast<int>(Key::Count)] = {};  // went down this frame
    bool button_down[static_cast<int>(MouseButton::Count)] = {};
    bool button_pressed[static_cast<int>(MouseButton::Count)] = {};

    bool down(Key k) const { return key_down[static_cast<int>(k)]; }
    bool pressed(Key k) const { return key_pressed[static_cast<int>(k)]; }
    bool down(MouseButton b) const { return button_down[static_cast<int>(b)]; }
    bool pressed(MouseButton b) const { return button_pressed[static_cast<int>(b)]; }
};

struct WindowConfig {
    const char* title = "Zillions";
    i32 width = 1600;
    i32 height = 900;
    bool visible = true;   // false: off-screen context only (screenshots, tools)
    bool vsync = true;
};

class Window {
public:
    explicit Window(const WindowConfig& config);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool should_close() const;
    void request_close();

    // Gathers OS events and refreshes input(). Call once per frame.
    void poll();
    void swap_buffers();

    const Input& input() const { return input_; }
    IVec2 framebuffer_size() const;
    void set_title(const char* title);

private:
    GLFWwindow* window_ = nullptr;
    Input input_;
    Vec2 last_mouse_{0, 0};
    bool have_mouse_ = false;
    bool mouse_moved_ = false;
    f32 pending_scroll_ = 0.0f;
    bool pending_key_pressed_[static_cast<int>(Key::Count)] = {};
    bool pending_button_pressed_[static_cast<int>(MouseButton::Count)] = {};
};

}  // namespace zl
