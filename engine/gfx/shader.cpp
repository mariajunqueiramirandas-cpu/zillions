#include "gfx/shader.h"

#include "gfx/gl.h"

namespace zl {

namespace {

GLuint compile_stage(const char* name, GLenum type, const char* src) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        ZL_LOG("shader '%s' (%s stage) failed to compile:\n%s", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        ZL_CHECK(false, "shader compilation failed");
    }
    return shader;
}

}  // namespace

Shader::~Shader() {
    if (id_) glDeleteProgram(id_);
}

void Shader::compile(const char* name, const char* vertex_src, const char* fragment_src) {
    const GLuint vs = compile_stage(name, GL_VERTEX_SHADER, vertex_src);
    const GLuint fs = compile_stage(name, GL_FRAGMENT_SHADER, fragment_src);
    if (id_) glDeleteProgram(id_);
    id_ = glCreateProgram();
    glAttachShader(id_, vs);
    glAttachShader(id_, fs);
    glLinkProgram(id_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(id_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(id_, sizeof(log), nullptr, log);
        ZL_LOG("shader '%s' failed to link:\n%s", name, log);
        ZL_CHECK(false, "shader link failed");
    }
}

void Shader::bind() const { glUseProgram(id_); }

i32 Shader::uniform(const char* name) const { return glGetUniformLocation(id_, name); }

}  // namespace zl
