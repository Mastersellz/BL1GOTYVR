#include "Config.hpp"
#include "../core/VRMod.hpp"
#include <Windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cmath>

namespace bl1gotyvr { namespace config {

static Settings s_settings;
static char s_configPath[MAX_PATH] = {};
static FILETIME s_lastWriteTime = {};
static ULONGLONG s_lastReloadCheck = 0;

Settings& Get() { return s_settings; }

static float ReadFloat(const char* section, const char* key, float fallback, const char* path) {
    char fallbackText[32] = {};
    char value[32] = {};
    sprintf_s(fallbackText, "%.4f", fallback);
    GetPrivateProfileStringA(section, key, fallbackText, value, sizeof(value), path);
    return static_cast<float>(atof(value));
}

void Load(const char* path) {
    if (!path || !*path) return;
    if (path != s_configPath) strcpy_s(s_configPath, path);
    s_settings.render_width = std::clamp(
        static_cast<int>(GetPrivateProfileIntA("Display", "Width", s_settings.render_width, path)), 640, 7680);
    s_settings.render_height = std::clamp(
        static_cast<int>(GetPrivateProfileIntA("Display", "Height", s_settings.render_height, path)), 480, 4320);
    s_settings.resolution_scale = std::clamp(
        ReadFloat("Display", "ResolutionScale", s_settings.resolution_scale, path), 0.5f, 2.0f);
    s_settings.fov_degrees = std::clamp(
        ReadFloat("Display", "FOV", s_settings.fov_degrees, path), 60.0f, 150.0f);
    s_settings.ipd_mm = std::clamp(
        ReadFloat("Stereo", "IPD", s_settings.ipd_mm, path), 50.0f, 80.0f);
    s_settings.convergence_m = std::clamp(
        ReadFloat("Stereo", "Convergence", s_settings.convergence_m, path), 0.0f, 20.0f);
    s_settings.openxr_refresh_rate_hz = std::clamp(
        ReadFloat("OpenXR", "RefreshRateHz", s_settings.openxr_refresh_rate_hz, path),
        0.0f, 240.0f);
    s_settings.near_plane = std::clamp(
        ReadFloat("Rendering", "NearPlane", s_settings.near_plane, path), 0.01f, 10.0f);
    s_settings.far_plane = std::clamp(
        ReadFloat("Rendering", "FarPlane", s_settings.far_plane, path), 100.0f, 100000.0f);
    s_settings.positional_scale = std::clamp(
        ReadFloat("Tracking", "PositionScale", s_settings.positional_scale, path), 0.0f, 5.0f);
    s_settings.rotation_scale = std::clamp(
        ReadFloat("Tracking", "RotationScale", s_settings.rotation_scale, path), 0.0f, 5.0f);
    s_settings.head_yaw_scale = std::clamp(
        ReadFloat("Tracking", "HeadYawScale", s_settings.head_yaw_scale, path), 0.0f, 1.0f);
    s_settings.head_pitch_scale = std::clamp(
        ReadFloat("Tracking", "HeadPitchScale", s_settings.head_pitch_scale, path), 0.0f, 1.0f);
    s_settings.roll_enabled = GetPrivateProfileIntA(
        "Tracking", "RollEnabled", s_settings.roll_enabled, path) != 0;
    s_settings.same_frame_stereo_requested = GetPrivateProfileIntA(
        "Stereo", "SameFrameStereo", s_settings.same_frame_stereo_requested, path) != 0;
    // Runtime enables this only after both the viewport boundary and camera
    // cache are validated. Until then AFR is the only safe capture path.
    s_settings.same_frame_stereo = false;
    s_settings.reverse_eyes = GetPrivateProfileIntA(
        "Stereo", "ReverseEyes", s_settings.reverse_eyes, path) != 0;
    s_settings.debug_logging = GetPrivateProfileIntA(
        "Debug", "Logging", s_settings.debug_logging, path) != 0;
    s_settings.downsample_threshold = std::clamp(static_cast<int>(GetPrivateProfileIntA(
        "Rendering", "DownsampleThreshold", s_settings.downsample_threshold, path)), 1280, 7680);

    s_settings.hud_enabled = GetPrivateProfileIntA(
        "HUD", "Enabled", s_settings.hud_enabled, path) != 0;
    s_settings.hud_distance = std::clamp(
        ReadFloat("HUD", "Distance", s_settings.hud_distance, path), 0.5f, 10.0f);
    s_settings.hud_width_degrees = std::clamp(
        ReadFloat("HUD", "WidthDegrees", s_settings.hud_width_degrees, path), 10.0f, 150.0f);
    s_settings.hud_scale = std::clamp(
        ReadFloat("HUD", "Scale", s_settings.hud_scale, path), 0.25f, 2.0f);
    s_settings.hud_opacity = std::clamp(
        ReadFloat("HUD", "Opacity", s_settings.hud_opacity, path), 0.0f, 1.0f);
    s_settings.hud_horizontal_offset = std::clamp(
        ReadFloat("HUD", "HorizontalOffset", s_settings.hud_horizontal_offset, path), -2.0f, 2.0f);
    s_settings.hud_vertical_offset = std::clamp(
        ReadFloat("HUD", "VerticalOffset", s_settings.hud_vertical_offset, path), -2.0f, 2.0f);

    s_settings.debug_force_no_hud_layer = GetPrivateProfileIntA(
        "Debug", "ForceNoHudLayer", s_settings.debug_force_no_hud_layer, path) != 0;

    /* Input / Locomotion */
    s_settings.turn_mode = static_cast<int>(std::clamp<int>(
        GetPrivateProfileIntA("Input", "TurnMode", s_settings.turn_mode, path), 0, 2));
    s_settings.snap_turn_angle = std::clamp(
        ReadFloat("Input", "SnapTurnAngle", s_settings.snap_turn_angle, path), 5.0f, 90.0f);
    s_settings.smooth_turn_speed = std::clamp(
        ReadFloat("Input", "SmoothTurnSpeed", s_settings.smooth_turn_speed, path), 10.0f, 360.0f);
    s_settings.locomotion_deadzone = std::clamp(
        ReadFloat("Input", "LocomotionDeadzone", s_settings.locomotion_deadzone, path), 0.0f, 0.5f);
    s_settings.room_scale_enabled = GetPrivateProfileIntA(
        "RoomScale", "Enabled", s_settings.room_scale_enabled, path) != 0;
    s_settings.room_scale_allow_horizontal = GetPrivateProfileIntA(
        "RoomScale", "AllowHorizontal", s_settings.room_scale_allow_horizontal, path) != 0;
    s_settings.room_scale_allow_vertical = GetPrivateProfileIntA(
        "RoomScale", "AllowVertical", s_settings.room_scale_allow_vertical, path) != 0;
    s_settings.room_scale_follow_strength = std::clamp(ReadFloat(
        "RoomScale", "BodyFollowStrength", s_settings.room_scale_follow_strength, path),
        0.0f, 60.0f);
    s_settings.room_scale_calibrated_height = std::clamp(ReadFloat(
        "RoomScale", "CalibratedHeight", s_settings.room_scale_calibrated_height, path),
        80.0f, 260.0f);
    s_settings.room_scale_head_to_chest = std::clamp(ReadFloat(
        "RoomScale", "HeadToChest", s_settings.room_scale_head_to_chest, path),
        10.0f, 100.0f);
    s_settings.room_scale_head_to_pelvis = std::clamp(ReadFloat(
        "RoomScale", "HeadToPelvis", s_settings.room_scale_head_to_pelvis, path),
        20.0f, 150.0f);
    s_settings.room_scale_shoulder_width = std::clamp(ReadFloat(
        "RoomScale", "ShoulderWidth", s_settings.room_scale_shoulder_width, path),
        20.0f, 80.0f);
    s_settings.room_scale_standing_threshold = std::clamp(ReadFloat(
        "RoomScale", "StandingThreshold", s_settings.room_scale_standing_threshold, path),
        0.40f, 1.10f);
    s_settings.room_scale_prone_threshold = std::clamp(ReadFloat(
        "RoomScale", "ProneThreshold", s_settings.room_scale_prone_threshold, path),
        0.10f, s_settings.room_scale_standing_threshold);
    s_settings.room_scale_pose_hysteresis = std::clamp(ReadFloat(
        "RoomScale", "PoseHysteresis", s_settings.room_scale_pose_hysteresis, path),
        0.0f, 0.20f);
    s_settings.debug_room_scale = GetPrivateProfileIntA(
        "Debug", "RoomScale", s_settings.debug_room_scale, path) != 0;

    /* Weapon 6DoF */
    s_settings.weapon_position_scale = std::clamp(
        ReadFloat("Weapon", "PositionScale", s_settings.weapon_position_scale, path), 0.0f, 1.0f);
    s_settings.aim_pitch_degrees = std::clamp(
        ReadFloat("Weapon", "AimPitch", s_settings.aim_pitch_degrees, path), -45.0f, 45.0f);
    s_settings.aim_yaw_degrees = std::clamp(
        ReadFloat("Weapon", "AimYaw", s_settings.aim_yaw_degrees, path), -45.0f, 45.0f);
    s_settings.aim_convergence_m = std::clamp(
        ReadFloat("Dot", "ConvergenceDistance", s_settings.aim_convergence_m, path),
        1.0f, 100.0f);
    s_settings.dot_distance_m = std::clamp(
        ReadFloat("Dot", "DistanceMeters", s_settings.dot_distance_m, path),
        0.5f, 20.0f);
    s_settings.weapon_offset_forward = std::clamp(
        ReadFloat("Weapon", "OffsetForward", s_settings.weapon_offset_forward, path), -100.0f, 100.0f);
    s_settings.weapon_offset_right = std::clamp(
        ReadFloat("Weapon", "OffsetRight", s_settings.weapon_offset_right, path), -100.0f, 100.0f);
    s_settings.weapon_offset_up = std::clamp(
        ReadFloat("Weapon", "OffsetUp", s_settings.weapon_offset_up, path), -100.0f, 100.0f);
    s_settings.weapon_rotation_pitch = std::clamp(
        ReadFloat("Weapon", "RotationPitch", s_settings.weapon_rotation_pitch, path), -180.0f, 180.0f);
    s_settings.weapon_rotation_yaw = std::clamp(
        ReadFloat("Weapon", "RotationYaw", s_settings.weapon_rotation_yaw, path), -180.0f, 180.0f);
    s_settings.weapon_rotation_roll = std::clamp(
        ReadFloat("Weapon", "RotationRoll", s_settings.weapon_rotation_roll, path), -180.0f, 180.0f);

    s_settings.left_hand_offset_forward = std::clamp(
        ReadFloat("Hands", "LeftForward", s_settings.left_hand_offset_forward, path), -100.0f, 100.0f);
    s_settings.left_hand_offset_right = std::clamp(
        ReadFloat("Hands", "LeftRight", s_settings.left_hand_offset_right, path), -100.0f, 100.0f);
    s_settings.left_hand_offset_up = std::clamp(
        ReadFloat("Hands", "LeftUp", s_settings.left_hand_offset_up, path), -100.0f, 100.0f);
    s_settings.left_hand_rotation_pitch = std::clamp(
        ReadFloat("Hands", "LeftPitch", s_settings.left_hand_rotation_pitch, path), -180.0f, 180.0f);
    s_settings.left_hand_rotation_yaw = std::clamp(
        ReadFloat("Hands", "LeftYaw", s_settings.left_hand_rotation_yaw, path), -180.0f, 180.0f);
    s_settings.left_hand_rotation_roll = std::clamp(
        ReadFloat("Hands", "LeftRoll", s_settings.left_hand_rotation_roll, path), -180.0f, 180.0f);
    s_settings.right_hand_offset_forward = std::clamp(
        ReadFloat("Hands", "RightForward", s_settings.right_hand_offset_forward, path), -100.0f, 100.0f);
    s_settings.right_hand_offset_right = std::clamp(
        ReadFloat("Hands", "RightRight", s_settings.right_hand_offset_right, path), -100.0f, 100.0f);
    s_settings.right_hand_offset_up = std::clamp(
        ReadFloat("Hands", "RightUp", s_settings.right_hand_offset_up, path), -100.0f, 100.0f);
    s_settings.right_hand_rotation_pitch = std::clamp(
        ReadFloat("Hands", "RightPitch", s_settings.right_hand_rotation_pitch, path), -180.0f, 180.0f);
    s_settings.right_hand_rotation_yaw = std::clamp(
        ReadFloat("Hands", "RightYaw", s_settings.right_hand_rotation_yaw, path), -180.0f, 180.0f);
    s_settings.right_hand_rotation_roll = std::clamp(
        ReadFloat("Hands", "RightRoll", s_settings.right_hand_rotation_roll, path), -180.0f, 180.0f);

    s_settings.dot_horizontal_offset = std::clamp(
        ReadFloat("Dot", "HorizontalOffset", s_settings.dot_horizontal_offset, path), -0.25f, 0.25f);
    s_settings.dot_vertical_offset = std::clamp(
        ReadFloat("Dot", "VerticalOffset", s_settings.dot_vertical_offset, path), -0.25f, 0.25f);

    s_settings.hide_player_body_and_arms = GetPrivateProfileIntA(
        "Visibility", "HidePlayerBodyAndArms", 0, path) != 0;
    s_settings.vanilla_hands_filter = GetPrivateProfileIntA(
        "Visibility", "VanillaHandsFilter",
        s_settings.vanilla_hands_filter ? 1 : 0, path) != 0;
    s_settings.vanilla_hands_cut_threshold = std::clamp(ReadFloat(
        "Visibility", "VanillaHandsCutThreshold",
        s_settings.vanilla_hands_cut_threshold, path), 20.0f, 90.0f);

    Log("[Config] Loaded %s: %dx%d scale=%.2f FOV=%.1f IPD=%.1fmm "
        "convergenceShift=%.2f%% refresh=%.1fHz aim=(%.2f,%.2f) dot=%.1fm",
        path, s_settings.render_width, s_settings.render_height, s_settings.resolution_scale,
        s_settings.fov_degrees, s_settings.ipd_mm, s_settings.convergence_m,
        s_settings.openxr_refresh_rate_hz,
        s_settings.aim_pitch_degrees, s_settings.aim_yaw_degrees,
        s_settings.dot_distance_m);
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &attributes))
        s_lastWriteTime = attributes.ftLastWriteTime;
}

