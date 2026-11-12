#include "../src/config/Config.hpp"
#include "../configurator/Settings.hpp"
#include "../src/player/TwoBoneIK.hpp"
#include "../src/player/RoomScaleBody.hpp"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace bl1gotyvr { void Log(const char*, ...) {} }
namespace cfg = bl1gotyvr::config;
namespace ui = bl1gotyvr::configurator;

static void Check(bool passed, const char* message) {
    if (!passed) throw std::runtime_error(message);
}

// Each test owns a unique file in CTest's build directory, never a game INI.
struct TestIni {
    char path[MAX_PATH] = {};
    TestIni() {
        char directory[MAX_PATH] = {};
        const DWORD length = GetCurrentDirectoryA(MAX_PATH, directory);
        Check(length && length < MAX_PATH, "test directory unavailable");
        Check(GetTempFileNameA(directory, "tvr", 0, path) != 0, "test file creation");
    }
    ~TestIni() {
        if (path[0]) {
            SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(path);
        }
    }
};

static std::string ReadBytes(const char* path) {
    std::ifstream input(path, std::ios::binary);
    Check(input.good(), "read test INI");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static void WriteValue(const char* path, const char* section, const char* key,
                       const char* value) {
    Check(WritePrivateProfileStringA(section, key, value, path) != FALSE, "write test INI");
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, path);
}

static ui::SettingsValues Defaults() {
    ui::SettingsValues result;
    for (size_t i = 0; i < ui::kFields.size(); ++i)
        result.fields[i] = ui::kFields[i].defaultValue;
    return result;
}

static void SetField(ui::SettingsValues& values, int id, const char* text) {
    for (size_t i = 0; i < ui::kFields.size(); ++i) {
        if (ui::kFields[i].id == id) {
            values.fields[i] = text;
            return;
        }
    }
    throw std::runtime_error("unknown test field");
}

static void Save(const char* path, const ui::SettingsValues& values) {
    std::string error;
    if (!ui::SaveModSettings(path, values, error)) throw std::runtime_error(error);
}

static void StereoReload() {
    TestIni ini;
    auto values = Defaults();
    values.sameFrameStereo = true;
    Save(ini.path, values);
    cfg::Load(ini.path);
    Check(!cfg::IsSameFrameStereoEnabled(), "INI must not bypass native-hook validation");
    cfg::InitializeStereoMode(true);
    Check(cfg::IsSameFrameStereoEnabled(), "validated native stereo was not selected");

    WIN32_FILE_ATTRIBUTE_DATA previous = {};
    Check(GetFileAttributesExA(ini.path, GetFileExInfoStandard, &previous) != FALSE,
          "read initial write time");
    SetField(values, 120, "0.75");
    Save(ini.path, values);
    // Force a distinct timestamp without depending on filesystem clock granularity.
    ULARGE_INTEGER ticks = {};
    ticks.LowPart = previous.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = previous.ftLastWriteTime.dwHighDateTime;
    ticks.QuadPart += 10000000;
    FILETIME changed = {ticks.LowPart, ticks.HighPart};
    HANDLE file = CreateFileA(ini.path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "open test write time");
    const BOOL stamped = SetFileTime(file, nullptr, nullptr, &changed);
    CloseHandle(file);
    Check(stamped != FALSE, "set test write time");
    Check(cfg::ReloadIfChanged(), "external configuration change was not reloaded");
    Check(cfg::Get().postfx_sharpen == 0.75f, "live setting was not applied");
    Check(cfg::IsSameFrameStereoEnabled(), "live INI reload disabled native stereo");

    values.sameFrameStereo = false;
    Save(ini.path, values);
    cfg::Load(ini.path);
    Check(!cfg::Get().same_frame_stereo_requested, "new request was not loaded");
    Check(cfg::IsSameFrameStereoEnabled(), "mode changed without restart");
    cfg::InitializeStereoMode(true); // Simulate the next startup.
    Check(!cfg::IsSameFrameStereoEnabled(), "next startup ignored disabled request");
    values.sameFrameStereo = true;
    Save(ini.path, values);
    cfg::Load(ini.path);
    Check(!cfg::IsSameFrameStereoEnabled(), "reload enabled an uninitialized mode");
    cfg::InitializeStereoMode(false);
    Check(!cfg::IsSameFrameStereoEnabled(), "invalid native hooks enabled stereo");
    cfg::Save(ini.path);
    Check(GetPrivateProfileIntA("Stereo", "SameFrameStereo", 0, ini.path) == 1,
          "save must persist the request, not the validated fallback");
}

static void FieldValidation() {
    Check(ui::kFields.size() == 19, "unexpected or empty configurator field");
    for (const auto& field : ui::kFields) {
        int parsed = 0;
        Check(ui::ParseField(field, field.defaultValue, parsed), "invalid field default");
        for (const char* invalid : {"", " ", "nan", "inf", "abc", "1tail"})
            Check(!ui::ParseField(field, invalid, parsed), "invalid number accepted");
        Check(!ui::ParseField(field, nullptr, parsed), "null number accepted");
    }
    int parsed = 0;
    Check(!ui::ParseField(ui::kFields[0], "2048.5", parsed), "fractional resolution accepted");
    TestIni ini;
    auto values = Defaults();
    SetField(values, 101, "1920");
    SetField(values, 102, "1920");
    std::string error;
    Check(!ui::SaveModSettings(ini.path, values, error), "unsupported square preset accepted");
}

