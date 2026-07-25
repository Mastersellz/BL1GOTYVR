#pragma once

namespace bl1gotyvr { namespace config {

struct Settings {
    int render_width = 1920;
    int render_height = 1080;
    float resolution_scale = 1.0f;
    float fov_degrees = 100.0f;
    float ipd_mm = 64.0f;
    float convergence_m = 10.0f;
    float near_plane = 0.1f;
    float far_plane = 10000.0f;
    float positional_scale = 1.0f;
    float rotation_scale = 1.0f;
    bool roll_enabled = true;
    bool same_frame_stereo_requested = true;
    bool same_frame_stereo = true;
    bool reverse_eyes = false;
    bool motion_blur_disabled = true;
    bool camera_recoil_disabled = true;
    int downsample_threshold = 3840;
    bool debug_logging = true;

    /* Input / Locomotion */
    int turn_mode = 1;              /* 0=snap, 1=smooth, 2=off */
    float snap_turn_angle = 30.0f;
    float smooth_turn_speed = 90.0f;
    float locomotion_deadzone = 0.2f;

    /* Weapon 6DoF */
    float weapon_position_scale = 0.5f;
};

Settings& Get();
void Load(const char* path);
void Save(const char* path);
bool ReloadIfChanged();

}} // namespace bl1gotyvr::config