bool ReloadIfChanged() {
    const ULONGLONG now = GetTickCount64();
    if (!s_configPath[0] || now - s_lastReloadCheck < 500) return false;
    s_lastReloadCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExA(s_configPath, GetFileExInfoStandard, &attributes) ||
        CompareFileTime(&attributes.ftLastWriteTime, &s_lastWriteTime) == 0) return false;
    Load(s_configPath);
    Log("[Config] Runtime settings reloaded");
    return true;
}

static void WriteFloat(const char* section, const char* key, float value, const char* path) {
    char text[32] = {};
    sprintf_s(text, "%.4f", value);
    WritePrivateProfileStringA(section, key, text, path);
}

void Save(const char* path) {
    if (!path || !*path) return;
    char text[32] = {};
    sprintf_s(text, "%d", s_settings.render_width);
    WritePrivateProfileStringA("Display", "Width", text, path);
    sprintf_s(text, "%d", s_settings.render_height);
    WritePrivateProfileStringA("Display", "Height", text, path);
    WriteFloat("Display", "ResolutionScale", s_settings.resolution_scale, path);
    WriteFloat("Display", "FOV", s_settings.fov_degrees, path);
    WriteFloat("Stereo", "IPD", s_settings.ipd_mm, path);
    WriteFloat("Stereo", "Convergence", s_settings.convergence_m, path);
    WriteFloat("OpenXR", "RefreshRateHz", s_settings.openxr_refresh_rate_hz, path);
    WriteFloat("Rendering", "NearPlane", s_settings.near_plane, path);
    WriteFloat("Rendering", "FarPlane", s_settings.far_plane, path);
    WriteFloat("Tracking", "PositionScale", s_settings.positional_scale, path);
    WriteFloat("Tracking", "RotationScale", s_settings.rotation_scale, path);
    WriteFloat("Tracking", "HeadYawScale", s_settings.head_yaw_scale, path);
    WriteFloat("Tracking", "HeadPitchScale", s_settings.head_pitch_scale, path);
    WritePrivateProfileStringA("Tracking", "RollEnabled", s_settings.roll_enabled ? "1" : "0", path);
    WritePrivateProfileStringA("Stereo", "SameFrameStereo",
        s_settings.same_frame_stereo_requested ? "1" : "0", path);
    WritePrivateProfileStringA("Stereo", "ReverseEyes", s_settings.reverse_eyes ? "1" : "0", path);
    WritePrivateProfileStringA("Debug", "Logging", s_settings.debug_logging ? "1" : "0", path);
    sprintf_s(text, "%d", s_settings.downsample_threshold);
    WritePrivateProfileStringA("Rendering", "DownsampleThreshold", text, path);

    WritePrivateProfileStringA("HUD", "Enabled", s_settings.hud_enabled ? "1" : "0", path);
    WriteFloat("HUD", "Distance", s_settings.hud_distance, path);
    WriteFloat("HUD", "WidthDegrees", s_settings.hud_width_degrees, path);
    WriteFloat("HUD", "Scale", s_settings.hud_scale, path);
    WriteFloat("HUD", "Opacity", s_settings.hud_opacity, path);
    WriteFloat("HUD", "HorizontalOffset", s_settings.hud_horizontal_offset, path);
    WriteFloat("HUD", "VerticalOffset", s_settings.hud_vertical_offset, path);

    WritePrivateProfileStringA("Debug", "ForceNoHudLayer",
        s_settings.debug_force_no_hud_layer ? "1" : "0", path);

    /* Input / Locomotion */
    sprintf_s(text, "%d", s_settings.turn_mode);
    WritePrivateProfileStringA("Input", "TurnMode", text, path);
    WriteFloat("Input", "SnapTurnAngle", s_settings.snap_turn_angle, path);
    WriteFloat("Input", "SmoothTurnSpeed", s_settings.smooth_turn_speed, path);
    WriteFloat("Input", "LocomotionDeadzone", s_settings.locomotion_deadzone, path);
    WritePrivateProfileStringA("RoomScale", "Enabled",
        s_settings.room_scale_enabled ? "1" : "0", path);
    WritePrivateProfileStringA("RoomScale", "AllowHorizontal",
        s_settings.room_scale_allow_horizontal ? "1" : "0", path);
    WritePrivateProfileStringA("RoomScale", "AllowVertical",
        s_settings.room_scale_allow_vertical ? "1" : "0", path);
    WriteFloat("RoomScale", "BodyFollowStrength",
        s_settings.room_scale_follow_strength, path);
    WriteFloat("RoomScale", "CalibratedHeight",
        s_settings.room_scale_calibrated_height, path);
    WriteFloat("RoomScale", "HeadToChest", s_settings.room_scale_head_to_chest, path);
    WriteFloat("RoomScale", "HeadToPelvis", s_settings.room_scale_head_to_pelvis, path);
    WriteFloat("RoomScale", "ShoulderWidth", s_settings.room_scale_shoulder_width, path);
    WriteFloat("RoomScale", "StandingThreshold",
        s_settings.room_scale_standing_threshold, path);
    WriteFloat("RoomScale", "ProneThreshold",
        s_settings.room_scale_prone_threshold, path);
    WriteFloat("RoomScale", "PoseHysteresis",
        s_settings.room_scale_pose_hysteresis, path);
    WritePrivateProfileStringA("Debug", "RoomScale",
        s_settings.debug_room_scale ? "1" : "0", path);

    /* Weapon 6DoF */
    WriteFloat("Weapon", "PositionScale", s_settings.weapon_position_scale, path);
    WriteFloat("Weapon", "AimPitch", s_settings.aim_pitch_degrees, path);
    WriteFloat("Weapon", "AimYaw", s_settings.aim_yaw_degrees, path);
    WriteFloat("Dot", "ConvergenceDistance", s_settings.aim_convergence_m, path);
    WriteFloat("Dot", "DistanceMeters", s_settings.dot_distance_m, path);
    WriteFloat("Weapon", "OffsetForward", s_settings.weapon_offset_forward, path);
    WriteFloat("Weapon", "OffsetRight", s_settings.weapon_offset_right, path);
    WriteFloat("Weapon", "OffsetUp", s_settings.weapon_offset_up, path);
    WriteFloat("Weapon", "RotationPitch", s_settings.weapon_rotation_pitch, path);
    WriteFloat("Weapon", "RotationYaw", s_settings.weapon_rotation_yaw, path);
    WriteFloat("Weapon", "RotationRoll", s_settings.weapon_rotation_roll, path);
    WriteFloat("Hands", "LeftForward", s_settings.left_hand_offset_forward, path);
    WriteFloat("Hands", "LeftRight", s_settings.left_hand_offset_right, path);
    WriteFloat("Hands", "LeftUp", s_settings.left_hand_offset_up, path);
    WriteFloat("Hands", "LeftPitch", s_settings.left_hand_rotation_pitch, path);
    WriteFloat("Hands", "LeftYaw", s_settings.left_hand_rotation_yaw, path);
    WriteFloat("Hands", "LeftRoll", s_settings.left_hand_rotation_roll, path);
    WriteFloat("Hands", "RightForward", s_settings.right_hand_offset_forward, path);
    WriteFloat("Hands", "RightRight", s_settings.right_hand_offset_right, path);
    WriteFloat("Hands", "RightUp", s_settings.right_hand_offset_up, path);
    WriteFloat("Hands", "RightPitch", s_settings.right_hand_rotation_pitch, path);
    WriteFloat("Hands", "RightYaw", s_settings.right_hand_rotation_yaw, path);
    WriteFloat("Hands", "RightRoll", s_settings.right_hand_rotation_roll, path);
    WriteFloat("Dot", "HorizontalOffset", s_settings.dot_horizontal_offset, path);
    WriteFloat("Dot", "VerticalOffset", s_settings.dot_vertical_offset, path);
    WritePrivateProfileStringA("Visibility", "HidePlayerBodyAndArms",
        s_settings.hide_player_body_and_arms ? "1" : "0", path);
    WritePrivateProfileStringA("Visibility", "VanillaHandsFilter",
        s_settings.vanilla_hands_filter ? "1" : "0", path);
    WriteFloat("Visibility", "VanillaHandsCutThreshold",
        s_settings.vanilla_hands_cut_threshold, path);
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, path);

    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &attributes))
        s_lastWriteTime = attributes.ftLastWriteTime;
}

