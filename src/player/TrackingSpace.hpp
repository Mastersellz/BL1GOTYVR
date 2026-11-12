#pragma once

#include "TwoBoneIK.hpp"

namespace bl1gotyvr::player {

struct TrackingSpaceFrame {
    Vec3 anchor;
    Vec3 referencePosition;
    Quat referenceRotation;
    float yaw = 0.0f;
    float scale = 100.0f;
    bool valid = false;
};

inline bool TrackingWorldToStage(const TrackingSpaceFrame& frame,
                                  const Vec3& world, Vec3& stage) {
    const auto& q = frame.referenceRotation;
    const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    if (!frame.valid || !std::isfinite(frame.scale) || frame.scale <= 1.0e-4f ||
        !std::isfinite(frame.yaw) || !std::isfinite(norm) || norm < 0.5f || norm > 1.5f)
        return false;
    const Vec3 delta = (world - frame.anchor) * (1.0f / frame.scale);
    const float c = std::cos(frame.yaw), s = std::sin(frame.yaw);
    const Vec3 ue{c*delta.x + s*delta.y, -s*delta.x + c*delta.y, delta.z};
    const Vec3 result = frame.referencePosition + RotateByQuat(frame.referenceRotation,
        {ue.y, ue.z, -ue.x});
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
        return false;
    stage = result;
    return true;
}

} // namespace bl1gotyvr::player
