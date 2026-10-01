// RTSky - minimal vector math (CPU side)
#pragma once

#include <cmath>

namespace rtsky {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegToRad = kPi / 180.0f;

struct float3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr float3() = default;
    constexpr float3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr float3 operator+(const float3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    constexpr float3 operator-(const float3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    constexpr float3 operator*(float s) const { return { x * s, y * s, z * s }; }
    constexpr float3 operator-() const { return { -x, -y, -z }; }
    float3& operator+=(const float3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};

constexpr float Dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr float3 Cross(const float3& a, const float3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float Length(const float3& v) { return std::sqrt(Dot(v, v)); }
inline float3 Normalize(const float3& v)
{
    float len = Length(v);
    return len > 1e-20f ? v * (1.0f / len) : float3(0.0f, 0.0f, 1.0f);
}

template <typename T>
constexpr T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr float Saturate(float v) { return Clamp(v, 0.0f, 1.0f); }
constexpr float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Camera basis in GTA V world space (right-handed, Z up). The camera looks along its local +Y;
// local +X is right and +Z is up. rotationDeg = GET_FINAL_RENDERED_CAM_ROT(2): x = pitch, y = roll,
// z = yaw, applied as R = Rz(yaw) * Rx(pitch) * Ry(roll).
struct CameraBasis
{
    float3 right;
    float3 forward;
    float3 up;
};

inline CameraBasis CameraBasisFromRotation(const float3& rotationDeg)
{
    const float p = rotationDeg.x * kDegToRad;
    const float r = rotationDeg.y * kDegToRad;
    const float y = rotationDeg.z * kDegToRad;
    const float sp = std::sin(p), cp = std::cos(p);
    const float sr = std::sin(r), cr = std::cos(r);
    const float sy = std::sin(y), cy = std::cos(y);

    // Columns of Rz(y) * Rx(p) * Ry(r)
    CameraBasis b;
    b.right = { cy * cr - sy * sp * sr, sy * cr + cy * sp * sr, -cp * sr };
    b.forward = { -sy * cp, cy * cp, sp };
    b.up = { cy * sr + sy * sp * cr, sy * sr - cy * sp * cr, cp * cr };
    return b;
}

// Angle between two unit vectors, in degrees
inline float AngleBetweenDeg(const float3& a, const float3& b)
{
    return std::acos(Clamp(Dot(a, b), -1.0f, 1.0f)) / kDegToRad;
}

} // namespace rtsky
