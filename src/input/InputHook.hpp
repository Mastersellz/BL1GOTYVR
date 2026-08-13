#pragma once

#include <windows.h>
#include <openxr/openxr.h>
#include <cstdint>

namespace bl1gotyvr { namespace input {

struct WeaponComponent {
    uintptr_t address = 0;
    int matrixOffset = 0;
    float savedMatrix[16] = {};
    float restPosition[3] = {};
    bool hasRestPosition = false;
    int hand = 0;  // 0=right, 1=left
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
    void ApplyRightHand(int eye);
    void ApplyLeftHand(int eye);
    void Restore();

private:
    InputHook() = default;

    void ProcessTurn();
    void PollAimTuningKeys();
    void PressKey(int vk);
    void ReleaseKey(int vk);
    void ReleaseAllInput();

    bool m_installed = false;
    bool m_xinputActive = false;
    bool m_outputLive = false;
    HWND m_gameWindow = nullptr;
    unsigned int m_logCounter = 0;

    /* Previous input state */
    int m_prevTrigger = 0, m_prevButtonA = 0;
    int m_prevGrip = 0, m_prevW = 0, m_prevA = 0, m_prevS = 0, m_prevD = 0;
    int m_prevSprint = 0, m_prevJump = 0, m_prevMelee = 0;
    int m_prevCrouch = 0, m_prevUse = 0, m_prevReload = 0;
    int m_prevGrenade = 0, m_prevMenu = 0, m_prevEcho = 0;
    int m_prevDpadUp = 0, m_prevDpadDown = 0;
    int m_prevDpadLeft = 0, m_prevDpadRight = 0;
    int m_prevWeaponCycle = 0;

    /* Analog/button filtering and Y chord */
    bool m_leftTriggerDown = false;
    bool m_rightTriggerDown = false;
    bool m_leftGripDown = false;
    bool m_rightGripDown = false;
    bool m_yWasDown = false;
    bool m_yChordUsed = false;
    bool m_recenterChordLatched = false;
    uint64_t m_yPressMs = 0;
    uint64_t m_yTapPulseUntilMs = 0;

    /* Turn system */
    float m_snapTurnAccum = 0;
    LARGE_INTEGER m_perfFrequency = {};
    LARGE_INTEGER m_lastFrameTime = {};
    bool m_perfInitialized = false;

    /* Weapon motion */
    WeaponComponent m_components[7] = {};
    int m_componentCount = 0;
    unsigned int m_applyCalls = 0;

    /* Ballistic calibration */
    uint32_t m_aimTuningKeysDown = 0;
    float m_aimTrimPitch = 0.0f;
    float m_aimTrimYaw = 0.0f;
};

}} // namespace bl1gotyvr::input
