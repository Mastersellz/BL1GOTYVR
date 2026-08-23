#include "XRInput.hpp"
#include "XInputBridge.hpp"
#include "../core/VRMod.hpp"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>

namespace bl1gotyvr { namespace input {

XRInput& XRInput::Instance() {
    static XRInput input;
    return input;
}

static XrResult CreateAction(XrActionSet actionSet, XrAction* action, XrActionType type,
                              const char* name, const char* localizedName = nullptr) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    strcpy_s(info.actionName, name);
    if (localizedName) strcpy_s(info.localizedActionName, localizedName);
    info.countSubactionPaths = 0;
    info.subactionPaths = nullptr;
    return xrCreateAction(actionSet, &info, action);
}

static XrResult SuggestBindings(XrInstance instance, const char* profilePath,
                                  XrActionSuggestedBinding* bindings, uint32_t count) {
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    xrStringToPath(instance, profilePath, &suggested.interactionProfile);
    suggested.countSuggestedBindings = count;
    suggested.suggestedBindings = bindings;
    return xrSuggestInteractionProfileBindings(instance, &suggested);
}

bool XRInput::Initialize(XrInstance instance, XrSession session, XrSpace space,
                         bool touchPlusEnabled) {
    m_instance = instance;
    m_session = session;
    m_space = space;

    Log("[XRInput] Initializing...");

    // Create action set
    XrActionSetCreateInfo asInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asInfo.actionSetName, "bl1gotyvr_input");
    strcpy_s(asInfo.localizedActionSetName, "BL1 GOTY VR Input");
    asInfo.priority = 0;
    if (xrCreateActionSet(m_instance, &asInfo, &m_actionSet) != XR_SUCCESS) {
        Log("[XRInput] Failed to create action set");
        return false;
    }

