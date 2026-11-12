// World-aligned rock material shared by the terrain shader and GPU regression.
#pragma once
namespace zl {
inline constexpr const char* kTerrainMaterialGLSL=R"(
uniform sampler2DArray uGround;
uniform sampler2D uRockParameters;
uniform sampler2D uRockHeight;

vec3 rockProjectionWeights(vec3 normal) {
    vec3 weight=pow(abs(normal),vec3(4.0));
    return weight/max(dot(weight,vec3(1.0)),0.00001);
}
vec3 rockSampleColor(vec3 q,vec3 weight) {
    return texture(uGround,vec3(q.yz,6)).rgb*weight.x
         + texture(uGround,vec3(q.xz+vec2(0.37,0.13),6)).rgb*weight.y
         + texture(uGround,vec3(q.xy+vec2(0.63,0.45),6)).rgb*weight.z;
}
vec3 rockSampleData(sampler2D source,vec3 q,vec3 weight) {
    return texture(source,q.yz).rgb*weight.x
         + texture(source,q.xz+vec2(0.37,0.13)).rgb*weight.y
         + texture(source,q.xy+vec2(0.63,0.45)).rgb*weight.z;
}
vec3 terrainRockColor(vec3 position,vec3 weight) {
    // Four-metre fractures plus larger mineral variation. Projection includes
    // vertical height, so steep faces retain the same texel density as ledges.
    vec3 color=mix(rockSampleColor(position/3.2,weight),rockSampleColor(position/19.0+7.3,weight),0.18);
    float grey=dot(color,vec3(0.2126,0.7152,0.0722));
    return mix(color,vec3(grey),0.72)*vec3(0.98,1.0,1.025);
}
vec3 terrainRockNormal(vec3 position,vec3 normal,vec3 weight,float pixelSize) {
    float detail=rockSampleData(uRockHeight,position/3.2,weight).r;
    float height=detail*0.14*(1.0-smoothstep(0.15,0.65,pixelSize));
    vec3 dx=dFdx(position),dy=dFdy(position);
    vec3 rx=cross(dy,normal),ry=cross(normal,dx);
    float determinant=dot(dx,rx);
    if(abs(determinant)<0.0000001)return normal;
    // Surface-gradient bump mapping: flat displacement leaves the geometric
    // normal intact, including on vertical walls and camera quarter turns.
    vec3 gradient=sign(determinant)*(dFdx(height)*rx+dFdy(height)*ry);
    return normalize(abs(determinant)*normal-gradient);
}
)";
} // namespace zl
