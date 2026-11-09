// GLSL program wrapper.
#pragma once

#include "core/base.h"

namespace zl {

class Shader {
public:
    Shader() = default;
    ~Shader();
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    // Compiles and links; aborts with the driver log on failure, since a broken
    // built-in shader is a programming error.
    void compile(const char* name, const char* vertex_src, const char* fragment_src);

    void bind() const;
    i32 uniform(const char* name) const;
    u32 id() const { return id_; }

private:
    u32 id_ = 0;
};

}  // namespace zl
