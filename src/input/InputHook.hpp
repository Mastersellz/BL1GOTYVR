#pragma once

#include <windows.h>
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
    void UpdateState();
    void Shutdown();

    // Weapon motion
    void CacheWeaponComponent(int index, uintptr_t address, int matrixOffset);
    void ApplyRightHand(int eye);
    void ApplyLeftHand(int eye);
    void Restore();

private:
    InputHook() = default;

    void ProcessTurn();
    void PressKey(int vk);
    void ReleaseKey(int vk);
    void ReleaseAllInput();

    bool m_installed = false;
    HWND m_gameWindow = nullptr;
    unsigned int m_logCounter = 0;

    /* Previous input state */
    int m_prevTrigger = 0, m_prevButtonA = 0;
    int m_prevGrip = 0, m_prevW = 0, m_prevA = 0, m_prevS = 0, m_prevD = 0;
    int m_prevSprint = 0, m_prevJump = 0, m_prevMelee = 0;

    /* Turn system */
    float m_snapTurnAccum = 0;
    LARGE_INTEGER m_perfFrequency = {};
    LARGE_INTEGER m_lastFrameTime = {};
    bool m_perfInitialized = false;

    /* Weapon motion */
    WeaponComponent m_components[7] = {};
    int m_componentCount = 0;
    unsigned int m_applyCalls = 0;
};

}} // namespace bl1gotyvr::input