bool SaveLoaded() {
    if (!s_configPath[0]) return false;
    Save(s_configPath);
    return true;
}

static void FormatWeaponAimSection(uint64_t key, char section[40]) {
    sprintf_s(section, 40, "WeaponPose.%016llX",
              static_cast<unsigned long long>(key));
}

bool LoadWeaponAimProfile(uint64_t key, float& pitch, float& yaw, float& roll,
                          float& offsetForward, float& offsetRight,
                          float& offsetUp) {
    if (!s_configPath[0] || key == 0) return false;
    char section[40] = {};
    char pitchText[32] = {};
    char yawText[32] = {};
    char rollText[32] = {};
    char forwardText[32] = {};
    char rightText[32] = {};
    char upText[32] = {};
    FormatWeaponAimSection(key, section);
    if (!GetPrivateProfileStringA(section, "Pitch", "", pitchText,
                                  sizeof(pitchText), s_configPath) ||
        !GetPrivateProfileStringA(section, "Yaw", "", yawText,
                                  sizeof(yawText), s_configPath)) return false;
    const float loadedPitch = static_cast<float>(atof(pitchText));
    const float loadedYaw = static_cast<float>(atof(yawText));
    GetPrivateProfileStringA(section, "Roll", "0", rollText,
                             sizeof(rollText), s_configPath);
    const float loadedRoll = static_cast<float>(atof(rollText));
    GetPrivateProfileStringA(section, "Forward", "0", forwardText,
                             sizeof(forwardText), s_configPath);
    GetPrivateProfileStringA(section, "Right", "0", rightText,
                             sizeof(rightText), s_configPath);
    GetPrivateProfileStringA(section, "Up", "0", upText,
                             sizeof(upText), s_configPath);
    const float loadedForward = static_cast<float>(atof(forwardText));
    const float loadedRight = static_cast<float>(atof(rightText));
    const float loadedUp = static_cast<float>(atof(upText));
    if (!std::isfinite(loadedPitch) || !std::isfinite(loadedYaw) ||
        !std::isfinite(loadedRoll) || !std::isfinite(loadedForward) ||
        !std::isfinite(loadedRight) || !std::isfinite(loadedUp)) return false;
    pitch = std::clamp(loadedPitch, -45.0f, 45.0f);
    yaw = std::clamp(loadedYaw, -45.0f, 45.0f);
    roll = std::clamp(loadedRoll, -180.0f, 180.0f);
    offsetForward = std::clamp(loadedForward, -100.0f, 100.0f);
    offsetRight = std::clamp(loadedRight, -100.0f, 100.0f);
    offsetUp = std::clamp(loadedUp, -100.0f, 100.0f);
    return true;
}

