// Actual GLSL: steep faces must retain detail in both surface directions.
#include <cmath>
#include <cstdio>
#include <string>

#include "gfx/gl.h"
#include "gfx/render_target.h"
#include "gfx/shader.h"
#include "gfx/texture.h"
#include "platform/window.h"
#include "render/terrain_material.h"

using namespace zl;
namespace {
int failures=0;
#define CHECK(condition) do {if(!(condition)){++failures;std::printf("FAIL line %d: %s\n",__LINE__,#condition);}} while(0)
int difference(const Image& a,const Image& b) {
    int total=0;
    for(size_t p=0;p<a.pixels.size();++p) {
        const auto x=a.pixels[p],y=b.pixels[p];
        total+=std::abs(int(x.r)-int(y.r))+std::abs(int(x.g)-int(y.g))+std::abs(int(x.b)-int(y.b));
    }
    return total;
}
}
int main() {
    WindowConfig config;config.visible=false;config.width=config.height=96;Window window(config);
    const char* vertex=R"(#version 330 core
        out vec2 vUV;
        void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);vUV=p;gl_Position=vec4(p*2.0-1.0,0,1);})";
    const std::string fragment=std::string("#version 330 core\n")+kTerrainMaterialGLSL+R"(
        uniform int uPlane,uNormals;
        uniform float uPixel;
        in vec2 vUV;
        out vec4 result;
        void main(){
            vec3 p=uPlane==0?vec3(vUV*6.4,0.37):(uPlane==1?vec3(0.37,vUV*6.4):vec3(vUV.x*6.4,0.37,vUV.y*6.4));
            vec3 N=uPlane==0?vec3(0,0,1):(uPlane==1?vec3(1,0,0):vec3(0,1,0));
            vec3 weights=rockProjectionWeights(N);
            vec3 color=uNormals==0?terrainRockColor(p,weights):terrainRockNormal(p,N,weights,uPixel)*0.5+0.5;
            result=vec4(color,1);
        })";
    Shader shader;shader.compile("rock material regression",vertex,fragment.c_str());
    Image pattern(64,64),height(64,64);
    for(i32 y=0;y<64;++y)for(i32 x=0;x<64;++x) {
        const u8 value=((x/8+y/8)%2)?200:55;
        pattern.at(x,y)={value,value,value,255};
        const auto relief=static_cast<u8>(128+70*std::sin(static_cast<f32>(x)*0.392699f)*std::cos(static_cast<f32>(y)*0.392699f));
        height.at(x,y)={relief,relief,relief,255};
    }
    std::vector<Image> layers(9,pattern);
    Texture colors,parameters,detail,flat;
    colors.create_array(layers,TextureFilter::Trilinear,TextureWrap::Repeat);
    parameters.create(Image(1,1,{255,235,0,255}),TextureFilter::Linear,TextureWrap::Repeat);
    detail.create(height,TextureFilter::Trilinear,TextureWrap::Repeat);
    flat.create(Image(1,1,{128,128,128,255}),TextureFilter::Linear,TextureWrap::Repeat);
    RenderTarget target;target.create(96,96,4);
    u32 vao=0;glGenVertexArrays(1,&vao);
    const auto render=[&](i32 plane,bool normals,const Texture& heights,f32 pixel=0.03f) {
        target.bind();glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glBindVertexArray(vao);
        shader.bind();colors.bind(0);parameters.bind(1);heights.bind(2);
        glUniform1i(shader.uniform("uGround"),0);glUniform1i(shader.uniform("uRockParameters"),1);glUniform1i(shader.uniform("uRockHeight"),2);
        glUniform1i(shader.uniform("uPlane"),plane);glUniform1i(shader.uniform("uNormals"),normals?1:0);glUniform1f(shader.uniform("uPixel"),pixel);
        glDrawArrays(GL_TRIANGLES,0,3);return target.read_pixels();
    };
    for(i32 plane=0;plane<3;++plane) {
        const Image color=render(plane,false,flat);
        int horizontal=0,vertical=0;
        for(i32 y=1;y<95;++y)for(i32 x=1;x<95;++x) {
            horizontal+=std::abs(int(color.at(x,y).r)-int(color.at(x-1,y).r));
            vertical+=std::abs(int(color.at(x,y).r)-int(color.at(x,y-1).r));
        }
        CHECK(horizontal>2000 && vertical>2000);
        const Image smooth=render(plane,true,flat),rough=render(plane,true,detail),distant=render(plane,true,detail,1.0f);
        const Color expected=plane==0?Color{128,128,255,255}:(plane==1?Color{255,128,128,255}:Color{128,255,128,255});
        for(const auto pixel:smooth.pixels)CHECK(std::abs(int(pixel.r)-int(expected.r))+std::abs(int(pixel.g)-int(expected.g))+std::abs(int(pixel.b)-int(expected.b))<=3);
        CHECK(difference(smooth,rough)>10000);
        CHECK(difference(smooth,distant)==0);
        std::printf("projection %d: %d / %d surface variation, %d normal detail\n",plane,horizontal,vertical,difference(smooth,rough));
    }
    CHECK(glGetError()==GL_NO_ERROR);glDeleteVertexArrays(1,&vao);
    std::printf("terrain material GPU checks: %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
