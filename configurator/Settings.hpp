#pragma once

#include <array>
#include <string>

namespace bl1gotyvr::configurator {

struct Field {
    int id;
    const char* label;
    const char* section;
    const char* key;
    const char* defaultValue;
    float minimum;
    float maximum;
    bool integer;
};

// Infer the count so a missing initializer cannot create an ID-zero/null field.
inline constexpr auto kFields = std::to_array<Field>({
    {101, "Render width", "Display", "Width", "2048", 640.0f, 7680.0f, true},
    {102, "Render height", "Display", "Height", "2048", 480.0f, 4320.0f, true},
    {103, "Resolution scale", "Display", "ResolutionScale", "1.00", 0.5f, 2.0f, false},
    {104, "Camera FOV (deg)", "Display", "FOV", "100.0", 60.0f, 150.0f, false},
    {105, "IPD (mm)", "Stereo", "IPD", "64.0", 50.0f, 80.0f, false},
    {106, "Convergence shift (% per eye)", "Stereo", "Convergence", "10.0", 0.0f, 20.0f, false},
    {107, "Near plane", "Rendering", "NearPlane", "0.10", 0.01f, 10.0f, false},
    {108, "Far plane", "Rendering", "FarPlane", "10000", 100.0f, 100000.0f, false},
    {109, "Position scale", "Tracking", "PositionScale", "1.00", 0.0f, 5.0f, false},
    {110, "Rotation scale", "Tracking", "RotationScale", "1.00", 0.0f, 5.0f, false},
    {112, "OpenXR refresh rate (Hz, 0=default)", "OpenXR", "RefreshRateHz", "72.0", 0.0f, 240.0f, false},
    {113, "Arm reach scale", "Hands", "ArmReachScale", "1.60", 1.0f, 2.0f, false},
    {114, "Hand cut position", "Visibility", "VanillaHandsCutThreshold", "70.0", 20.0f, 90.0f, false},
    {115, "Melee range scale", "Melee", "RangeMultiplier", "1.60", 1.0f, 5.0f, false},
    {120, "Post FX sharpen", "PostFx", "Sharpen", "0.35", 0.0f, 1.0f, false},
    {121, "Post FX contrast", "PostFx", "Contrast", "1.00", 0.5f, 2.0f, false},
    {122, "Post FX saturation", "PostFx", "Saturation", "1.00", 0.0f, 2.0f, false},
    {123, "Post FX brightness", "PostFx", "Brightness", "0.00", -0.5f, 0.5f, false},
    {124, "Post FX gamma", "PostFx", "Gamma", "1.00", 0.5f, 2.0f, false},
});

static_assert([] {
    for (size_t i = 0; i < kFields.size(); ++i) {
        const auto& field = kFields[i];
        if (field.id <= 0 || !field.label || !field.section || !field.key ||
            !field.defaultValue || field.minimum > field.maximum) return false;
        for (size_t j = 0; j < i; ++j)
            if (field.id == kFields[j].id) return false;
    }
    return true;
}(), "Configurator fields must have unique nonzero IDs and valid metadata");

struct SettingsValues {
    std::array<std::string, kFields.size()> fields;
    bool sameFrameStereo = false;
    bool reverseEyes = false;
    bool logging = true;
    bool dot = true;
    bool hmdDirectedMovement = false;
    bool physicalCrouch = true;
    bool postFx = false;
    bool fsr = true;
};

bool ParseField(const Field& field, const char* text, int& integerValue);
// Validates everything before writing, preserves unknown keys/profiles, then
// atomically replaces the mod INI. Never touches the game's own INI files.
bool SaveModSettings(const char* path, const SettingsValues& values, std::string& error);

} // namespace bl1gotyvr::configurator
