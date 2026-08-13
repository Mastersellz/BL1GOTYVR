#pragma once
#include <cstdint>

namespace bl1gotyvr { namespace input {

// Decoupled aim system (inspired by BioShockVR2)
// Separates weapon aim from camera rotation for more natural VR handling
class AimHook {
public:
    static AimHook& Instance();

    void Initialize();
    void Shutdown();

    // Called each frame with controller state (game thread only)
    void UpdateAim(float controllerPitch, float controllerYaw, float controllerRoll);

    // Get aim rotation offsets (in UE rotator units) - game thread reads
    int32_t GetAimPitchOffset() const { return m_aimPitchOffset; }
    int32_t GetAimYawOffset() const { return m_aimYawOffset; }
    int32_t GetAimRollOffset() const { return m_aimRollOffset; }

    // Trim adjustments (for calibration)
    void SetTrim(int32_t pitch, int32_t yaw, int32_t roll);
    void GetTrim(int32_t& pitch, int32_t& yaw, int32_t& roll) const;

    // Enable/disable decoupled aim
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    // Recenter aim (resets trim to current controller orientation)
    void Recenter();

private:
    AimHook() = default;

    bool m_enabled = true;
    int32_t m_aimPitchOffset = 0;
    int32_t m_aimYawOffset = 0;
    int32_t m_aimRollOffset = 0;

    // Trim values (user-adjustable) - game thread only
    int32_t m_trimPitch = 0;
    int32_t m_trimYaw = 0;
    int32_t m_trimRoll = 0;

    // Reference orientation at last recenter - game thread only
    float m_referencePitch = 0.0f;
    float m_referenceYaw = 0.0f;
    float m_referenceRoll = 0.0f;
    bool m_hasReference = false;
};

}} // namespace bl1gotyvr::input
