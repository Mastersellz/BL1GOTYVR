#include "XRInput.hpp"
#include "../core/VRMod.hpp"
#include <cstring>
#include <cmath>
#include <cstdio>

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

bool XRInput::Initialize(XrInstance instance, XrSession session, XrSpace space) {
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
    CreateAction(m_actionSet, &m_leftPose, XR_ACTION_TYPE_POSE_INPUT, "left_hand_pose", "Left Hand Pose");
    CreateAction(m_actionSet, &m_rightPose, XR_ACTION_TYPE_POSE_INPUT, "right_hand_pose", "Right Hand Pose");

    // Create thumbstick actions
    CreateAction(m_actionSet, &m_leftThumbstick, XR_ACTION_TYPE_VECTOR2F_INPUT, "left_thumbstick", "Left Thumbstick");
    CreateAction(m_actionSet, &m_rightThumbstick, XR_ACTION_TYPE_VECTOR2F_INPUT, "right_thumbstick", "Right Thumbstick");

    // Create trigger actions
    CreateAction(m_actionSet, &m_leftTrigger, XR_ACTION_TYPE_FLOAT_INPUT, "left_trigger", "Left Trigger");
    CreateAction(m_actionSet, &m_rightTrigger, XR_ACTION_TYPE_FLOAT_INPUT, "right_trigger", "Right Trigger");

    // Create grip actions
    CreateAction(m_actionSet, &m_leftGrip, XR_ACTION_TYPE_FLOAT_INPUT, "left_grip", "Left Grip");
    CreateAction(m_actionSet, &m_rightGrip, XR_ACTION_TYPE_FLOAT_INPUT, "right_grip", "Right Grip");

    // Create button actions
    CreateAction(m_actionSet, &m_buttonA, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "Button A");
    CreateAction(m_actionSet, &m_buttonB, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "Button B");
    CreateAction(m_actionSet, &m_leftThumbstickClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "left_stick_click", "Left Stick Click");
    CreateAction(m_actionSet, &m_rightThumbstickClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "right_stick_click", "Right Stick Click");
    CreateAction(m_actionSet, &m_menuButton, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu");

    // Suggest bindings for Oculus Touch, Valve Index, HTC Vive
    const char* profiles[] = {
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/valve/index_controller",
        "/interaction_profiles/htc/vive_controller"
    };

    for (const char* profile : profiles) {
        XrActionSuggestedBinding bindings[14];
        uint32_t idx = 0;

        XrPath leftPath, rightPath;
        xrStringToPath(m_instance, "/user/hand/left", &leftPath);
        xrStringToPath(m_instance, "/user/hand/right", &rightPath);

        char pathStr[256];

        // Poses
        sprintf_s(pathStr, "%s/input/grip/pose", profile);
        XrPath bindPath;
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_leftPose, bindPath};
        sprintf_s(pathStr, "%s/input/grip/pose", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_rightPose, bindPath};

        // Thumbsticks
        sprintf_s(pathStr, "%s/input/thumbstick", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_leftThumbstick, bindPath};
        sprintf_s(pathStr, "%s/input/thumbstick", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_rightThumbstick, bindPath};

        // Triggers
        sprintf_s(pathStr, "%s/input/trigger/value", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_leftTrigger, bindPath};
        sprintf_s(pathStr, "%s/input/trigger/value", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_rightTrigger, bindPath};

        // Grips
        sprintf_s(pathStr, "%s/input/squeeze/value", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_leftGrip, bindPath};
        sprintf_s(pathStr, "%s/input/squeeze/value", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_rightGrip, bindPath};

        // Buttons
        sprintf_s(pathStr, "%s/input/a/click", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_buttonA, bindPath};
        sprintf_s(pathStr, "%s/input/b/click", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_buttonB, bindPath};
        sprintf_s(pathStr, "%s/input/thumbstick/click", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_leftThumbstickClick, bindPath};
        sprintf_s(pathStr, "%s/input/thumbstick/click", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_rightThumbstickClick, bindPath};
        sprintf_s(pathStr, "%s/input/menu/click", profile);
        xrStringToPath(m_instance, pathStr, &bindPath);
        bindings[idx++] = {m_menuButton, bindPath};

        SuggestBindings(m_instance, profile, bindings, idx);
    }

    // Create pose spaces
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = m_leftPose;
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    xrCreateActionSpace(m_session, &spaceInfo, &m_leftPoseSpace);

    spaceInfo.action = m_rightPose;
    xrCreateActionSpace(m_session, &spaceInfo, &m_rightPoseSpace);

    // Attach action set
    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &m_actionSet;
    xrAttachSessionActionSets(m_session, &attachInfo);

    Log("[XRInput] Initialized (14 actions created)");
    return true;
}

