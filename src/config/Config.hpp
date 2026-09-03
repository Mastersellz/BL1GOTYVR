#pragma once

#include <cstdint>

namespace bl1gotyvr { namespace config {

struct Settings {
    int render_width = 2048;
    int render_height = 2048;
    float resolution_scale = 1.0f;
    float fov_degrees = 100.0f;
    float ipd_mm = 64.0f;
    float convergence_m = 0.0f;
    float openxr_refresh_rate_hz = 72.0f; /* 0 keeps the runtime default */
    float near_plane = 0.1f;
    float far_plane = 10000.0f;
    float positional_scale = 1.0f;
    float rotation_scale = 1.0f;
    float head_yaw_scale = 1.0f;     /* 0=free look, 1=full camera rotation */
    float head_pitch_scale = 1.0f;   /* 0=free look, 1=full camera rotation */
    bool roll_enabled = true;
    bool same_frame_stereo_requested = false;
    bool same_frame_stereo = false;
    bool reverse_eyes = false;
    bool motion_blur_disabled = true;
    bool camera_recoil_disabled = true;
    int downsample_threshold = 3840;
    bool debug_logging = true;

    /* VR HUD */
    bool hud_enabled = true;
    float hud_distance = 2.0f;
    float hud_width_degrees = 80.0f;
    float hud_scale = 1.0f;
    float hud_opacity = 1.0f;
float hud_horizontal_offset = 0.0f;
    float hud_vertical_offset = -0.35f;

    /* Input / Locomotion */
    int turn_mode = 1;              /* 0=snap, 1=smooth, 2=off */
    float snap_turn_angle = 30.0f;
    float smooth_turn_speed = 90.0f;
    float locomotion_deadzone = 0.2f;
    bool hmd_directed_locomotion = false;
    bool physical_crouch_enabled = true;

    /* Room-scale body tracking */
    bool room_scale_enabled = true;
    bool room_scale_allow_horizontal = true;
    bool room_scale_allow_vertical = true;
    float room_scale_follow_strength = 10.0f;
    float room_scale_calibrated_height = 170.0f;
    float room_scale_head_to_chest = 45.0f;
    float room_scale_head_to_pelvis = 85.0f;
    float room_scale_shoulder_width = 42.0f;
    float room_scale_standing_threshold = 0.72f;
    float room_scale_prone_threshold = 0.35f;
    float room_scale_pose_hysteresis = 0.04f;
    bool debug_room_scale = false;

    /* Weapon 6DoF */
    float weapon_position_scale = 0.5f;
    float aim_pitch_degrees = 0.0f;
    float aim_yaw_degrees = 0.0f;
    float aim_convergence_m = 20.0f;
    bool dot_enabled = true;
    float dot_distance_m = 5.0f;
    float weapon_offset_forward = 0.0f;
    float weapon_offset_right = 0.0f;
    float weapon_offset_up = 0.0f;
    float weapon_rotation_pitch = 0.0f;
    float weapon_rotation_yaw = 0.0f;
    float weapon_rotation_roll = 0.0f;

    /* Hand IK offsets in camera-local Unreal units */
    float left_hand_offset_forward = 0.0f;
    float left_hand_offset_right = 0.0f;
    float left_hand_offset_up = 0.0f;
    float left_hand_rotation_pitch = 0.0f;
    float left_hand_rotation_yaw = 0.0f;
    float left_hand_rotation_roll = 0.0f;
    float right_hand_offset_forward = 0.0f;
    float right_hand_offset_right = 0.0f;
    float right_hand_offset_up = 0.0f;
    float right_hand_rotation_pitch = 0.0f;
    float right_hand_rotation_yaw = 0.0f;
    float right_hand_rotation_roll = 0.0f;
    float arm_reach_scale = 1.35f;

    /* Legacy visual-only aim dot offsets in normalized eye UV */
    float dot_horizontal_offset = 0.0f;
    float dot_vertical_offset = 0.0f;

    /* Player visibility */
    bool hide_player_body_and_arms = false;
    bool vanilla_hands_filter = true;
    float vanilla_hands_cut_threshold = 74.0f;

    /* Debug */
    bool debug_force_no_hud_layer = false;  /* Skip HUD layer submission for VDXR testing */
};

Settings& Get();
void Load(const char* path);
void Save(const char* path);
bool SaveLoaded();
bool ReloadIfChanged();
bool LoadWeaponAimProfile(uint64_t key, float& pitch, float& yaw, float& roll,
                          float& offsetForward, float& offsetRight,
                          float& offsetUp);
bool SaveWeaponAimProfile(uint64_t key, float pitch, float yaw, float roll,
                          float offsetForward, float offsetRight, float offsetUp,
                           const char* profileName);
bool LoadAbsoluteWeaponMount(uint64_t key, float matrix[16]);
bool SaveAbsoluteWeaponMount(uint64_t key, const float matrix[16],
                             const char* profileName);
bool LoadWeaponMountProfile(uint64_t key, float matrix[16], float gripLocal[3],
                            int& barrelAxis, float& barrelSign);
bool SaveWeaponMountProfile(uint64_t key, const float matrix[16],
                            const float gripLocal[3], int barrelAxis,
                            float barrelSign, const char* profileName);

}} // namespace bl1gotyvr::config
