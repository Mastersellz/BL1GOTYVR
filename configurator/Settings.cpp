#include "Settings.hpp"

#include <Windows.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace bl1gotyvr::configurator {

bool ParseField(const Field& field, const char* text, int& integerValue) {
    if (!text || !*text) return false;
    char* end = nullptr;
    const float parsed = strtof(text, &end);
    if (!end || end == text || *end != '\0' || !std::isfinite(parsed) ||
        parsed < field.minimum || parsed > field.maximum) return false;
    if (field.integer && parsed != std::trunc(parsed)) return false;
    integerValue = static_cast<int>(parsed);
    return true;
}

bool SaveModSettings(const char* path, const SettingsValues& values, std::string& error) {
    error.clear();
    int width = 0, height = 0;
    for (size_t i = 0; i < kFields.size(); ++i) {
        int parsed = 0;
        if (!ParseField(kFields[i], values.fields[i].c_str(), parsed)) {
            error = std::string("Invalid value for ") + kFields[i].label +
                ".\nAllowed range: " + std::to_string(kFields[i].minimum) +
                " - " + std::to_string(kFields[i].maximum) + ".";
            return false;
        }
        if (kFields[i].id == 101) width = parsed;
        if (kFields[i].id == 102) height = parsed;
    }
    bool supported = false;
    for (int size : {1536, 2048, 2560, 3072, 4096})
        if (width == size && height == size) supported = true;
    if (!supported) {
        error = "Unsupported resolution. Choose Low, Medium, High, Ultra, or Mega.";
        return false;
    }

    char fullPath[MAX_PATH] = {};
    char* filename = nullptr;
    const DWORD length = path && *path
        ? GetFullPathNameA(path, MAX_PATH, fullPath, &filename) : 0;
    if (!length || length >= MAX_PATH || !filename) {
        error = "Could not resolve the settings path.";
        return false;
    }
    char directory[MAX_PATH] = {};
    memcpy(directory, fullPath, static_cast<size_t>(filename - fullPath));
    struct TemporaryFile {
        char path[MAX_PATH] = {};
        ~TemporaryFile() { if (path[0]) DeleteFileA(path); }
    } temporary;
    if (!GetTempFileNameA(directory, "bvr", 0, temporary.path)) {
        error = "Could not create a temporary settings file. Check directory permissions.";
        return false;
    }
    const DWORD attributes = GetFileAttributesA(fullPath);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_READONLY) ||
            !CopyFileA(fullPath, temporary.path, FALSE)) {
            error = "Could not copy the existing settings. Check file permissions.";
            return false;
        }
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        error = "Could not read the existing settings file.";
        return false;
    }

    for (size_t i = 0; i < kFields.size(); ++i) {
        int parsed = 0;
        ParseField(kFields[i], values.fields[i].c_str(), parsed);
        const std::string text = kFields[i].integer
            ? std::to_string(parsed) : values.fields[i];
        if (!WritePrivateProfileStringA(kFields[i].section, kFields[i].key,
                text.c_str(), temporary.path)) {
            error = "Could not write settings. The original INI was not replaced.";
            return false;
        }
    }
    const auto writeBool = [&](const char* section, const char* key, bool value) {
        return WritePrivateProfileStringA(section, key, value ? "1" : "0",
                                          temporary.path) != FALSE;
    };
    if (!writeBool("Stereo", "SameFrameStereo", values.sameFrameStereo) ||
        !writeBool("Stereo", "ReverseEyes", values.reverseEyes) ||
        !writeBool("Debug", "Logging", values.logging) ||
        !writeBool("Dot", "Enabled", values.dot) ||
        !writeBool("Input", "HmdDirectedLocomotion", values.hmdDirectedMovement) ||
        !writeBool("Input", "PhysicalCrouch", values.physicalCrouch) ||
        !writeBool("PostFx", "Enabled", values.postFx) ||
        !writeBool("PostFx", "FsrUpscale", values.fsr)) {
        error = "Could not write settings. The original INI was not replaced.";
        return false;
    }
    // The all-null profile call invalidates the INI cache; its return value is
    // not a reliable write-success signal. Check the actual file flush instead.
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, temporary.path);
    HANDLE file = CreateFileA(temporary.path, GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const BOOL flushed = file != INVALID_HANDLE_VALUE && FlushFileBuffers(file);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!flushed) {
        error = "Could not flush settings. The original INI was not replaced.";
        return false;
    }
    if (!MoveFileExA(temporary.path, fullPath,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not replace the settings file. Check permissions or file locks.";
        return false;
    }
    temporary.path[0] = '\0';
    return true;
}

} // namespace bl1gotyvr::configurator
