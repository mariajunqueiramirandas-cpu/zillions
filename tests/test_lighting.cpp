// Exercise the actual shared GPU lighting, including HDR and material response.
#include <cstdio>
#include <string>

#include "gfx/gl.h"
#include "gfx/lighting.h"
#include "gfx/render_target.h"
#include "gfx/shader.h"
#include "platform/window.h"

using namespace zl;
int main() {
    WindowConfig config;config.visible=false;config.width=config.height=32;Window window(config);
    Shader shader;
    const char* vertex=R"(#version 330 core
    void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);})";
    const std::string fragment=std::string("#version 330 core\n")+kSceneLightingGLSL+R"(
    uniform vec3 uColor;
    uniform float uVisibility,uRoughness,uMetallic,uHDR;
    out vec4 result;
    void main(){
        vec3 L=normalize(vec3(-0.90,-0.35,1.0));
        vec3 color=uHDR>=0.0?vec3(uHDR):sceneLighting(sceneToLinear(uColor),L,L,L,uVisibility,uRoughness,uMetallic,1.0);
        result=vec4(sceneDisplay(color),1);
    })";
    shader.compile("lighting regression",vertex,fragment.c_str());
    RenderTarget target;target.create(32,32);u32 vao=0;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    int failed=0;
    const auto check=[&](bool value,const char* name){if(!value){++failed;std::printf("FAIL: %s\n",name);}};
    const auto sample=[&](Vec3 color,f32 visibility,f32 roughness=0.9f,f32 metallic=0.0f,f32 hdr=-1.0f){
        target.bind();shader.bind();glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);
        glUniform3f(shader.uniform("uColor"),color.x,color.y,color.z);
        glUniform1f(shader.uniform("uVisibility"),visibility);glUniform1f(shader.uniform("uRoughness"),roughness);
        glUniform1f(shader.uniform("uMetallic"),metallic);glUniform1f(shader.uniform("uHDR"),hdr);
        glDrawArrays(GL_TRIANGLES,0,3);return target.read_pixels().at(16,16);
    };
    const Color lit=sample({0.45f,0.45f,0.45f},1),shade=sample({0.45f,0.45f,0.45f},0);
    check(lit.g>shade.g+20,"sun shadow has visible contrast");
    check(shade.g>50,"sky keeps shadow detail readable");
    check(lit.r<235 && lit.b>lit.r-40,"neutral materials stay neutral instead of bleaching brown");
    const Color red=sample({0.65f,0.08f,0.05f},1);
    check(red.r>red.g+60 && red.r>red.b+60,"surface pigment retains saturation");
    const Color rough=sample({0.2f,0.2f,0.2f},1,0.9f),smooth=sample({0.2f,0.2f,0.2f},1,0.25f);
    check(smooth.r>rough.r+15,"roughness controls the highlight instead of a universal plastic gloss");
    const Color mid=sample({},1,1,0,0.5f),bright=sample({},1,1,0,2),hdr=sample({},1,1,0,8);
    check(mid.r<bright.r && bright.r<hdr.r && hdr.r<250,"HDR rolloff is monotonic without clipping white");
    const Color black=sample({0.0f,0.0f,0.0f},0);
    check(black.r<40 && black.g<40,"dark material does not turn into chalk");
    check(glGetError()==GL_NO_ERROR,"valid GPU lighting state");
    glDeleteVertexArrays(1,&vao);
    std::printf("lighting GPU checks: %s\n",failed?"FAIL":"PASS");return failed?1:0;
}
