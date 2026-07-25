#include "InputHook.hpp"
#include "XRInput.hpp"
#include "../core/VRMod.hpp"
#include "../config/Config.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>

namespace bl1gotyvr { namespace input {

InputHook& InputHook::Instance() {
    static InputHook hook;
    return hook;
}

static float GetFrameDeltaTime(InputHook& hook) {
    // Access private members via friend or make this a member — simplified here
    return 1.0f / 60.0f;
}

void InputHook::Install() {
    if (m_installed) return;
    Log("[Input] Installing VR input system...");

    m_gameWindow = FindWindowA(NULL, "BorderlandsGOTY");
    if (!m_gameWindow) m_gameWindow = FindWindowA("LaunchUnrealUWindowsClient", NULL);
    Log("[Input] Game window: %p", m_gameWindow);

    if (!QueryPerformanceFrequency(&m_perfFrequency))
        m_perfFrequency.QuadPart = 10000000LL;
    QueryPerformanceCounter(&m_lastFrameTime);
    m_perfInitialized = true;

    m_installed = true;
    Log("[Input] Installed (turn_mode=%d smooth=%.0f snap=%.0f deadzone=%.2f weapon_pos=%.2f)",
        config::Get().turn_mode, config::Get().smooth_turn_speed,
        config::Get().snap_turn_angle, config::Get().locomotion_deadzone,
        config::Get().weapon_position_scale);
}

static bool IsGameForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg) return false;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void InputHook::PressKey(int vk) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    SendInput(1, &input, sizeof(INPUT));
}

void InputHook::ReleaseKey(int vk) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputHook::ReleaseAllInput() {
    INPUT inputs[8] = {};
    int count = 0;
#define RELEASE(s, k) if (s) { inputs[count].type=INPUT_KEYBOARD; inputs[count].ki.wVk=k; inputs[count++].ki.dwFlags=KEYEVENTF_KEYUP; }
    RELEASE(m_prevGrip, 'R')
    RELEASE(m_prevW, 'W')
    RELEASE(m_prevA, 'A')
    RELEASE(m_prevS, 'S')
    RELEASE(m_prevD, 'D')
    RELEASE(m_prevSprint, VK_LSHIFT)
    RELEASE(m_prevJump, VK_SPACE)
    RELEASE(m_prevMelee, 'V')
#undef RELEASE
    if (count) SendInput(count, inputs, sizeof(INPUT));
    m_prevGrip = m_prevW = m_prevA = m_prevS = m_prevD = 0;
    m_prevSprint = m_prevJump = m_prevMelee = 0;

    INPUT mouse[2] = {};
    int mcount = 0;
    if (m_prevTrigger) { mouse[mcount].type=INPUT_MOUSE; mouse[mcount++].mi.dwFlags=MOUSEEVENTF_LEFTUP; }
    if (m_prevButtonA) { mouse[mcount].type=INPUT_MOUSE; mouse[mcount++].mi.dwFlags=MOUSEEVENTF_RIGHTUP; }
    if (mcount) SendInput(mcount, mouse, sizeof(INPUT));
    m_prevTrigger = m_prevButtonA = 0;
    m_snapTurnAccum = 0;
}

