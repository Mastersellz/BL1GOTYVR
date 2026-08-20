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

    /* Weapon 6DoF */
    s_settings.weapon_position_scale = std::clamp(
        ReadFloat("Weapon", "PositionScale", s_settings.weapon_position_scale, path), 0.0f, 1.0f);
    s_settings.aim_pitch_degrees = std::clamp(
        ReadFloat("Weapon", "AimPitch", s_settings.aim_pitch_degrees, path), -45.0f, 45.0f);
    s_settings.aim_yaw_degrees = std::clamp(
        ReadFloat("Weapon", "AimYaw", s_settings.aim_yaw_degrees, path), -45.0f, 45.0f);

    s_settings.hide_player_body_and_arms = GetPrivateProfileIntA(
        "Visibility", "HidePlayerBodyAndArms", 0, path) != 0;
    s_settings.vanilla_hands_filter = GetPrivateProfileIntA(
        "Visibility", "VanillaHandsFilter", 0, path) != 0;

    Log("[Config] Loaded %s: %dx%d scale=%.2f FOV=%.1f IPD=%.1fmm "
        "convergenceShift=%.2f%% aim=(%.2f,%.2f)",
        path, s_settings.render_width, s_settings.render_height, s_settings.resolution_scale,
        s_settings.fov_degrees, s_settings.ipd_mm, s_settings.convergence_m,
        s_settings.aim_pitch_degrees, s_settings.aim_yaw_degrees);
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

    /* Weapon 6DoF */
    WriteFloat("Weapon", "PositionScale", s_settings.weapon_position_scale, path);
    WriteFloat("Weapon", "AimPitch", s_settings.aim_pitch_degrees, path);
    WriteFloat("Weapon", "AimYaw", s_settings.aim_yaw_degrees, path);
    WritePrivateProfileStringA("Visibility", "HidePlayerBodyAndArms",
        s_settings.hide_player_body_and_arms ? "1" : "0", path);
    WritePrivateProfileStringA("Visibility", "VanillaHandsFilter",
        s_settings.vanilla_hands_filter ? "1" : "0", path);
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
    sprintf_s(section, 40, "WeaponAim.%016llX",
              static_cast<unsigned long long>(key));
}

bool LoadWeaponAimProfile(uint64_t key, float& pitch, float& yaw) {
    if (!s_configPath[0] || key == 0) return false;
    char section[40] = {};
    char pitchText[32] = {};
    char yawText[32] = {};
    FormatWeaponAimSection(key, section);
    if (!GetPrivateProfileStringA(section, "Pitch", "", pitchText,
                                  sizeof(pitchText), s_configPath) ||
        !GetPrivateProfileStringA(section, "Yaw", "", yawText,
                                  sizeof(yawText), s_configPath)) return false;
    const float loadedPitch = static_cast<float>(atof(pitchText));
    const float loadedYaw = static_cast<float>(atof(yawText));
    if (!std::isfinite(loadedPitch) || !std::isfinite(loadedYaw)) return false;
    pitch = std::clamp(loadedPitch, -45.0f, 45.0f);
    yaw = std::clamp(loadedYaw, -45.0f, 45.0f);
    return true;
}

bool SaveWeaponAimProfile(uint64_t key, float pitch, float yaw,
                          const char* profileName) {
    if (!s_configPath[0] || key == 0) return false;
    char section[40] = {};
    FormatWeaponAimSection(key, section);
    WriteFloat(section, "Pitch", std::clamp(pitch, -45.0f, 45.0f), s_configPath);
    WriteFloat(section, "Yaw", std::clamp(yaw, -45.0f, 45.0f), s_configPath);
    if (profileName && *profileName)
        WritePrivateProfileStringA(section, "Name", profileName, s_configPath);
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, s_configPath);
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExA(s_configPath, GetFileExInfoStandard, &attributes))
        s_lastWriteTime = attributes.ftLastWriteTime;
    return true;
}

}} // namespace bl1gotyvr::config
