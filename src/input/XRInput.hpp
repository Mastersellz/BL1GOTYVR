#pragma once

#include <openxr/openxr.h>
#include <cstdint>
#include <Windows.h>

namespace bl1gotyvr { namespace input {

struct ControllerState {
    float position[3] = {};
    float rotation[4] = {};     // quaternion [x,y,z,w]
    float aimPosition[3] = {};
    float aimRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float eulerPitch = 0, eulerYaw = 0, eulerRoll = 0;
    float thumbstickX = 0, thumbstickY = 0;  // normalized -1..1
    float trigger = 0;          // 0..1
    float grip = 0;             // 0..1
    bool buttonA = false;
    bool buttonB = false;
    bool buttonX = false;
    bool buttonY = false;
    bool thumbstickClick = false;
    bool menuButton = false;
    bool valid = false;
    bool aimValid = false;
};

class XRInput {
public:
    static XRInput& Instance();

    bool Initialize(XrInstance instance, XrSession session, XrSpace space);
    void Shutdown();
    bool SyncActions();
    void UpdateControllerStates(XrTime displayTime);
    bool GetControllerSnapshot(ControllerState controllers[2], uint64_t* generation = nullptr) const;

private:
    XRInput() = default;
    void UpdateControllerStateUnlocked(int hand, XrTime displayTime);

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_space = XR_NULL_HANDLE;

    XrActionSet m_actionSet = XR_NULL_HANDLE;

    // Pose actions
    XrAction m_leftPose = XR_NULL_HANDLE;
    XrAction m_rightPose = XR_NULL_HANDLE;
    XrAction m_rightAimPose = XR_NULL_HANDLE;
    XrSpace m_leftPoseSpace = XR_NULL_HANDLE;
    XrSpace m_rightPoseSpace = XR_NULL_HANDLE;
    XrSpace m_rightAimPoseSpace = XR_NULL_HANDLE;

    // Thumbstick
    XrAction m_leftThumbstick = XR_NULL_HANDLE;
    XrAction m_rightThumbstick = XR_NULL_HANDLE;

    // Trigger
    XrAction m_leftTrigger = XR_NULL_HANDLE;
    XrAction m_rightTrigger = XR_NULL_HANDLE;

    // Grip
    XrAction m_leftGrip = XR_NULL_HANDLE;
    XrAction m_rightGrip = XR_NULL_HANDLE;

    // Buttons
    XrAction m_buttonA = XR_NULL_HANDLE;
    XrAction m_buttonB = XR_NULL_HANDLE;
    XrAction m_buttonX = XR_NULL_HANDLE;
    XrAction m_buttonY = XR_NULL_HANDLE;
    XrAction m_leftThumbstickClick = XR_NULL_HANDLE;
    XrAction m_rightThumbstickClick = XR_NULL_HANDLE;
    XrAction m_menuButton = XR_NULL_HANDLE;

    ControllerState m_controllers[2] = {};
    uint64_t m_generation = 0;
    mutable SRWLOCK m_controllerLock = SRWLOCK_INIT;
};

}} // namespace bl1gotyvr::input