void InputHook::ProcessTurn() {
    const auto& xr = XRInput::Instance();
    const auto& right = xr.GetRight();
    const auto& cfg = config::Get();

    if (cfg.turn_mode == 2 || !right.valid) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = m_perfInitialized ?
        (float)(now.QuadPart - m_lastFrameTime.QuadPart) / (float)m_perfFrequency.QuadPart : 1.0f/60.0f;
    m_lastFrameTime = now;
    if (dt <= 0 || dt > 0.5f) dt = 1.0f/60.0f;

    float sx = right.thumbstickX;
    float deadzone = cfg.locomotion_deadzone;
    float magnitude = sx;
    if (magnitude > 0) {
        if (magnitude < deadzone) magnitude = 0;
        else magnitude = (magnitude - deadzone) / (1.0f - deadzone);
    } else {
        if (magnitude > -deadzone) magnitude = 0;
        else magnitude = (magnitude + deadzone) / (1.0f - deadzone);
    }
    if (magnitude == 0) { m_snapTurnAccum = 0; return; }

    if (cfg.turn_mode == 0) {
        /* Snap turn */
        m_snapTurnAccum += magnitude * cfg.smooth_turn_speed * dt;
        if (m_snapTurnAccum >= cfg.snap_turn_angle) {
            mouse_event(MOUSEEVENTF_MOVE, 3, 0, 0, 0);
            m_snapTurnAccum -= cfg.snap_turn_angle;
        } else if (m_snapTurnAccum <= -cfg.snap_turn_angle) {
            mouse_event(MOUSEEVENTF_MOVE, -3, 0, 0, 0);
            m_snapTurnAccum += cfg.snap_turn_angle;
        }
    } else {
        /* Smooth turn */
        float turnAmount = magnitude * cfg.smooth_turn_speed * dt;
        int dx = (int)(turnAmount * 0.5f);
        if (dx == 0) dx = (turnAmount > 0) ? 1 : -1;
        mouse_event(MOUSEEVENTF_MOVE, dx, 0, 0, 0);
    }
}

void InputHook::UpdateState() {
    if (!m_installed) return;

    auto& xr = XRInput::Instance();
    xr.SyncActions();
    xr.UpdateControllerState(0);  // left
    xr.UpdateControllerState(1);  // right

    const auto& right = xr.GetRight();
    const auto& left = xr.GetLeft();
    const auto& cfg = config::Get();

    if (!right.valid) { ReleaseAllInput(); return; }
    if (!IsGameForeground(m_gameWindow)) { ReleaseAllInput(); return; }

    if (m_logCounter++ % 300 == 0)
        Log("[Input] right=(%.1f,%.1f,%.1f) pitch=%.1f yaw=%.1f trigger=%.2f grip=%.2f stick=(%.2f,%.2f)",
            right.position[0], right.position[1], right.position[2],
            right.eulerPitch, right.eulerYaw, right.trigger, right.grip,
            right.thumbstickX, right.thumbstickY);

    /* Trigger → fire */
    int trigger = right.trigger > 0.5f ? 1 : 0;
    if (trigger && !m_prevTrigger) mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    else if (!trigger && m_prevTrigger) mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    m_prevTrigger = trigger;

    /* Button A → ADS */
    int btnA = right.buttonA ? 1 : 0;
    if (btnA && !m_prevButtonA) mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
    else if (!btnA && m_prevButtonA) mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
    m_prevButtonA = btnA;

    /* Right thumbstick → turn */
    ProcessTurn();

    /* Left controller */
    if (left.valid) {
        int grip = left.grip > 0.5f ? 1 : 0;
        if (grip && !m_prevGrip) PressKey('R');
        else if (!grip && m_prevGrip) ReleaseKey('R');
        m_prevGrip = grip;

        int stickClick = left.thumbstickClick ? 1 : 0;
        if (stickClick && !m_prevSprint) PressKey(VK_LSHIFT);
        else if (!stickClick && m_prevSprint) ReleaseKey(VK_LSHIFT);
        m_prevSprint = stickClick;

        int jump = right.buttonB ? 1 : 0;
        if (jump && !m_prevJump) PressKey(VK_SPACE);
        else if (!jump && m_prevJump) ReleaseKey(VK_SPACE);
        m_prevJump = jump;

        int melee = left.trigger > 0.5f ? 1 : 0;
        if (melee && !m_prevMelee) PressKey('V');
        else if (!melee && m_prevMelee) ReleaseKey('V');
        m_prevMelee = melee;

        /* WASD */
        float sx = left.thumbstickX;
        float sy = left.thumbstickY;
        float deadzone = cfg.locomotion_deadzone;
        int w = sy > deadzone, s = sy < -deadzone;
        int d = sx > deadzone, a = sx < -deadzone;
        INPUT inputs[4] = {};
        int count = 0;
        if (w && !m_prevW) { inputs[count].type=INPUT_KEYBOARD; inputs[count].ki.wVk='W'; count++; }
        if (!w && m_prevW) { inputs[count].type=INPUT_KEYBOARD; inputs[count].ki.wVk='W'; inputs[count].ki.dwFlags=KEYEVENTF_KEYUP; count++; }
        if (a && !m_prevA) { inputs[count].type=INPUT_KEYBOARD; inputs[count].ki.wVk='A'; count++; }
        if (!a && m_prevA) { inputs[count].type=INPUT_KEYBOARD; inputs[count].ki.wVk='A'; inputs[count].ki.dwFlags=KEYEVENTF_KEYUP; count++; }
        if (s && !m_prevS) { inputs[count].type=INPUT_KEYBOARD; inputs[count].ki.wVk='S'; count++; }
        if (!s && m_prevS) { inputs[count].type=INPUT_KEYBOARD; inputs[count].ki.wVk='S'; inputs[count].ki.dwFlags=KEYEVENTF_KEYUP; count++; }
        if (d && !m_prevD) { inputs[count].type=INPUT_KEYBOARD; inputs[count].ki.wVk='D'; count++; }
        if (!d && m_prevD) { inputs[count].type=INPUT_KEYBOARD; inputs[count].ki.wVk='D'; inputs[count].ki.dwFlags=KEYEVENTF_KEYUP; count++; }
        if (count > 0) SendInput(count, inputs, sizeof(INPUT));
        m_prevW = w; m_prevA = a; m_prevS = s; m_prevD = d;
    } else if (m_prevGrip || m_prevW || m_prevA || m_prevS || m_prevD ||
               m_prevSprint || m_prevJump || m_prevMelee) {
        ReleaseAllInput();
    }
}