bool SaveWeaponAimProfile(uint64_t key, float pitch, float yaw, float roll,
                           float offsetForward, float offsetRight, float offsetUp,
                           const char* profileName) {
    if (!s_configPath[0] || key == 0) return false;
    char section[40] = {};
    FormatWeaponAimSection(key, section);
    WriteFloat(section, "Pitch", std::clamp(pitch, -45.0f, 45.0f), s_configPath);
    WriteFloat(section, "Yaw", std::clamp(yaw, -45.0f, 45.0f), s_configPath);
    WriteFloat(section, "Roll", std::clamp(roll, -180.0f, 180.0f), s_configPath);
    WriteFloat(section, "Forward", std::clamp(offsetForward, -100.0f, 100.0f),
               s_configPath);
    WriteFloat(section, "Right", std::clamp(offsetRight, -100.0f, 100.0f),
               s_configPath);
    WriteFloat(section, "Up", std::clamp(offsetUp, -100.0f, 100.0f), s_configPath);
    if (profileName && *profileName)
        WritePrivateProfileStringA(section, "Name", profileName, s_configPath);
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, s_configPath);
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExA(s_configPath, GetFileExInfoStandard, &attributes))
        s_lastWriteTime = attributes.ftLastWriteTime;
    return true;
}

static void FormatAbsoluteWeaponMountSection(uint64_t key, char section[48]) {
    sprintf_s(section, 48, "WeaponAbsolute.%016llX",
              static_cast<unsigned long long>(key));
}

