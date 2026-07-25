#pragma once

#include <openxr/openxr.h>
#include <cstdint>

namespace bl1gotyvr { namespace input {

struct ControllerState {
    float position[3] = {};
    float rotation[4] = {};     // quaternion [x,y,z,w]
    float eulerPitch = 0, eulerYaw = 0, eulerRoll = 0;
    float thumbstickX = 0, thumbstickY = 0;  // normalized -1..1
    float trigger = 0;          // 0..1
    float grip = 0;             // 0..1
    bool buttonA = false;
    bool buttonB = false;
    bool thumbstickClick = false;
    bool menuButton = false;
    bool valid = false;
};

class XRInput {
public:
    static XRInput& Instance();

    bool Initialize(XrInstance instance, XrSession session, XrSpace space);
    void Shutdown();
    void SyncActions();
    void UpdateControllerState(int hand);  // 0=left, 1=right

    const ControllerState& GetLeft() const { return m_controllers[0]; }
    const ControllerState& GetRight() const { return m_controllers[1]; }

private:
    XRInput() = default;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_space = XR_NULL_HANDLE;

    XrActionSet m_actionSet = XR_NULL_HANDLE;

    // Pose actions
    XrAction m_leftPose = XR_NULL_HANDLE;
    XrAction m_rightPose = XR_NULL_HANDLE;
    XrSpace m_leftPoseSpace = XR_NULL_HANDLE;
    XrSpace m_rightPoseSpace = XR_NULL_HANDLE;

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
    XrAction m_leftThumbstickClick = XR_NULL_HANDLE;
    XrAction m_rightThumbstickClick = XR_NULL_HANDLE;
    XrAction m_menuButton = XR_NULL_HANDLE;

    ControllerState m_controllers[2] = {};
};

}} // namespace bl1gotyvr::input