static void AtomicSave() {
    TestIni ini;
    WriteValue(ini.path, "WeaponPose.test", "Pitch", "12.5");
    WriteValue(ini.path, "Input", "TurnMode", "0");
    WriteValue(ini.path, "Input", "SnapTurnAngle", "45");
    WriteValue(ini.path, "Input", "SmoothTurnSpeed", "120");
    auto values = Defaults();
    values.sameFrameStereo = true;
    values.hmdDirectedMovement = true;
    Save(ini.path, values);
    Check(GetPrivateProfileIntA("PostFx", "Enabled", 1, ini.path) == 0,
          "unchecked PostFX was enabled by default sharpening");
    Check(GetPrivateProfileIntA("WeaponPose.test", "Pitch", 0, ini.path) == 12,
          "unknown weapon profile was lost");
    Check(GetPrivateProfileIntA("Input", "HmdDirectedLocomotion", 0, ini.path) == 1,
          "checkbox was not saved");
    const std::string original = ReadBytes(ini.path);
    SetField(values, 101, "4096");
    SetField(values, 102, "4096");
    SetField(values, 124, "invalid"); // Last field: no earlier writes may leak.
    std::string error;
    Check(!ui::SaveModSettings(ini.path, values, error), "invalid final field accepted");
    Check(!error.empty() && original == ReadBytes(ini.path), "failed validation changed the INI");
    values = Defaults();
    values.postFx = true;
    SetField(values, 101, "2.048e3");
    SetField(values, 102, "2048.0");
    Save(ini.path, values);
    Check(GetPrivateProfileIntA("Display", "Width", 0, ini.path) == 2048 &&
          GetPrivateProfileIntA("Display", "Height", 0, ini.path) == 2048,
          "integer fields were not normalized for Win32 INI parsing");
    Check(GetPrivateProfileIntA("PostFx", "Enabled", 0, ini.path) == 1,
          "checked PostFX was not saved");
    cfg::Load(ini.path);
    Check(cfg::Get().postfx_enabled && cfg::Get().render_width == 2048,
          "DLL could not read configurator settings");
    Check(cfg::Get().turn_mode == 0 && cfg::Get().snap_turn_angle == 45 &&
          cfg::Get().smooth_turn_speed == 120,
          "standalone configurator replaced saved turning settings");
}

static void WriteFailure() {
    TestIni ini;
    auto values = Defaults();
    Save(ini.path, values);
    const auto original = ReadBytes(ini.path);
    SetField(values, 104, "110");
    Check(SetFileAttributesA(ini.path, FILE_ATTRIBUTE_READONLY) != FALSE, "set readonly");
    std::string error;
    Check(!ui::SaveModSettings(ini.path, values, error), "readonly save reported success");
    Check(original == ReadBytes(ini.path), "readonly INI changed");
    Check(SetFileAttributesA(ini.path, FILE_ATTRIBUTE_NORMAL) != FALSE, "clear readonly");
    HANDLE lock = CreateFileA(ini.path, GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "lock INI");
    const bool saved = ui::SaveModSettings(ini.path, values, error);
    CloseHandle(lock);
    Check(!saved && !error.empty(), "locked replacement reported success");
    Check(original == ReadBytes(ini.path), "failed replacement changed original INI");
    Check(!ui::SaveModSettings(nullptr, values, error), "null path accepted");
}

static void ConfigClamps() {
    TestIni ini;
    WriteValue(ini.path, "Display", "Width", "1920");
    WriteValue(ini.path, "Display", "Height", "1080");
    WriteValue(ini.path, "Tracking", "PositionScale", "nan");
    WriteValue(ini.path, "PostFx", "Gamma", "inf");
    WriteValue(ini.path, "Melee", "RangeMultiplier", "99");
    cfg::Load(ini.path);
    Check(cfg::Get().render_width == 2048 && cfg::Get().render_height == 2048,
          "unsupported resolution did not fall back");
    Check(cfg::Get().positional_scale == 1.0f && cfg::Get().postfx_gamma == 1.0f,
          "nonfinite settings escaped validation");
    Check(cfg::GetMeleeRangeMultiplier() == 5.0f, "melee range was not clamped");
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "specify a test case");
        const std::string test = argv[1];
        if (test == "stereo_reload") StereoReload();
        else if (test == "field_validation") FieldValidation();
        else if (test == "atomic_save") AtomicSave();
        else if (test == "write_failure") WriteFailure();
        else if (test == "config_clamps") ConfigClamps();
        else if (test == "math") {
            Check(bl1gotyvr::player::RunTwoBoneIKSelfTest(), "IK self-test");
            Check(bl1gotyvr::player::RunRoomScaleBodySelfTest(), "room-scale self-test");
        } else throw std::runtime_error("unknown test case");
        std::printf("PASS: %s\n", argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
