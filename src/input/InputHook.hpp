#pragma once

#include <windows.h>
#include <openxr/openxr.h>
#include <atomic>
#include <cstdint>

namespace bl1gotyvr { namespace input {

struct ControllerState;

struct WeaponComponent {
    uintptr_t address = 0;
    int matrixOffset = 0;
    float savedMatrix[16] = {};
    float restPosition[3] = {};
    bool hasRestPosition = false;
    int hand = 0;  // 0=right, 1=left
    bool valid = false;
};

struct WeaponMountCacheEntry {
    uintptr_t pawn = 0;
    uintptr_t weapon = 0;
    uintptr_t component = 0;
    uintptr_t skeletalMesh = 0;
    float matrix[16] = {};
    float nativeMatrix[16] = {};
    float gripLocal[3] = {};
    int barrelAxis = 0;
    float barrelSign = 1.0f;
    bool gripValid = false;
    bool absolute = false;
    bool valid = false;
};

struct WeaponAimProfile {
    uint64_t stableKey = 0;
    uintptr_t pawn = 0;
    uintptr_t weapon = 0;
    uintptr_t component = 0;
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    float offsetForward = 0.0f;
    float offsetRight = 0.0f;
    float offsetUp = 0.0f;
    char name[64] = {};
    bool persistent = false;
    bool valid = false;
};

class InputHook {
public:
    static InputHook& Instance();

    void Install();
    void UpdateState(XrTime displayTime);
    void Shutdown();

    // Weapon motion
    void CacheWeaponComponent(int index, uintptr_t address, int matrixOffset);
    void SetCanonicalWeaponPose(const float controllerPosition[3],
                                const float controllerForward[3],
                                const float controllerUp[3],
                                const float cameraPosition[3],
                                const float cameraForward[3],
                                const float cameraUp[3]);
    void ClearCanonicalWeaponPose();
    void RequestMotionCalibrationReset();
    bool IsMotionControlsEnabled() const {
        return m_motionControlsEnabled.load(std::memory_order_acquire);
    }
    bool IsWeaponPoseActive() const {
        return m_weaponPoseActive.load(std::memory_order_acquire);
    }
    bool IsAimDotVisible() const;
    bool GetWeaponBarrelLocalDirection(float direction[3]);
    bool GetDrivenWeaponFrame(float position[3], float forward[3],
                              float up[3]) const;
    void SetWeaponGrabArmed(bool armed) {
        m_weaponGrabArmed.store(armed, std::memory_order_release);
    }
    bool IsWeaponGrabHeld() const {
        return m_weaponGrabHeld.load(std::memory_order_acquire);
    }
    void CancelWeaponGrab();
    bool GetActiveWeaponPoseTuning(float& pitch, float& yaw, float& roll,
                                   float& forward, float& right, float& up) const;
    void ApplyRightHand(int eye);
    void ApplyLeftHand(int eye);
    bool ReapplyWeaponPose(void* component);
    bool ReapplyWeaponPose();
    void Restore();

private:
    InputHook() = default;

    void ProcessTurn();
    void PollAimTuningKeys();
    bool UpdatePhysicalMelee(const ControllerState& left, uint64_t nowMs);
    void ResetPhysicalMelee();
    void ActivateWeaponAimProfile(uintptr_t pawn, uintptr_t weapon,
                                  const char* characterMeshName,
                                  const char* outerName, const char* meshName,
                                  uintptr_t component);
    bool SaveActiveAimProfile();
    void PressKey(int vk);
    void ReleaseKey(int vk);
    void ReleaseAllInput();

    bool m_installed = false;
    bool m_xinputActive = false;
    bool m_outputLive = false;
    HWND m_gameWindow = nullptr;
    unsigned int m_logCounter = 0;

    /* Previous input state */
    int m_prevTrigger = 0, m_prevButtonA = 0, m_prevVehicleAltFire = 0;
    int m_prevGrip = 0, m_prevW = 0, m_prevA = 0, m_prevS = 0, m_prevD = 0;
    int m_prevSprint = 0, m_prevJump = 0, m_prevMelee = 0;
    int m_prevCrouch = 0, m_prevUse = 0, m_prevReload = 0;
    int m_prevGrenade = 0, m_prevMenu = 0, m_prevEcho = 0;
    int m_prevDpadUp = 0, m_prevDpadDown = 0;
    int m_prevDpadLeft = 0, m_prevDpadRight = 0;
    int m_prevWeaponCycle = 0;

    /* Analog/button filtering and Y chord */
    bool m_leftTriggerDown = false;
    bool m_leftTriggerWasDown = false;
    bool m_rightTriggerDown = false;
    bool m_leftGripDown = false;
    bool m_rightGripDown = false;
    bool m_yWasDown = false;
    bool m_yChordUsed = false;
    bool m_recenterChordLatched = false;
    bool m_bWasDown = false;
    bool m_bHoldUsed = false;
    uint64_t m_yPressMs = 0;
    uint64_t m_yTapPulseUntilMs = 0;
    uint64_t m_bPressMs = 0;
    uint64_t m_bTapPulseUntilMs = 0;

    /* Physical melee swing detector */
    float m_meleePreviousTip[3] = {};
    float m_meleeFilteredSpeed = 0.0f;
    float m_meleeTravel = 0.0f;
    uint64_t m_meleePreviousSampleMs = 0;
    uint64_t m_meleeBelowThresholdSinceMs = 0;
    uint64_t m_physicalMeleePulseUntilMs = 0;
    uint64_t m_physicalMeleeCooldownUntilMs = 0;
    bool m_meleeTipValid = false;
    bool m_physicalMeleeReady = true;

