#pragma once

#include <cmath>
#include <cstdint>

namespace bl1gotyvr::camera {

inline bool IsViewportPoseThread(uint32_t currentThread, uint32_t viewportThread) {
    return viewportThread != 0 && currentThread == viewportThread;
}

// On foot the HMD supplies pitch. Never promote the previously restored HMD
// interaction pitch (or recoil) to a new, permanent game-camera reference.
inline int32_t SelectGamePitchReference(bool onFoot, int32_t nativePitch) {
    if (onFoot || nativePitch < -65536 || nativePitch > 65535) return 0;
    const uint32_t wrapped = static_cast<uint32_t>(nativePitch) & 0xffffu;
    return wrapped >= 32768u ? static_cast<int32_t>(wrapped) - 65536
                            : static_cast<int32_t>(wrapped);
}

inline bool UseFrozenViewRotation(bool xrViewsValid, uint64_t pair,
        bool livePawn, bool vehicle, bool recoverFirstPerson) {
    return recoverFirstPerson || (xrViewsValid && pair != 0 && livePawn && !vehicle);
}

inline void BuildStableViewRotation(const int32_t rotation[3], float matrix[16]) {
    constexpr float kUnisToRadians = 6.2831853071795864769f / 65536.0f;
    const float pitch = SelectGamePitchReference(false, rotation[0]) * kUnisToRadians;
    // Yaw/roll may accumulate whole turns. Reduce before float conversion.
    const float yaw = static_cast<int32_t>(static_cast<uint32_t>(rotation[1]) & 0xffffu) * kUnisToRadians;
    const float roll = static_cast<int32_t>(static_cast<uint32_t>(rotation[2]) & 0xffffu) * kUnisToRadians;
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cr = std::cos(roll), sr = std::sin(roll);
    const float right[3] = {-sy, cy, 0.0f};
    const float up[3] = {-sp*cy, -sp*sy, cp};
    const float forward[3] = {cp*cy, cp*sy, sp};
    for (int element = 0; element < 16; ++element) matrix[element] = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        matrix[axis*4] = right[axis]*cr + up[axis]*sr;
        matrix[axis*4 + 1] = up[axis]*cr - right[axis]*sr;
        matrix[axis*4 + 2] = forward[axis];
    }
    matrix[15] = 1.0f;
}

// Owned exclusively by the viewport/game thread. Input only publishes the
// atomic fire state; it must not mutate the live camera or this hold state.
class RecoilPitchHold {
public:
    bool Update(bool enabled, bool firing, uintptr_t controller, uintptr_t pawn,
                uint64_t nowMs) {
        if (!enabled || !controller || !pawn) {
            *this = {};
            return false;
        }
        if (controller != m_controller || pawn != m_pawn ||
            (m_seenFire && nowMs < m_lastFireMs)) {
            *this = {};
            m_controller = controller;
            m_pawn = pawn;
        }
        if (firing) {
            m_lastFireMs = nowMs;
            m_seenFire = true;
        }
        return m_seenFire && (firing || nowMs - m_lastFireMs < 400);
    }

private:
    uintptr_t m_controller = 0;
    uintptr_t m_pawn = 0;
    uint64_t m_lastFireMs = 0;
    bool m_seenFire = false;
};

} // namespace bl1gotyvr::camera
