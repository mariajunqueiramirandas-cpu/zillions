#include "platform/window.h"

#include "gfx/gl.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace zl {

namespace {

void on_glfw_error(int code, const char* description) { ZL_LOG("GLFW error %d: %s", code, description); }

#ifndef NDEBUG
void GLAPIENTRY on_gl_debug(GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    ZL_LOG("OpenGL %s: %s", type == GL_DEBUG_TYPE_ERROR ? "error" : "message", message);
}
#endif

}  // namespace

Window::Window(const WindowConfig& config) {
    glfwSetErrorCallback(on_glfw_error);
    ZL_CHECK(glfwInit() == GLFW_TRUE, "could not initialise GLFW (is a display available?)");

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);
    // The 2D renderer needs neither depth nor stencil.
    glfwWindowHint(GLFW_DEPTH_BITS, 0);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);
#ifndef NDEBUG
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif

    window_ = glfwCreateWindow(config.width, config.height, config.title, nullptr, nullptr);
    ZL_CHECK(window_ != nullptr, "could not create an OpenGL 3.3 window");
    glfwMakeContextCurrent(window_);
    ZL_CHECK(gladLoadGL(glfwGetProcAddress) != 0, "could not load OpenGL");
    glfwSwapInterval(config.vsync ? 1 : 0);

#ifndef NDEBUG
    if (GLAD_GL_KHR_debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(on_gl_debug, nullptr);
    }
#endif

    glfwSetWindowUserPointer(window_, this);
    glfwSetScrollCallback(window_, [](GLFWwindow* w, double, double dy) {
        static_cast<Window*>(glfwGetWindowUserPointer(w))->pending_scroll_ += static_cast<f32>(dy);
    });

    ZL_LOG("OpenGL %s on %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
}

Window::~Window() {
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

bool Window::should_close() const { return glfwWindowShouldClose(window_) != 0; }
void Window::request_close() { glfwSetWindowShouldClose(window_, GLFW_TRUE); }
void Window::swap_buffers() { glfwSwapBuffers(window_); }
void Window::set_title(const char* title) { glfwSetWindowTitle(window_, title); }

IVec2 Window::framebuffer_size() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    return {w, h};
}

void Window::poll() {
    pending_scroll_ = 0.0f;
    glfwPollEvents();

    for (int k = static_cast<int>(Key::Space); k < static_cast<int>(Key::Count); ++k) {
        const bool down = glfwGetKey(window_, k) == GLFW_PRESS;
        input_.key_pressed[k] = down && !input_.key_down[k];
        input_.key_down[k] = down;
    }
    for (int b = 0; b < static_cast<int>(MouseButton::Count); ++b) {
        input_.button_down[b] = glfwGetMouseButton(window_, b) == GLFW_PRESS;
    }

    // Cursor positions arrive in window coordinates; convert to framebuffer
    // pixels so high-DPI displays behave.
    double mx = 0, my = 0;
    int ww = 1, wh = 1;
    glfwGetCursorPos(window_, &mx, &my);
    glfwGetWindowSize(window_, &ww, &wh);
    const IVec2 fb = framebuffer_size();
    const Vec2 mouse{static_cast<f32>(mx) * static_cast<f32>(fb.x) / static_cast<f32>(ww > 0 ? ww : 1),
                     static_cast<f32>(my) * static_cast<f32>(fb.y) / static_cast<f32>(wh > 0 ? wh : 1)};
    input_.mouse_delta = have_mouse_ ? mouse - last_mouse_ : Vec2{0, 0};
    input_.mouse = mouse;
    last_mouse_ = mouse;
    have_mouse_ = true;
    input_.scroll = pending_scroll_;
}

}  // namespace zl