    // Create pose actions
    if (XR_FAILED(CreateAction(m_actionSet, &m_leftPose, XR_ACTION_TYPE_POSE_INPUT,
            "left_hand_pose", "Left Hand Pose")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_rightPose, XR_ACTION_TYPE_POSE_INPUT,
            "right_hand_pose", "Right Hand Pose")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_rightAimPose, XR_ACTION_TYPE_POSE_INPUT,
            "right_aim_pose", "Right Aim Pose"))) return false;

    // Create thumbstick actions
    if (XR_FAILED(CreateAction(m_actionSet, &m_leftThumbstick, XR_ACTION_TYPE_VECTOR2F_INPUT,
            "left_thumbstick", "Left Thumbstick")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_rightThumbstick, XR_ACTION_TYPE_VECTOR2F_INPUT,
            "right_thumbstick", "Right Thumbstick")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_leftThumbstickX, XR_ACTION_TYPE_FLOAT_INPUT,
            "left_thumbstick_x", "Left Thumbstick X")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_leftThumbstickY, XR_ACTION_TYPE_FLOAT_INPUT,
            "left_thumbstick_y", "Left Thumbstick Y")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_rightThumbstickX, XR_ACTION_TYPE_FLOAT_INPUT,
            "right_thumbstick_x", "Right Thumbstick X")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_rightThumbstickY, XR_ACTION_TYPE_FLOAT_INPUT,
            "right_thumbstick_y", "Right Thumbstick Y"))) return false;

    // Create trigger actions
    if (XR_FAILED(CreateAction(m_actionSet, &m_leftTrigger, XR_ACTION_TYPE_FLOAT_INPUT,
            "left_trigger", "Left Trigger")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_rightTrigger, XR_ACTION_TYPE_FLOAT_INPUT,
            "right_trigger", "Right Trigger"))) return false;

    // Create grip actions
    if (XR_FAILED(CreateAction(m_actionSet, &m_leftGrip, XR_ACTION_TYPE_FLOAT_INPUT,
            "left_grip", "Left Grip")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_rightGrip, XR_ACTION_TYPE_FLOAT_INPUT,
            "right_grip", "Right Grip"))) return false;

    // Create button actions
    if (XR_FAILED(CreateAction(m_actionSet, &m_buttonA, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "button_a", "Button A")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_buttonB, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "button_b", "Button B")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_buttonX, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "button_x", "Button X")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_buttonY, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "button_y", "Button Y")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_leftThumbstickClick, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "left_stick_click", "Left Stick Click")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_rightThumbstickClick, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "right_stick_click", "Right Stick Click")) ||
        XR_FAILED(CreateAction(m_actionSet, &m_menuButton, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "menu", "Menu"))) return false;

    auto path = [&](const char* value) {
        XrPath result = XR_NULL_PATH;
        return xrStringToPath(m_instance, value, &result) == XR_SUCCESS
            ? result : XR_NULL_PATH;
    };
    auto suggest = [&](const char* profile, const XrActionSuggestedBinding* values,
                       uint32_t count) {
        XrActionSuggestedBinding valid[32] = {};
        uint32_t validCount = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (values[i].binding != XR_NULL_PATH) valid[validCount++] = values[i];
        const XrResult result = SuggestBindings(m_instance, profile, valid, validCount);
        Log("[XRInput] Bindings %s: result=%d count=%u", profile, (int)result, validCount);
        return XR_SUCCEEDED(result);
    };

    XrActionSuggestedBinding touch[] = {
        {m_leftPose, path("/user/hand/left/input/grip/pose")},
        {m_rightPose, path("/user/hand/right/input/grip/pose")},
        {m_rightAimPose, path("/user/hand/right/input/aim/pose")},
        {m_leftThumbstick, path("/user/hand/left/input/thumbstick")},
        {m_rightThumbstick, path("/user/hand/right/input/thumbstick")},
        {m_leftThumbstickX, path("/user/hand/left/input/thumbstick/x")},
        {m_leftThumbstickY, path("/user/hand/left/input/thumbstick/y")},
        {m_rightThumbstickX, path("/user/hand/right/input/thumbstick/x")},
        {m_rightThumbstickY, path("/user/hand/right/input/thumbstick/y")},
        {m_leftTrigger, path("/user/hand/left/input/trigger/value")},
        {m_rightTrigger, path("/user/hand/right/input/trigger/value")},
        {m_leftGrip, path("/user/hand/left/input/squeeze/value")},
        {m_rightGrip, path("/user/hand/right/input/squeeze/value")},
        {m_buttonA, path("/user/hand/right/input/a/click")},
        {m_buttonB, path("/user/hand/right/input/b/click")},
        {m_buttonX, path("/user/hand/left/input/x/click")},
        {m_buttonY, path("/user/hand/left/input/y/click")},
        {m_leftThumbstickClick, path("/user/hand/left/input/thumbstick/click")},
        {m_rightThumbstickClick, path("/user/hand/right/input/thumbstick/click")},
        {m_menuButton, path("/user/hand/left/input/menu/click")},
    };
    if (!suggest("/interaction_profiles/oculus/touch_controller", touch, _countof(touch)))
        return false;
    if (touchPlusEnabled &&
        !suggest("/interaction_profiles/meta/touch_controller_plus", touch, _countof(touch)))
        return false;

    XrActionSuggestedBinding index[] = {
        {m_leftPose, path("/user/hand/left/input/grip/pose")},
        {m_rightPose, path("/user/hand/right/input/grip/pose")},
        {m_rightAimPose, path("/user/hand/right/input/aim/pose")},
        {m_leftThumbstick, path("/user/hand/left/input/thumbstick")},
        {m_rightThumbstick, path("/user/hand/right/input/thumbstick")},
        {m_leftThumbstickX, path("/user/hand/left/input/thumbstick/x")},
        {m_leftThumbstickY, path("/user/hand/left/input/thumbstick/y")},
        {m_rightThumbstickX, path("/user/hand/right/input/thumbstick/x")},
        {m_rightThumbstickY, path("/user/hand/right/input/thumbstick/y")},
        {m_leftTrigger, path("/user/hand/left/input/trigger/value")},
        {m_rightTrigger, path("/user/hand/right/input/trigger/value")},
        {m_leftGrip, path("/user/hand/left/input/squeeze/value")},
        {m_rightGrip, path("/user/hand/right/input/squeeze/value")},
        {m_buttonA, path("/user/hand/right/input/a/click")},
        {m_buttonB, path("/user/hand/right/input/b/click")},
        {m_leftThumbstickClick, path("/user/hand/left/input/thumbstick/click")},
        {m_rightThumbstickClick, path("/user/hand/right/input/thumbstick/click")},
        {m_menuButton, path("/user/hand/left/input/system/click")},
    };
    suggest("/interaction_profiles/valve/index_controller", index, _countof(index));

    XrActionSuggestedBinding vive[] = {
        {m_leftPose, path("/user/hand/left/input/grip/pose")},
        {m_rightPose, path("/user/hand/right/input/grip/pose")},
        {m_rightAimPose, path("/user/hand/right/input/aim/pose")},
        {m_leftThumbstick, path("/user/hand/left/input/trackpad")},
        {m_rightThumbstick, path("/user/hand/right/input/trackpad")},
        {m_leftThumbstickX, path("/user/hand/left/input/trackpad/x")},
        {m_leftThumbstickY, path("/user/hand/left/input/trackpad/y")},
        {m_rightThumbstickX, path("/user/hand/right/input/trackpad/x")},
        {m_rightThumbstickY, path("/user/hand/right/input/trackpad/y")},
        {m_leftTrigger, path("/user/hand/left/input/trigger/value")},
        {m_rightTrigger, path("/user/hand/right/input/trigger/value")},
        {m_menuButton, path("/user/hand/left/input/menu/click")},
    };
    suggest("/interaction_profiles/htc/vive_controller", vive, _countof(vive));

    // Create pose spaces
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = m_leftPose;
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateActionSpace(m_session, &spaceInfo, &m_leftPoseSpace))) return false;

    spaceInfo.action = m_rightPose;
    if (XR_FAILED(xrCreateActionSpace(m_session, &spaceInfo, &m_rightPoseSpace))) return false;
    spaceInfo.action = m_rightAimPose;
    if (XR_FAILED(xrCreateActionSpace(m_session, &spaceInfo, &m_rightAimPoseSpace))) return false;

    // Attach action set
    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &m_actionSet;
    if (XR_FAILED(xrAttachSessionActionSets(m_session, &attachInfo))) return false;

    Log("[XRInput] Initialized (20 actions, Quest A/B/X/Y, scalar stick fallback, "
        "TouchPlus=%d)", touchPlusEnabled ? 1 : 0);
    return true;
}