bool LoadAbsoluteWeaponMount(uint64_t key, float matrix[16]) {
    if (!s_configPath[0] || key == 0 || !matrix) return false;
    char section[48] = {};
    FormatAbsoluteWeaponMountSection(key, section);
    if (GetPrivateProfileIntA(section, "Version", 0, s_configPath) != 1 ||
        GetPrivateProfileIntA(section, "Valid", 0, s_configPath) != 1) return false;
    for (int index = 0; index < 16; ++index) {
        char name[8] = {};
        char value[32] = {};
        sprintf_s(name, "M%02d", index);
        if (!GetPrivateProfileStringA(section, name, "", value,
                                      sizeof(value), s_configPath)) return false;
        matrix[index] = static_cast<float>(atof(value));
        if (!std::isfinite(matrix[index])) return false;
    }
    const float scaleX = sqrtf(matrix[0] * matrix[0] + matrix[1] * matrix[1] +
        matrix[2] * matrix[2]);
    const float scaleY = sqrtf(matrix[4] * matrix[4] + matrix[5] * matrix[5] +
        matrix[6] * matrix[6]);
    const float scaleZ = sqrtf(matrix[8] * matrix[8] + matrix[9] * matrix[9] +
        matrix[10] * matrix[10]);
    return scaleX > 0.01f && scaleX < 100.0f && scaleY > 0.01f &&
        scaleY < 100.0f && scaleZ > 0.01f && scaleZ < 100.0f &&
        fabsf(matrix[3]) < 0.001f && fabsf(matrix[7]) < 0.001f &&
        fabsf(matrix[11]) < 0.001f && fabsf(matrix[15] - 1.0f) < 0.001f;
}