void XRInput::Shutdown() {
    if (m_leftPoseSpace != XR_NULL_HANDLE) { xrDestroySpace(m_leftPoseSpace); m_leftPoseSpace = XR_NULL_HANDLE; }
    if (m_rightPoseSpace != XR_NULL_HANDLE) { xrDestroySpace(m_rightPoseSpace); m_rightPoseSpace = XR_NULL_HANDLE; }
    if (m_actionSet != XR_NULL_HANDLE) { xrDestroyActionSet(m_actionSet); m_actionSet = XR_NULL_HANDLE; }
    m_instance = XR_NULL_HANDLE;
    m_session = XR_NULL_HANDLE;
    m_space = XR_NULL_HANDLE;
    Log("[XRInput] Shutdown");
}

void XRInput::SyncActions() {
    if (!m_session) return;
    XrActiveActionSet activeSet{};
    activeSet.actionSet = m_actionSet;
    activeSet.subactionPath = XR_NULL_PATH;
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    xrSyncActions(m_session, &syncInfo);
}

void XRInput::UpdateControllerState(int hand) {
    if (hand < 0 || hand > 1 || !m_session) return;
    ControllerState& ctrl = m_controllers[hand];
    XrAction poseAction = (hand == 0) ? m_leftPose : m_rightPose;
    XrAction thumbstickAction = (hand == 0) ? m_leftThumbstick : m_rightThumbstick;
    XrAction triggerAction = (hand == 0) ? m_leftTrigger : m_rightTrigger;
    XrAction gripAction = (hand == 0) ? m_leftGrip : m_rightGrip;
    XrSpace poseSpace = (hand == 0) ? m_leftPoseSpace : m_rightPoseSpace;
    XrAction stickClickAction = (hand == 0) ? m_leftThumbstickClick : m_rightThumbstickClick;

    // Get pose
    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = poseAction;
    getInfo.subactionPath = XR_NULL_PATH;

    XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
    xrGetActionStatePose(m_session, &getInfo, &poseState);

    if (poseState.isActive) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(poseSpace, m_space, 0, &location);
        if (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
            ctrl.position[0] = location.pose.position.x;
            ctrl.position[1] = location.pose.position.y;
            ctrl.position[2] = location.pose.position.z;
        }
        if (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
            ctrl.rotation[0] = location.pose.orientation.x;
            ctrl.rotation[1] = location.pose.orientation.y;
            ctrl.rotation[2] = location.pose.orientation.z;
            ctrl.rotation[3] = location.pose.orientation.w;
            float qx = ctrl.rotation[0], qy = ctrl.rotation[1], qz = ctrl.rotation[2], qw = ctrl.rotation[3];
            ctrl.eulerPitch = asinf(-2.0f * (qx*qz - qw*qy)) * 57.2957795f;
            ctrl.eulerYaw = atan2f(2.0f*(qw*qz + qx*qy), 1.0f - 2.0f*(qy*qy + qz*qz)) * 57.2957795f;
            ctrl.eulerRoll = atan2f(2.0f*(qw*qx + qy*qz), 1.0f - 2.0f*(qx*qx + qy*qy)) * 57.2957795f;
        }
        ctrl.valid = true;
    } else {
        ctrl.valid = false;
    }

    // Get thumbstick
    getInfo.action = thumbstickAction;
    XrActionStateVector2f thumbState{XR_TYPE_ACTION_STATE_VECTOR2F};
    xrGetActionStateVector2f(m_session, &getInfo, &thumbState);
    if (thumbState.isActive) {
        ctrl.thumbstickX = thumbState.currentState.x;
        ctrl.thumbstickY = thumbState.currentState.y;
    }

    // Get trigger
    getInfo.action = triggerAction;
    XrActionStateFloat triggerState{XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(m_session, &getInfo, &triggerState);
    if (triggerState.isActive) ctrl.trigger = triggerState.currentState;

    // Get grip
    getInfo.action = gripAction;
    XrActionStateFloat gripState{XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(m_session, &getInfo, &gripState);
    if (gripState.isActive) ctrl.grip = gripState.currentState;

    // Get buttons
    XrActionStateBoolean boolState{XR_TYPE_ACTION_STATE_BOOLEAN};

    getInfo.action = stickClickAction;
    xrGetActionStateBoolean(m_session, &getInfo, &boolState);
    ctrl.thumbstickClick = boolState.isActive && boolState.currentState;

    getInfo.action = m_buttonA;
    xrGetActionStateBoolean(m_session, &getInfo, &boolState);
    ctrl.buttonA = boolState.isActive && boolState.currentState;

    getInfo.action = m_buttonB;
    xrGetActionStateBoolean(m_session, &getInfo, &boolState);
    ctrl.buttonB = boolState.isActive && boolState.currentState;

    getInfo.action = m_menuButton;
    xrGetActionStateBoolean(m_session, &getInfo, &boolState);
    ctrl.menuButton = boolState.isActive && boolState.currentState;
}

}} // namespace bl1gotyvr::input