void XRInput::Shutdown() {
    XInputBridge::Instance().ReleaseVrState();
    AcquireSRWLockExclusive(&m_controllerLock);
    if (m_leftPoseSpace != XR_NULL_HANDLE) { xrDestroySpace(m_leftPoseSpace); m_leftPoseSpace = XR_NULL_HANDLE; }
    if (m_rightPoseSpace != XR_NULL_HANDLE) { xrDestroySpace(m_rightPoseSpace); m_rightPoseSpace = XR_NULL_HANDLE; }
    if (m_rightAimPoseSpace != XR_NULL_HANDLE) { xrDestroySpace(m_rightAimPoseSpace); m_rightAimPoseSpace = XR_NULL_HANDLE; }
    if (m_actionSet != XR_NULL_HANDLE) { xrDestroyActionSet(m_actionSet); m_actionSet = XR_NULL_HANDLE; }
    m_instance = XR_NULL_HANDLE;
    m_session = XR_NULL_HANDLE;
    m_space = XR_NULL_HANDLE;
    m_controllers[0] = {};
    m_controllers[1] = {};
    ++m_generation;
    ReleaseSRWLockExclusive(&m_controllerLock);
    Log("[XRInput] Shutdown");
}

bool XRInput::SyncActions() {
    if (!m_session || !m_actionSet) return false;
    XrActiveActionSet activeSet{};
    activeSet.actionSet = m_actionSet;
    activeSet.subactionPath = XR_NULL_PATH;
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    const XrResult result = xrSyncActions(m_session, &syncInfo);
    const bool synchronized = XR_SUCCEEDED(result);
    if (!synchronized) {
        static uint64_t failureCount = 0;
        if (failureCount++ == 0 || failureCount % 300 == 0)
            Log("[XRInput] xrSyncActions failed: result=%d count=%llu", (int)result,
                static_cast<unsigned long long>(failureCount));
        AcquireSRWLockExclusive(&m_controllerLock);
        m_controllers[0] = {};
        m_controllers[1] = {};
        ++m_generation;
        ReleaseSRWLockExclusive(&m_controllerLock);
    }
    return synchronized;
}