bool SaveAbsoluteWeaponMount(uint64_t key, const float matrix[16],
                             const char* profileName) {
    if (!s_configPath[0] || key == 0 || !matrix) return false;
    char section[48] = {};
    FormatAbsoluteWeaponMountSection(key, section);
    WritePrivateProfileStringA(section, "Version", "1", s_configPath);
    WritePrivateProfileStringA(section, "Valid", "1", s_configPath);
    for (int index = 0; index < 16; ++index) {
        char name[8] = {};
        sprintf_s(name, "M%02d", index);
        WriteFloat(section, name, matrix[index], s_configPath);
    }
    if (profileName && *profileName)
        WritePrivateProfileStringA(section, "Name", profileName, s_configPath);
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, s_configPath);
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExA(s_configPath, GetFileExInfoStandard, &attributes))
        s_lastWriteTime = attributes.ftLastWriteTime;
    return true;
}

static void FormatWeaponMountSection(uint64_t key, char section[40]) {
    sprintf_s(section, 40, "WeaponMount.%016llX",
              static_cast<unsigned long long>(key));
}

bool LoadWeaponMountProfile(uint64_t key, float matrix[16], float gripLocal[3],
                            int& barrelAxis, float& barrelSign) {
    if (!s_configPath[0] || key == 0 || !matrix || !gripLocal) return false;
    char section[40] = {};
    FormatWeaponMountSection(key, section);
    if (GetPrivateProfileIntA(section, "Version", 0, s_configPath) != 2 ||
        GetPrivateProfileIntA(section, "Valid", 0, s_configPath) != 1) return false;
    for (int index = 0; index < 16; ++index) {
        char name[8] = {};
        char value[32] = {};
        sprintf_s(name, "M%02d", index);
        if (!GetPrivateProfileStringA(section, name, "", value,
                                      sizeof(value), s_configPath)) return false;
        matrix[index] = static_cast<float>(atof(value));
        if (!std::isfinite(matrix[index])) return false;
    }
    for (int index = 0; index < 3; ++index) {
        char name[8] = {};
        char value[32] = {};
        sprintf_s(name, "Grip%d", index);
        if (!GetPrivateProfileStringA(section, name, "", value,
                                      sizeof(value), s_configPath)) return false;
        gripLocal[index] = static_cast<float>(atof(value));
        if (!std::isfinite(gripLocal[index])) return false;
    }
    barrelAxis = GetPrivateProfileIntA(section, "BarrelAxis", -1, s_configPath);
    char signText[32] = {};
    GetPrivateProfileStringA(section, "BarrelSign", "0", signText,
                             sizeof(signText), s_configPath);
    barrelSign = static_cast<float>(atof(signText));
    return barrelAxis >= 0 && barrelAxis <= 2 &&
        std::isfinite(barrelSign) && fabsf(barrelSign) >= 0.5f;
}