/* Weapon Motion */
void InputHook::CacheWeaponComponent(int index, uintptr_t address, int matrixOffset) {
    if (index < 0 || index >= 7) return;
    m_components[index].address = address;
    m_components[index].matrixOffset = matrixOffset;
    m_components[index].hand = (index == 0) ? 0 : 1;
    m_components[index].hasRestPosition = false;
    m_components[index].valid = true;
    if (index + 1 > m_componentCount) m_componentCount = index + 1;
}

static void QuaternionToRotationMatrix(float qw, float qx, float qy, float qz, float out[9]) {
    float xx=qx*qx, yy=qy*qy, zz=qz*qz, xy=qx*qy, xz=qx*qz, yz=qy*qz;
    float wx=qw*qx, wy=qw*qy, wz=qw*qz;
    out[0]=1-2*(yy+zz); out[1]=2*(xy-wz);  out[2]=2*(xz+wy);
    out[3]=2*(xy+wz);   out[4]=1-2*(xx+zz); out[5]=2*(yz-wx);
    out[6]=2*(xz-wy);   out[7]=2*(yz+wx);   out[8]=1-2*(xx+yy);
}

void InputHook::ApplyRightHand(int eye) {
    (void)eye;
    if (!m_componentCount) return;
    const auto& xr = XRInput::Instance();
    const auto& right = xr.GetRight();
    if (!right.valid) return;

    float vrRot[9];
    QuaternionToRotationMatrix(right.rotation[3], right.rotation[0], right.rotation[1], right.rotation[2], vrRot);

    float posScale = config::Get().weapon_position_scale;

    for (int i = 0; i < m_componentCount; ++i) {
        WeaponComponent& w = m_components[i];
        if (!w.valid || !w.address || !w.matrixOffset || w.hand != 0) continue;

        uintptr_t addr = w.address + w.matrixOffset;
        float matrix[16];
        ReadProcessMemory(GetCurrentProcess(), (void*)addr, matrix, sizeof(float)*16, NULL);
        memcpy(w.savedMatrix, matrix, sizeof(float)*16);

        /* Rotation */
        matrix[0]=vrRot[0]; matrix[1]=vrRot[1]; matrix[2]=vrRot[2];
        matrix[4]=vrRot[3]; matrix[5]=vrRot[4]; matrix[6]=vrRot[5];
        matrix[8]=vrRot[6]; matrix[9]=vrRot[7]; matrix[10]=vrRot[8];

        /* Position delta (6DoF) */
        if (posScale > 0) {
            float controllerPosUU[3] = { right.position[0]*200, right.position[1]*200, right.position[2]*200 };
            if (!w.hasRestPosition) {
                w.restPosition[0] = matrix[12]; w.restPosition[1] = matrix[13]; w.restPosition[2] = matrix[14];
                w.hasRestPosition = true;
            }
            float dx = controllerPosUU[0] - w.restPosition[0];
            float dy = controllerPosUU[1] - w.restPosition[1];
            float dz = controllerPosUU[2] - w.restPosition[2];
            matrix[12] = w.restPosition[0] + dx * posScale;
            matrix[13] = w.restPosition[1] + dy * posScale;
            matrix[14] = w.restPosition[2] + dz * posScale;
        }

        WriteProcessMemory(GetCurrentProcess(), (void*)addr, matrix, sizeof(float)*16, NULL);
    }
    ++m_applyCalls;
}