void XRInput::UpdateControllerStateUnlocked(int hand, XrTime displayTime) {
    if (hand < 0 || hand > 1 || !m_session) return;
    ControllerState& ctrl = m_controllers[hand];
    XrAction poseAction = (hand == 0) ? m_leftPose : m_rightPose;
    XrAction thumbstickAction = (hand == 0) ? m_leftThumbstick : m_rightThumbstick;
    XrAction thumbstickXAction = (hand == 0) ? m_leftThumbstickX : m_rightThumbstickX;
    XrAction thumbstickYAction = (hand == 0) ? m_leftThumbstickY : m_rightThumbstickY;
    XrAction triggerAction = (hand == 0) ? m_leftTrigger : m_rightTrigger;
    XrAction gripAction = (hand == 0) ? m_leftGrip : m_rightGrip;
    XrSpace poseSpace = (hand == 0) ? m_leftPoseSpace : m_rightPoseSpace;
    XrAction stickClickAction = (hand == 0) ? m_leftThumbstickClick : m_rightThumbstickClick;

    // Get pose
    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = poseAction;
    getInfo.subactionPath = XR_NULL_PATH;

    XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
    const XrResult poseResult = xrGetActionStatePose(m_session, &getInfo, &poseState);
    XrResult locationResult = XR_SUCCESS;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};

    if (poseState.isActive) {
        locationResult = xrLocateSpace(poseSpace, m_space, displayTime, &location);
        const bool positionValid =
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
        const bool orientationValid =
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        if (positionValid) {
            ctrl.position[0] = location.pose.position.x;
            ctrl.position[1] = location.pose.position.y;
            ctrl.position[2] = location.pose.position.z;
        }
        if (orientationValid) {
            ctrl.rotation[0] = location.pose.orientation.x;
            ctrl.rotation[1] = location.pose.orientation.y;
            ctrl.rotation[2] = location.pose.orientation.z;
            ctrl.rotation[3] = location.pose.orientation.w;
            float qx = ctrl.rotation[0], qy = ctrl.rotation[1], qz = ctrl.rotation[2], qw = ctrl.rotation[3];
            const float pitchTerm = (std::max)(-1.0f,
                (std::min)(1.0f, -2.0f * (qx*qz - qw*qy)));
            ctrl.eulerPitch = asinf(pitchTerm) * 57.2957795f;
            ctrl.eulerYaw = atan2f(2.0f*(qw*qz + qx*qy), 1.0f - 2.0f*(qy*qy + qz*qz)) * 57.2957795f;
            ctrl.eulerRoll = atan2f(2.0f*(qw*qx + qy*qz), 1.0f - 2.0f*(qx*qx + qy*qy)) * 57.2957795f;
        }
        ctrl.valid = positionValid && orientationValid;
    } else {
        ctrl.valid = false;
    }

    ctrl.aimValid = false;
    if (hand == 1 && m_rightAimPose != XR_NULL_HANDLE &&
        m_rightAimPoseSpace != XR_NULL_HANDLE) {
        getInfo.action = m_rightAimPose;
        XrActionStatePose aimState{XR_TYPE_ACTION_STATE_POSE};
        if (XR_SUCCEEDED(xrGetActionStatePose(m_session, &getInfo, &aimState)) &&
            aimState.isActive) {
            XrSpaceLocation aimLocation{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(m_rightAimPoseSpace, m_space,
                                           displayTime, &aimLocation)) &&
                (aimLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (aimLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                ctrl.aimPosition[0] = aimLocation.pose.position.x;
                ctrl.aimPosition[1] = aimLocation.pose.position.y;
                ctrl.aimPosition[2] = aimLocation.pose.position.z;
                ctrl.aimRotation[0] = aimLocation.pose.orientation.x;
                ctrl.aimRotation[1] = aimLocation.pose.orientation.y;
                ctrl.aimRotation[2] = aimLocation.pose.orientation.z;
                ctrl.aimRotation[3] = aimLocation.pose.orientation.w;
                ctrl.aimValid = true;
            }
        }
    }
    if (!ctrl.aimValid && ctrl.valid) {
        memcpy(ctrl.aimPosition, ctrl.position, sizeof(ctrl.aimPosition));
        memcpy(ctrl.aimRotation, ctrl.rotation, sizeof(ctrl.aimRotation));
        ctrl.aimValid = true;
    }

    // Get thumbstick
    ctrl.thumbstickX = 0.0f;
    ctrl.thumbstickY = 0.0f;
    getInfo.action = thumbstickAction;
    XrActionStateVector2f thumbState{XR_TYPE_ACTION_STATE_VECTOR2F};
    xrGetActionStateVector2f(m_session, &getInfo, &thumbState);
    if (thumbState.isActive) {
        ctrl.thumbstickX = thumbState.currentState.x;
        ctrl.thumbstickY = thumbState.currentState.y;
    }
    XrActionStateFloat thumbXState{XR_TYPE_ACTION_STATE_FLOAT};
    getInfo.action = thumbstickXAction;
    const XrResult thumbXResult = xrGetActionStateFloat(m_session, &getInfo, &thumbXState);
    if (XR_SUCCEEDED(thumbXResult) && thumbXState.isActive)
        ctrl.thumbstickX = thumbXState.currentState;
    XrActionStateFloat thumbYState{XR_TYPE_ACTION_STATE_FLOAT};
    getInfo.action = thumbstickYAction;
    const XrResult thumbYResult = xrGetActionStateFloat(m_session, &getInfo, &thumbYState);
    if (XR_SUCCEEDED(thumbYResult) && thumbYState.isActive)
        ctrl.thumbstickY = thumbYState.currentState;

    // Get trigger
    ctrl.trigger = 0.0f;
    getInfo.action = triggerAction;
    XrActionStateFloat triggerState{XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(m_session, &getInfo, &triggerState);
    if (triggerState.isActive) ctrl.trigger = triggerState.currentState;

    // Get grip
    ctrl.grip = 0.0f;
    getInfo.action = gripAction;
    XrActionStateFloat gripState{XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(m_session, &getInfo, &gripState);
    if (gripState.isActive) ctrl.grip = gripState.currentState;

    // Get buttons
    XrActionStateBoolean boolState{XR_TYPE_ACTION_STATE_BOOLEAN};

    getInfo.action = stickClickAction;
    ctrl.thumbstickClick = XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &boolState)) &&
        boolState.isActive && boolState.currentState;

    auto getButton = [&](XrAction action) {
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        getInfo.action = action;
        return XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &state)) &&
            state.isActive && state.currentState;
    };
    ctrl.buttonA = hand == 1 && getButton(m_buttonA);
    ctrl.buttonB = hand == 1 && getButton(m_buttonB);
    ctrl.buttonX = hand == 0 && getButton(m_buttonX);
    ctrl.buttonY = hand == 0 && getButton(m_buttonY);
    ctrl.menuButton = hand == 0 && getButton(m_menuButton);

    static uint64_t updateCounts[2] = {};
    const uint64_t updateCount = ++updateCounts[hand];
    if (updateCount == 1 || updateCount % 300 == 0) {
        Log("[XRInput] hand=%s poseResult=%d active=%d locateResult=%d flags=0x%llX "
            "valid=%d stickActive=%d scalarActive=%d/%d triggerActive=%d gripActive=%d "
            "buttons=%d%d%d%d%d%d values=(%.2f,%.2f %.2f %.2f)",
            hand == 0 ? "left" : "right", (int)poseResult, poseState.isActive,
            (int)locationResult, static_cast<unsigned long long>(location.locationFlags),
            ctrl.valid, thumbState.isActive, thumbXState.isActive, thumbYState.isActive,
            triggerState.isActive, gripState.isActive,
            ctrl.buttonA, ctrl.buttonB, ctrl.buttonX, ctrl.buttonY,
            ctrl.thumbstickClick, ctrl.menuButton,
            ctrl.thumbstickX, ctrl.thumbstickY, ctrl.trigger, ctrl.grip);
    }
}

