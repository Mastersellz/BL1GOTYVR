#include "TwoBoneIK.hpp"

#include <algorithm>
#include <cmath>

namespace bl1gotyvr::player {

namespace {

bool IsFinite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

bool IsFinite(const Quat& value) {
    const float norm = value.x*value.x + value.y*value.y +
        value.z*value.z + value.w*value.w;
    return std::isfinite(norm) && norm > 0.5f && norm < 1.5f;
}

Vec3 Cross(const Vec3& left, const Vec3& right) {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

float Dot(const Vec3& left, const Vec3& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

} // namespace

Quat QuatFromAxisAngle(const Vec3& axis, float radians) {
    const Vec3 normalizedAxis = axis.normalized();
    const float halfAngle = radians * 0.5f;
    const float sine = std::sin(halfAngle);
    return {normalizedAxis.x * sine, normalizedAxis.y * sine,
            normalizedAxis.z * sine, std::cos(halfAngle)};
}

Quat QuatMultiply(const Quat& left, const Quat& right) {
    return {
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
    };
}

Vec3 RotateByQuat(const Quat& rotation, const Vec3& vector) {
    const Quat normalized = rotation.normalized();
    const Quat point{vector.x, vector.y, vector.z, 0.0f};
    const Quat result = QuatMultiply(QuatMultiply(normalized, point),
                                     normalized.conjugate());
    return {result.x, result.y, result.z};
}

Quat QuatLookAt(const Vec3& forward, const Vec3& up) {
    Vec3 x = forward.normalized();
    if (x.lengthSq() < 1.0e-6f) x = {1.0f, 0.0f, 0.0f};
    Vec3 z = Cross(x, up).normalized();
    if (z.lengthSq() < 1.0e-6f)
        z = Cross(x, std::fabs(x.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0}).normalized();
    const Vec3 y = Cross(z, x).normalized();

    const float m00 = x.x, m01 = y.x, m02 = z.x;
    const float m10 = x.y, m11 = y.y, m12 = z.y;
    const float m20 = x.z, m21 = y.z, m22 = z.z;
    const float trace = m00 + m11 + m22;
    Quat result;
    if (trace > 0.0f) {
        const float scale = std::sqrt(trace + 1.0f) * 2.0f;
        result = {(m21 - m12) / scale, (m02 - m20) / scale,
                  (m10 - m01) / scale, 0.25f * scale};
    } else if (m00 > m11 && m00 > m22) {
        const float scale = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        result = {0.25f * scale, (m01 + m10) / scale,
                  (m02 + m20) / scale, (m21 - m12) / scale};
    } else if (m11 > m22) {
        const float scale = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        result = {(m01 + m10) / scale, 0.25f * scale,
                  (m12 + m21) / scale, (m02 - m20) / scale};
    } else {
        const float scale = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        result = {(m02 + m20) / scale, (m12 + m21) / scale,
                  0.25f * scale, (m10 - m01) / scale};
    }
    return result.normalized();
}

TwoBoneIKResult SolveTwoBoneIK(const TwoBoneIKInput& input) {
    TwoBoneIKResult result;
    if (!IsFinite(input.shoulder) || !IsFinite(input.handTarget) ||
        !IsFinite(input.poleHint) || !std::isfinite(input.upperArmLength) ||
        !std::isfinite(input.forearmLength) || !std::isfinite(input.reachEpsilon) ||
        input.reachEpsilon < 0.0f || input.upperArmLength <= 1.0e-5f ||
        input.forearmLength <= 1.0e-5f) {
        return result;
    }

    Vec3 toTarget = input.handTarget - input.shoulder;
    float distance = toTarget.length();
    Vec3 direction = toTarget.normalized();
    if (direction.lengthSq() < 1.0e-6f) {
        direction = (input.poleHint - input.shoulder).normalized();
        if (direction.lengthSq() < 1.0e-6f) direction = {1, 0, 0};
    }

    const float minimumReach = std::fabs(input.upperArmLength - input.forearmLength) +
        input.reachEpsilon;
    const float maximumReach = input.upperArmLength + input.forearmLength -
        input.reachEpsilon;
    if (minimumReach > maximumReach) return result;
    distance = std::clamp(distance, minimumReach, maximumReach);
    const Vec3 clampedTarget = input.shoulder + direction * distance;

    Vec3 poleDirection = input.poleHint - input.shoulder;
    poleDirection = poleDirection - direction * Dot(poleDirection, direction);
    if (poleDirection.lengthSq() < 1.0e-8f) {
        const Vec3 fallback = std::fabs(direction.z) < 0.9f
            ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        poleDirection = Cross(direction, fallback);
    }
    poleDirection = poleDirection.normalized();

    const float along = (input.upperArmLength * input.upperArmLength +
        distance * distance - input.forearmLength * input.forearmLength) /
        (2.0f * distance);
    const float height = std::sqrt(std::max(0.0f,
        input.upperArmLength * input.upperArmLength - along * along));
    result.elbow = input.shoulder + direction * along + poleDirection * height;
    const Vec3 upperDirection = (result.elbow - input.shoulder).normalized();
    const Vec3 lowerDirection = (clampedTarget - result.elbow).normalized();
    result.upperArmRotation = QuatLookAt(upperDirection, poleDirection);
    result.forearmRotation = QuatLookAt(lowerDirection, direction);
    result.handRotation = input.useHandRotation
        ? input.handTargetRotation.normalized() : result.forearmRotation;
    result.valid = IsFinite(result.elbow) && IsFinite(result.upperArmRotation) &&
        IsFinite(result.forearmRotation) && IsFinite(result.handRotation);
    return result;
}

bool RunTwoBoneIKSelfTest() {
    TwoBoneIKInput input;
    input.shoulder = {0, 0, 0};
    input.handTarget = {1, 1, 0};
    input.poleHint = {0, 0, 1};
    input.upperArmLength = 1.0f;
    input.forearmLength = 1.0f;
    const TwoBoneIKResult result = SolveTwoBoneIK(input);
    if (!result.valid) return false;
    const float upperError = std::fabs((result.elbow - input.shoulder).length() - 1.0f);
    const float lowerError = std::fabs((input.handTarget - result.elbow).length() - 1.0f);
    return upperError < 0.01f && lowerError < 0.01f;
}

} // namespace bl1gotyvr::player
