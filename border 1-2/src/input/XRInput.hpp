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
    // Capacitive touch sensors (Quest Touch): thumb on stick, index on
    // trigger, thumb on face buttons; squeeze stays analog for the rest.
    bool thumbstickTouch = false;
    bool triggerTouch = false;
    bool primaryTouch = false;
    bool secondaryTouch = false;
    bool valid = false;
    bool aimValid = false;
};

class XRInput {
public:
    static XRInput& Instance();

    bool Initialize(XrInstance instance, XrSession session, XrSpace space,
                    bool touchPlusEnabled);
    void Shutdown();
    bool SyncActions();
    void UpdateControllerStates(XrTime displayTime);
    bool GetControllerSnapshot(ControllerState controllers[2], uint64_t* generation = nullptr) const;
    void SendHaptic(int hand, float amplitude, float durationMs,
                    float frequencyHz = 150.0f);
    void LogCurrentInteractionProfiles() const;

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
    XrAction m_leftThumbstickX = XR_NULL_HANDLE;
    XrAction m_leftThumbstickY = XR_NULL_HANDLE;
    XrAction m_rightThumbstickX = XR_NULL_HANDLE;
    XrAction m_rightThumbstickY = XR_NULL_HANDLE;

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

    // Capacitive touch
    XrAction m_leftThumbstickTouch = XR_NULL_HANDLE;
    XrAction m_rightThumbstickTouch = XR_NULL_HANDLE;
    XrAction m_leftTriggerTouch = XR_NULL_HANDLE;
    XrAction m_rightTriggerTouch = XR_NULL_HANDLE;
    XrAction m_buttonATouch = XR_NULL_HANDLE;
    XrAction m_buttonBTouch = XR_NULL_HANDLE;
    XrAction m_buttonXTouch = XR_NULL_HANDLE;
    XrAction m_buttonYTouch = XR_NULL_HANDLE;

    // Haptic output
    XrAction m_haptic[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    ControllerState m_controllers[2] = {};
    uint64_t m_generation = 0;
    mutable SRWLOCK m_controllerLock = SRWLOCK_INIT;
};

}} // namespace bl1gotyvr::input
