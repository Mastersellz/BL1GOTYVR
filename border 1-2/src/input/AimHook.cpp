#include "AimHook.hpp"
#include "../core/VRMod.hpp"
#include "../camera/CameraHook.hpp"
#include <cmath>

namespace bl1gotyvr { namespace input {

// UE rotation constants
constexpr float kRotUnitsPerRadian = 65536.0f / (2.0f * 3.14159265f);
constexpr float kRotUnitsPerDegree = 65536.0f / 360.0f;

AimHook& AimHook::Instance() {
    static AimHook hook;
    return hook;
}

void AimHook::Initialize() {
    bl1gotyvr::Log("[Aim] Initializing decoupled aim system");
    m_enabled = true;
    m_hasReference = false;
    m_aimPitchOffset = 0;
    m_aimYawOffset = 0;
    m_aimRollOffset = 0;
    m_trimPitch = 0;
    m_trimYaw = 0;
    m_trimRoll = 0;
}

void AimHook::Shutdown() {
    bl1gotyvr::Log("[Aim] Shutting down decoupled aim system");
    m_enabled = false;
    m_hasReference = false;
}

void AimHook::UpdateAim(float controllerPitch, float controllerYaw, float controllerRoll) {
    if (!m_enabled) {
        m_aimPitchOffset = 0;
        m_aimYawOffset = 0;
        m_aimRollOffset = 0;
        return;
    }

    // Set reference on first update or after recenter
    if (!m_hasReference) {
        m_referencePitch = controllerPitch;
        m_referenceYaw = controllerYaw;
        m_referenceRoll = controllerRoll;
        m_hasReference = true;
        bl1gotyvr::Log("[Aim] Reference set: pitch=%.3f yaw=%.3f roll=%.3f",
            controllerPitch * 57.29578f, controllerYaw * 57.29578f, controllerRoll * 57.29578f);
    }

    // Calculate offset from reference (in radians)
    const float pitchDelta = controllerPitch - m_referencePitch;
    const float yawDelta = controllerYaw - m_referenceYaw;
    const float rollDelta = controllerRoll - m_referenceRoll;

    // Convert to UE rotator units and apply trim
    m_aimPitchOffset = static_cast<int32_t>(pitchDelta * kRotUnitsPerRadian) + m_trimPitch;
    m_aimYawOffset = static_cast<int32_t>(yawDelta * kRotUnitsPerRadian) + m_trimYaw;
    m_aimRollOffset = static_cast<int32_t>(rollDelta * kRotUnitsPerRadian) + m_trimRoll;
}

void AimHook::SetTrim(int32_t pitch, int32_t yaw, int32_t roll) {
    m_trimPitch = pitch;
    m_trimYaw = yaw;
    m_trimRoll = roll;
    bl1gotyvr::Log("[Aim] Trim set: pitch=%d yaw=%d roll=%d", pitch, yaw, roll);
}

void AimHook::GetTrim(int32_t& pitch, int32_t& yaw, int32_t& roll) const {
    pitch = m_trimPitch;
    yaw = m_trimYaw;
    roll = m_trimRoll;
}

void AimHook::Recenter() {
    m_hasReference = false;
    bl1gotyvr::Log("[Aim] Recenter requested");
}

}} // namespace bl1gotyvr::input
