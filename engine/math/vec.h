// Small 2D and 3D math types. Deliberately plain structs: trivially copyable and
// friendly to tight loops and GPU uploads.
#pragma once

#include <cmath>

#include "core/base.h"

namespace zl {

struct Vec2 {
    f32 x = 0, y = 0;

    constexpr Vec2() = default;
    constexpr Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(f32 s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(f32 s) const { return {x / s, y / s}; }
    constexpr Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    constexpr Vec2& operator*=(f32 s) { x *= s; y *= s; return *this; }
};

struct Vec3 {
    f32 x = 0, y = 0, z = 0;

    constexpr Vec3() = default;
    constexpr Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(f32 s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(f32 s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};

struct IVec2 {
    i32 x = 0, y = 0;
    constexpr bool operator==(const IVec2&) const = default;
};

constexpr f32 dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline f32 length(Vec2 v) { return std::sqrt(dot(v, v)); }

constexpr f32 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline f32 length(Vec3 v) { return std::sqrt(dot(v, v)); }
// Unit vector along `v`; a zero vector comes back unchanged.
inline Vec3 normalize(Vec3 v) {
    const f32 len = length(v);
    return len > 0.0f ? v / len : v;
}

template <class T>
constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

constexpr f32 saturate(f32 v) { return clamp(v, 0.0f, 1.0f); }
constexpr f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
constexpr Vec2 lerp(Vec2 a, Vec2 b, f32 t) { return a + (b - a) * t; }
constexpr Vec3 lerp(Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }

constexpr f32 smoothstep(f32 edge0, f32 edge1, f32 x) {
    const f32 t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace zl
