// Shared world lighting. Art and GLB base colours are sRGB; light transport
// happens in linear space, with one display transform at the end of each pass.
// UI and packed terrain data remain untouched.
#pragma once

namespace zl {
inline constexpr const char* kSceneLightingGLSL = R"(
const float SCENE_PI = 3.14159265359;
vec3 sceneToLinear(vec3 color) {
    color=max(color,vec3(0));
    return mix(pow((color+0.055)/1.055,vec3(2.4)),color/12.92,lessThanEqual(color,vec3(0.04045)));
}
vec3 sceneToSRGB(vec3 color) {
    color=max(color,vec3(0));
    return mix(1.055*pow(color,vec3(1.0/2.4))-0.055,12.92*color,lessThanEqual(color,vec3(0.0031308)));
}
vec3 sceneDisplay(vec3 radiance) {
    // Chromaticity-preserving shoulder: scale RGB together so bright surfaces
    // retain their hue and every HDR highlight rolls off below display white.
    vec3 color=max(radiance,vec3(0))*1.08;
    color/=1.0+max(color.r,max(color.g,color.b));
    return sceneToSRGB(color);
}
vec3 sceneSky(vec3 normal) {
    float hemisphere=clamp(normal.z*0.5+0.5,0.0,1.0);
    return mix(vec3(0.11,0.105,0.09),vec3(0.60,0.68,0.80),hemisphere);
}
vec3 sceneLighting(vec3 albedo,vec3 normal,vec3 sun,vec3 view,float visibility,
                   float roughness,float metallic,float occlusion) {
    vec3 N=normalize(normal),L=normalize(sun),V=normalize(view);
    vec3 H=normalize(L+V);
    float nl=max(dot(N,L),0.0),nv=max(dot(N,V),0.001),nh=max(dot(N,H),0.0);
    roughness=clamp(roughness,0.22,1.0);metallic=clamp(metallic,0.0,1.0);
    vec3 f0=mix(vec3(0.04),albedo,metallic);
    vec3 fresnel=f0+(1.0-f0)*pow(1.0-max(dot(V,H),0.0),5.0);
    float a=roughness*roughness,a2=a*a;
    float denominator=nh*nh*(a2-1.0)+1.0;
    float distribution=a2/max(SCENE_PI*denominator*denominator,0.00001);
    float k=(roughness+1.0)*(roughness+1.0)/8.0;
    float masking=(nv/(nv*(1.0-k)+k))*(nl/(nl*(1.0-k)+k));
    vec3 specular=distribution*masking*fresnel/max(4.0*nv*nl,0.001);
    vec3 diffuse=(1.0-fresnel)*(1.0-metallic)*albedo/SCENE_PI;
    vec3 direct=(diffuse+specular)*vec3(4.4,4.05,3.65)*nl*clamp(visibility,0.0,1.0);
    vec3 ambient=albedo*(1.0-metallic)*sceneSky(N);
    vec3 reflection=sceneSky(reflect(-V,N))*f0*mix(0.55,0.22,roughness);
    return direct+(ambient+reflection)*clamp(occlusion,0.0,1.0);
}
vec3 sceneViewDirection(mat2 rotation) {
    return normalize(vec3(transpose(rotation)*vec2(0.6123724357),0.5));
}
// Painted fallback art already carries face shading. Apply external shadows
// in the same linear space without lighting the picture a second time.
vec3 scenePaintedShadow(vec3 color,float visibility) {
    return sceneToSRGB(sceneToLinear(color)*mix(vec3(0.34,0.39,0.47),vec3(1),visibility));
}
)";
} // namespace zl