    /* Turn system */
    float m_snapTurnAccum = 0;
    LARGE_INTEGER m_perfFrequency = {};
    LARGE_INTEGER m_lastFrameTime = {};
    bool m_perfInitialized = false;

    /* Weapon motion */
    WeaponComponent m_components[7] = {};
    int m_componentCount = 0;
    unsigned int m_applyCalls = 0;
    float m_canonicalWeaponPosition[3] = {};
    float m_canonicalWeaponForward[3] = {1.0f, 0.0f, 0.0f};
    float m_canonicalWeaponUp[3] = {0.0f, 0.0f, 1.0f};
    float m_nativeCameraPosition[3] = {};
    float m_nativeCameraForward[3] = {1.0f, 0.0f, 0.0f};
    float m_nativeCameraUp[3] = {0.0f, 0.0f, 1.0f};
    bool m_canonicalWeaponPoseValid = false;
    std::atomic<bool> m_weaponPoseActive{false};
    float m_weaponMountMatrix[16] = {};
    float m_weaponNativeMatrix[16] = {};
    float m_weaponGripLocal[3] = {};
    bool m_weaponNativeMatrixValid = false;
    bool m_weaponGripValid = false;
    uintptr_t m_mountWeapon = 0;
    uintptr_t m_mountComponent = 0;
    uint64_t m_mountIdentityGeneration = 0;
    bool m_weaponMountValid = false;
    bool m_weaponMountAbsolute = false;
    int m_weaponBarrelAxis = 0;
    float m_weaponBarrelSign = 1.0f;
    static constexpr int kWeaponMountCacheCapacity = 16;
    WeaponMountCacheEntry m_weaponMountCache[kWeaponMountCacheCapacity] = {};
    int m_weaponMountCacheCursor = 0;
    uintptr_t m_lastDrivenWeapon = 0;
    uintptr_t m_lastDrivenComponent = 0;
    uint64_t m_lastWeaponRefreshGeneration = 0;
    uint64_t m_pendingWeaponIdentityGeneration = 0;
    uintptr_t m_pendingWeaponPawn = 0;
    uintptr_t m_pendingWeapon = 0;
    uintptr_t m_pendingWeaponComponent = 0;
    uint64_t m_weaponIdentityStableSinceMs = 0;
    uint64_t m_nextWeaponComponentScanMs = 0;
    mutable SRWLOCK m_weaponPoseWriteLock = SRWLOCK_INIT;
    uintptr_t m_renderWeaponComponent = 0;
    int m_renderWeaponMatrixOffset = 0;
    float m_renderWeaponMatrix[16] = {};
    float m_renderWeaponGripPosition[3] = {};
    float m_renderWeaponForward[3] = {1.0f, 0.0f, 0.0f};
    float m_renderWeaponUp[3] = {0.0f, 0.0f, 1.0f};
    bool m_renderWeaponStampActive = false;
    uint64_t m_renderWeaponStampUpdatedMs = 0;
    float m_weaponBarrelLocalDirection[3] = {1.0f, 0.0f, 0.0f};
    bool m_weaponBarrelDirectionValid = false;
    std::atomic<uint64_t> m_postAnimationWeaponWrites{0};
    std::atomic<bool> m_weaponCalibrationResetRequested{false};
    std::atomic<bool> m_motionControlsEnabled{true};
    std::atomic<uint64_t> m_dotVisibleAtMs{0};
    std::atomic<bool> m_weaponGrabArmed{false};
    std::atomic<bool> m_weaponGrabHeld{false};
    std::atomic<bool> m_weaponGrabCancelRequested{false};
    bool m_weaponGrabLatched = false;

    /* Ballistic calibration */
    uint32_t m_aimTuningKeysDown = 0;
    uint64_t m_aimTuningNextRepeatMs[10] = {};
    bool m_weaponTuningPositionMode = false;
    int m_handTuningHand = 1;
    bool m_handTuningDirty = false;
    bool m_aimTuningDirty = false;
    float m_aimTrimPitch = 0.0f;
    float m_aimTrimYaw = 0.0f;
    float m_aimTrimRoll = 0.0f;
    float m_weaponOffsetForward = 0.0f;
    float m_weaponOffsetRight = 0.0f;
    float m_weaponOffsetUp = 0.0f;
    std::atomic<float> m_activeWeaponTrimPitch{0.0f};
    std::atomic<float> m_activeWeaponTrimYaw{0.0f};
    std::atomic<float> m_activeWeaponTrimRoll{0.0f};
    std::atomic<float> m_activeWeaponOffsetForward{0.0f};
    std::atomic<float> m_activeWeaponOffsetRight{0.0f};
    std::atomic<float> m_activeWeaponOffsetUp{0.0f};
    std::atomic<bool> m_activeWeaponTrimValid{false};
    float m_defaultAimTrimPitch = 0.0f;
    float m_defaultAimTrimYaw = 0.0f;
    static constexpr int kWeaponAimProfileCapacity = 64;
    WeaponAimProfile m_weaponAimProfiles[kWeaponAimProfileCapacity] = {};
    int m_weaponAimProfileCursor = 0;
    int m_activeWeaponAimProfile = -1;
};

}} // namespace bl1gotyvr::input