bool SaveWeaponMountProfile(uint64_t key, const float matrix[16],
                            const float gripLocal[3], int barrelAxis,
                            float barrelSign, const char* profileName) {
    if (!s_configPath[0] || key == 0 || !matrix || !gripLocal ||
        barrelAxis < 0 || barrelAxis > 2) return false;
    char section[40] = {};
    FormatWeaponMountSection(key, section);
    WritePrivateProfileStringA(section, "Version", "2", s_configPath);
    WritePrivateProfileStringA(section, "Valid", "1", s_configPath);
    for (int index = 0; index < 16; ++index) {
        char name[8] = {};
        sprintf_s(name, "M%02d", index);
        WriteFloat(section, name, matrix[index], s_configPath);
    }
    for (int index = 0; index < 3; ++index) {
        char name[8] = {};
        sprintf_s(name, "Grip%d", index);
        WriteFloat(section, name, gripLocal[index], s_configPath);
    }
    char axisText[8] = {};
    sprintf_s(axisText, "%d", barrelAxis);
    WritePrivateProfileStringA(section, "BarrelAxis", axisText, s_configPath);
    WriteFloat(section, "BarrelSign", barrelSign < 0.0f ? -1.0f : 1.0f,
               s_configPath);
    if (profileName && *profileName)
        WritePrivateProfileStringA(section, "Name", profileName, s_configPath);
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, s_configPath);
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExA(s_configPath, GetFileExInfoStandard, &attributes))
        s_lastWriteTime = attributes.ftLastWriteTime;
    return true;
}

}} // namespace bl1gotyvr::config