void XRInput::LogCurrentInteractionProfiles() const {
    if (m_session == XR_NULL_HANDLE || m_instance == XR_NULL_HANDLE) return;
    const char* hands[] = {"left", "right"};
    const char* paths[] = {"/user/hand/left", "/user/hand/right"};
    for (int hand = 0; hand < 2; ++hand) {
        XrPath userPath = XR_NULL_PATH;
        if (xrStringToPath(m_instance, paths[hand], &userPath) != XR_SUCCESS) continue;
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        const XrResult result = xrGetCurrentInteractionProfile(m_session, userPath, &state);
        char profile[XR_MAX_PATH_LENGTH] = {};
        uint32_t length = 0;
        if (result == XR_SUCCESS && state.interactionProfile != XR_NULL_PATH)
            xrPathToString(m_instance, state.interactionProfile, sizeof(profile), &length, profile);
        Log("[XRInput] Current %s interaction profile: result=%d path='%s'",
            hands[hand], static_cast<int>(result), profile[0] ? profile : "<none>");
    }
}

void XRInput::UpdateControllerStates(XrTime displayTime) {
    AcquireSRWLockExclusive(&m_controllerLock);
    UpdateControllerStateUnlocked(0, displayTime);
    UpdateControllerStateUnlocked(1, displayTime);
    ++m_generation;
    ReleaseSRWLockExclusive(&m_controllerLock);
}

bool XRInput::GetControllerSnapshot(ControllerState controllers[2],
                                    uint64_t* generation) const {
    if (!controllers) return false;
    AcquireSRWLockShared(&m_controllerLock);
    controllers[0] = m_controllers[0];
    controllers[1] = m_controllers[1];
    if (generation) *generation = m_generation;
    const bool anyValid = controllers[0].valid || controllers[1].valid;
    ReleaseSRWLockShared(&m_controllerLock);
    return anyValid;
}

}} // namespace bl1gotyvr::input
