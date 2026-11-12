#pragma once

#include "TwoBoneIK.hpp"

namespace bl1gotyvr::player {

// UE component matrices store basis vectors at 0/4/8 and translation at 12.
inline Vec3 TransformPosePoint(const float matrix[16], const Vec3& point) {
    return {
        matrix[0]*point.x + matrix[4]*point.y + matrix[8]*point.z + matrix[12],
        matrix[1]*point.x + matrix[5]*point.y + matrix[9]*point.z + matrix[13],
        matrix[2]*point.x + matrix[6]*point.y + matrix[10]*point.z + matrix[14]};
}

// Do not use rotation transpose as an inverse: the native viewmodel can have
// non-unit scale during equip/animation. Reject invalid/singular input.
inline bool InversePosePoint(const float matrix[16], const Vec3& world, Vec3& local) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(matrix[i])) return false;
    const Vec3 x{matrix[0], matrix[1], matrix[2]};
    const Vec3 y{matrix[4], matrix[5], matrix[6]};
    const Vec3 z{matrix[8], matrix[9], matrix[10]};
    const Vec3 yz{y.y*z.z - y.z*z.y, y.z*z.x - y.x*z.z, y.x*z.y - y.y*z.x};
    const Vec3 zx{z.y*x.z - z.z*x.y, z.z*x.x - z.x*x.z, z.x*x.y - z.y*x.x};
    const Vec3 xy{x.y*y.z - x.z*y.y, x.z*y.x - x.x*y.z, x.x*y.y - x.y*y.x};
    const float determinant = x.x*yz.x + x.y*yz.y + x.z*yz.z;
    if (!std::isfinite(determinant) || std::fabs(determinant) < 1.0e-6f) return false;
    const Vec3 delta = world - Vec3{matrix[12], matrix[13], matrix[14]};
    const Vec3 result{
        (delta.x*yz.x + delta.y*yz.y + delta.z*yz.z) / determinant,
        (delta.x*zx.x + delta.y*zx.y + delta.z*zx.z) / determinant,
        (delta.x*xy.x + delta.y*xy.y + delta.z*xy.z) / determinant};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z)) return false;
    local = result;
    return true;
}

} // namespace bl1gotyvr::player