void InputHook::ApplyLeftHand(int eye) {
    (void)eye;
    if (!m_componentCount) return;
    const auto& xr = XRInput::Instance();
    const auto& left = xr.GetLeft();
    if (!left.valid) return;

    float vrRot[9];
    QuaternionToRotationMatrix(left.rotation[3], left.rotation[0], left.rotation[1], left.rotation[2], vrRot);

    float posScale = config::Get().weapon_position_scale;

    for (int i = 0; i < m_componentCount; ++i) {
        WeaponComponent& w = m_components[i];
        if (!w.valid || !w.address || !w.matrixOffset || w.hand != 1) continue;

        uintptr_t addr = w.address + w.matrixOffset;
        float matrix[16];
        ReadProcessMemory(GetCurrentProcess(), (void*)addr, matrix, sizeof(float)*16, NULL);
        memcpy(w.savedMatrix, matrix, sizeof(float)*16);

        matrix[0]=vrRot[0]; matrix[1]=vrRot[1]; matrix[2]=vrRot[2];
        matrix[4]=vrRot[3]; matrix[5]=vrRot[4]; matrix[6]=vrRot[5];
        matrix[8]=vrRot[6]; matrix[9]=vrRot[7]; matrix[10]=vrRot[8];

        if (posScale > 0) {
            float controllerPosUU[3] = { left.position[0]*200, left.position[1]*200, left.position[2]*200 };
            if (!w.hasRestPosition) {
                w.restPosition[0] = matrix[12]; w.restPosition[1] = matrix[13]; w.restPosition[2] = matrix[14];
                w.hasRestPosition = true;
            }
            float dx = controllerPosUU[0] - w.restPosition[0];
            float dy = controllerPosUU[1] - w.restPosition[1];
            float dz = controllerPosUU[2] - w.restPosition[2];
            matrix[12] = w.restPosition[0] + dx * posScale;
            matrix[13] = w.restPosition[1] + dy * posScale;
            matrix[14] = w.restPosition[2] + dz * posScale;
        }

        WriteProcessMemory(GetCurrentProcess(), (void*)addr, matrix, sizeof(float)*16, NULL);
    }
}

void InputHook::Restore() {
    for (int i = 0; i < m_componentCount; ++i) {
        WeaponComponent& w = m_components[i];
        if (!w.valid || !w.address || !w.matrixOffset) continue;
        uintptr_t addr = w.address + w.matrixOffset;
        WriteProcessMemory(GetCurrentProcess(), (void*)addr, w.savedMatrix, sizeof(float)*16, NULL);
    }
}

void InputHook::Shutdown() {
    ReleaseAllInput();
    m_installed = false;
    Log("[Input] Shutdown");
}

}} // namespace bl1gotyvr::input
