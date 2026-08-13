#pragma once

#include <cmath>

namespace bl1gotyvr::player {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float xValue, float yValue, float zValue)
        : x(xValue), y(yValue), z(zValue) {}
    Vec3 operator+(const Vec3& other) const {
        return {x + other.x, y + other.y, z + other.z};
    }
    Vec3 operator-(const Vec3& other) const {
        return {x - other.x, y - other.y, z - other.z};
    }
    Vec3 operator*(float scale) const { return {x * scale, y * scale, z * scale}; }
    float lengthSq() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(lengthSq()); }
    Vec3 normalized() const {
        const float magnitude = length();
        return magnitude > 1.0e-6f
            ? Vec3{x / magnitude, y / magnitude, z / magnitude}
            : Vec3{};
    }
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    Quat() = default;
    Quat(float xValue, float yValue, float zValue, float wValue)
        : x(xValue), y(yValue), z(zValue), w(wValue) {}
    Quat normalized() const {
        const float magnitude = std::sqrt(x * x + y * y + z * z + w * w);
        return magnitude > 1.0e-6f
            ? Quat{x / magnitude, y / magnitude, z / magnitude, w / magnitude}
            : Quat{};
    }
    Quat conjugate() const { return {-x, -y, -z, w}; }
};

struct TwoBoneIKInput {
    Vec3 shoulder;
    Vec3 handTarget;
    Quat handTargetRotation;
    Vec3 poleHint{0.0f, 0.0f, 1.0f};
    float upperArmLength = 0.0f;
    float forearmLength = 0.0f;
    float reachEpsilon = 0.001f;
    bool useHandRotation = false;
};

struct TwoBoneIKResult {
    Vec3 elbow;
    Quat upperArmRotation;
    Quat forearmRotation;
    Quat handRotation;
    bool valid = false;
};

Quat QuatFromAxisAngle(const Vec3& axis, float radians);
Quat QuatMultiply(const Quat& left, const Quat& right);
Quat QuatLookAt(const Vec3& forward, const Vec3& up);
Vec3 RotateByQuat(const Quat& rotation, const Vec3& vector);
TwoBoneIKResult SolveTwoBoneIK(const TwoBoneIKInput& input);
bool RunTwoBoneIKSelfTest();

} // namespace bl1gotyvr::player
