#pragma once

#include "../player/PoseFrame.hpp"
#include <cstdint>

namespace bl1gotyvr::input {

struct MagazinePoseKey {
    uintptr_t weapon = 0;
    uintptr_t component = 0;
    uintptr_t mesh = 0;
    uintptr_t poseData = 0;
    int bone = -1;
    int count = 0;
    bool operator==(const MagazinePoseKey&) const = default;
    bool Valid() const {
        return weapon >= 0x10000 && component >= 0x10000 && mesh >= 0x10000 &&
            poseData >= 0x10000 && count > 0 && count <= 256 && bone >= 0 && bone < count;
    }
};

inline bool UsableMagazineMatrix(const float matrix[16]) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(matrix[i])) return false;
    if (std::fabs(matrix[15] - 1.0f) > 0.05f) return false;
    for (int row = 0; row < 3; ++row) {
        const float length = matrix[row*4]*matrix[row*4] +
            matrix[row*4+1]*matrix[row*4+1] + matrix[row*4+2]*matrix[row*4+2];
        if (length < 1.0e-8f || length > 10000.0f) return false;
    }
    return true;
}

// Include equip scale; rotation transpose is not an affine inverse.
inline bool MagazineWorldToComponent(const float component[16],
        const float world[16], float result[16]) {
    if (!UsableMagazineMatrix(component) || !UsableMagazineMatrix(world)) return false;
    player::Vec3 position;
    if (!player::InversePosePoint(component, {world[12], world[13], world[14]}, position))
        return false;
    float local[16] = {};
    const player::Vec3 origin{component[12], component[13], component[14]};
    for (int row = 0; row < 3; ++row) {
        player::Vec3 axis;
        if (!player::InversePosePoint(component,
                origin + player::Vec3{world[row*4], world[row*4+1], world[row*4+2]}, axis))
            return false;
        local[row*4] = axis.x;
        local[row*4+1] = axis.y;
        local[row*4+2] = axis.z;
    }
    local[12] = position.x; local[13] = position.y; local[14] = position.z;
    local[15] = 1.0f;
    for (int i = 0; i < 16; ++i) result[i] = local[i];
    return true;
}

// Caller supplies a fresh LT edge, not a held level (original interaction).
inline bool CanAcquireMagazine(bool gripPressed, float distance, float radius) {
    return gripPressed && std::isfinite(distance) && std::isfinite(radius) &&
        radius > 0.0f && distance >= 0.0f && distance <= radius;
}

inline bool NeedsOriginalMagazineCapture(int mode, bool grabRotationValid) {
    return (mode == 1 && !grabRotationValid) || mode == 3;
}


} // namespace bl1gotyvr::input
