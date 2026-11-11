// Quaternions and 4x4 column-major matrices for the glTF skinning pipeline.
// Kept plain and header-only, like vec.h: the simulation stores them and the
// renderer uploads them without a conversion layer.
#pragma once

#include <cmath>

#include "core/base.h"
#include "math/vec.h"

namespace zl {

struct Quat {
    f32 x = 0, y = 0, z = 0, w = 1;

    constexpr Quat() = default;
    constexpr Quat(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
};

inline Quat quat_normalize(Quat q) {
    const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 1.0e-20f) return {};
    const f32 inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

inline Quat quat_mul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat quat_from_axis_angle(Vec3 axis, f32 radians) {
    const f32 len = length(axis);
    if (len <= 1.0e-20f) return {};
    const f32 half = radians * 0.5f;
    const f32 s = std::sin(half) / len;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

inline Vec3 quat_rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

// Shortest-arc spherical interpolation. Nearly parallel inputs use a
// normalised linear blend, which is stable and cheaper.
inline Quat quat_slerp(Quat a, Quat b, f32 t) {
    f32 dot_ab = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot_ab < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        dot_ab = -dot_ab;
    }
    if (dot_ab > 0.9995f) {
        return quat_normalize({lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)});
    }
    const f32 angle = std::acos(clamp(dot_ab, -1.0f, 1.0f));
    const f32 sin_angle = std::sin(angle);
    const f32 wa = std::sin((1.0f - t) * angle) / sin_angle;
    const f32 wb = std::sin(t * angle) / sin_angle;
    return quat_normalize({a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}

// Column-major: m[column * 4 + row]. Matches glUniformMatrix4fv without a
// transpose and the matrices stored in glTF accessors.
struct Mat4 {
    f32 m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 identity() { return {}; }

    static Mat4 translation(Vec3 t) {
        Mat4 r;
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static Mat4 scale(Vec3 s) {
        Mat4 r;
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        return r;
    }

    static Mat4 rotation(Quat q) {
        const f32 x = q.x, y = q.y, z = q.z, w = q.w;
        const f32 xx = x * x, yy = y * y, zz = z * z;
        const f32 xy = x * y, xz = x * z, yz = y * z;
        const f32 wx = w * x, wy = w * y, wz = w * z;
        Mat4 r;
        r.m[0] = 1.0f - 2.0f * (yy + zz);
        r.m[1] = 2.0f * (xy + wz);
        r.m[2] = 2.0f * (xz - wy);
        r.m[4] = 2.0f * (xy - wz);
        r.m[5] = 1.0f - 2.0f * (xx + zz);
        r.m[6] = 2.0f * (yz + wx);
        r.m[8] = 2.0f * (xz + wy);
        r.m[9] = 2.0f * (yz - wx);
        r.m[10] = 1.0f - 2.0f * (xx + yy);
        return r;
    }

    static Mat4 rotation_x(f32 radians) { return rotation(quat_from_axis_angle({1, 0, 0}, radians)); }
    static Mat4 rotation_y(f32 radians) { return rotation(quat_from_axis_angle({0, 1, 0}, radians)); }
    static Mat4 rotation_z(f32 radians) { return rotation(quat_from_axis_angle({0, 0, 1}, radians)); }

    static Mat4 trs(Vec3 t, Quat r, Vec3 s) { return translation(t) * rotation(r) * scale(s); }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (i32 c = 0; c < 4; ++c) {
            for (i32 row = 0; row < 4; ++row) {
                f32 sum = 0.0f;
                for (i32 k = 0; k < 4; ++k) sum += m[k * 4 + row] * o.m[c * 4 + k];
                r.m[c * 4 + row] = sum;
            }
        }
        return r;
    }

    Vec3 transform_point(Vec3 p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }

    Vec3 transform_vector(Vec3 v) const {
        return {m[0] * v.x + m[4] * v.y + m[8] * v.z,
                m[1] * v.x + m[5] * v.y + m[9] * v.z,
                m[2] * v.x + m[6] * v.y + m[10] * v.z};
    }

    Vec3 translation() const { return {m[12], m[13], m[14]}; }

    // Inverse of an affine transform (rotation, uniform or non-uniform scale,
    // translation). Used by tests and by callers that need world-to-local.
    Mat4 inverse_affine() const {
        const f32 a = m[0], b = m[4], c = m[8];
        const f32 d = m[1], e = m[5], f = m[9];
        const f32 g = m[2], h = m[6], i = m[10];
        const f32 det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        Mat4 r;
        if (std::abs(det) <= 1.0e-20f) return r;
        const f32 inv = 1.0f / det;
        r.m[0] = (e * i - f * h) * inv;
        r.m[4] = (c * h - b * i) * inv;
        r.m[8] = (b * f - c * e) * inv;
        r.m[1] = (f * g - d * i) * inv;
        r.m[5] = (a * i - c * g) * inv;
        r.m[9] = (c * d - a * f) * inv;
        r.m[2] = (d * h - e * g) * inv;
        r.m[6] = (b * g - a * h) * inv;
        r.m[10] = (a * e - b * d) * inv;
        const Vec3 t = translation();
        r.m[12] = -(r.m[0] * t.x + r.m[4] * t.y + r.m[8] * t.z);
        r.m[13] = -(r.m[1] * t.x + r.m[5] * t.y + r.m[9] * t.z);
        r.m[14] = -(r.m[2] * t.x + r.m[6] * t.y + r.m[10] * t.z);
        return r;
    }
};

}  // namespace zl
