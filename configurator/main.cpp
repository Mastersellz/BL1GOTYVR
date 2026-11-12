#include <Windows.h>
#include <ShlObj.h>
#include "Settings.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int kSaveButton = 200;
constexpr int kDefaultsButton = 201;
constexpr int kSameFrameCheck = 202;
constexpr int kReverseEyesCheck = 203;
constexpr int kLoggingCheck = 205;
constexpr int kDotCheck = 206;
constexpr int kHmdDirectionCheck = 207;
constexpr int kPhysicalCrouchCheck = 208;
constexpr int kPostFxCheck = 220;
constexpr int kFsrCheck = 221;
constexpr int kLowPreset = 210;
constexpr int kMediumPreset = 211;
constexpr int kHighPreset = 212;
constexpr int kUltraPreset = 213;
constexpr int kMegaUltraPreset = 214;

using bl1gotyvr::configurator::kFields;

struct RenderPreset {
    int id;
    const char* width;
    const char* height;
};

constexpr std::array<RenderPreset, 5> kRenderPresets = {{
    {kLowPreset, "1536", "1536"},
    {kMediumPreset, "2048", "2048"},
    {kHighPreset, "2560", "2560"},
    {kUltraPreset, "3072", "3072"},
    {kMegaUltraPreset, "4096", "4096"},
}};

std::string ExeDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string result(path);
    const size_t slash = result.find_last_of("\\/");
    return slash == std::string::npos ? "." : result.substr(0, slash);
}

std::string ConfigPath() {
    return ExeDirectory() + "\\BL1GOTYVR.ini";
}

void SetDefaults(HWND window) {
    for (const auto& field : kFields) SetDlgItemTextA(window, field.id, field.defaultValue);
    CheckDlgButton(window, kSameFrameCheck, BST_UNCHECKED);
    CheckDlgButton(window, kReverseEyesCheck, BST_UNCHECKED);
    CheckDlgButton(window, kLoggingCheck, BST_CHECKED);
    CheckDlgButton(window, kDotCheck, BST_CHECKED);
    CheckDlgButton(window, kHmdDirectionCheck, BST_UNCHECKED);
    CheckDlgButton(window, kPhysicalCrouchCheck, BST_CHECKED);
    CheckDlgButton(window, kPostFxCheck, BST_UNCHECKED);
    CheckDlgButton(window, kFsrCheck, BST_CHECKED);
}

void ApplyRenderPreset(HWND window, int id) {
    for (const auto& preset : kRenderPresets) {
        if (preset.id != id) continue;
        SetDlgItemTextA(window, 101, preset.width);
        SetDlgItemTextA(window, 102, preset.height);
        return;
    }
}

void LoadSettings(HWND window) {
    const std::string path = ConfigPath();
    for (const auto& field : kFields) {
        char value[32] = {};
        GetPrivateProfileStringA(field.section, field.key, field.defaultValue,
                                 value, sizeof(value), path.c_str());
        SetDlgItemTextA(window, field.id, value);
    }
    CheckDlgButton(window, kSameFrameCheck,
        GetPrivateProfileIntA("Stereo", "SameFrameStereo", 0, path.c_str()) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(window, kReverseEyesCheck,
        GetPrivateProfileIntA("Stereo", "ReverseEyes", 0, path.c_str()) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(window, kLoggingCheck,
        GetPrivateProfileIntA("Debug", "Logging", 1, path.c_str()) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(window, kDotCheck,
        GetPrivateProfileIntA("Dot", "Enabled", 1, path.c_str()) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(window, kHmdDirectionCheck,
        GetPrivateProfileIntA("Input", "HmdDirectedLocomotion", 0, path.c_str())
            ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(window, kPhysicalCrouchCheck,
        GetPrivateProfileIntA("Input", "PhysicalCrouch", 1, path.c_str())
            ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(window, kPostFxCheck,
        GetPrivateProfileIntA("PostFx", "Enabled", 0, path.c_str())
            ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(window, kFsrCheck,
        GetPrivateProfileIntA("PostFx", "FsrUpscale", 1, path.c_str())
            ? BST_CHECKED : BST_UNCHECKED);
}

bool ReplaceIniValue(std::vector<std::string>& lines, const char* key, int value) {
    const std::string prefix = std::string(key) + "=";
    for (auto& line : lines) {
        if (_strnicmp(line.c_str(), prefix.c_str(), prefix.size()) == 0) {
            line = prefix + std::to_string(value);
            return true;
        }
    }
    return false;
}

bool WriteIniLines(const std::string& path, const std::vector<std::string>& lines) {
    // Players commonly lock the game configs read-only; write through the
    // attribute and restore it afterwards.
    const DWORD attributes = GetFileAttributesA(path.c_str());
    const bool wasReadonly = attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    if (wasReadonly)
        SetFileAttributesA(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    bool written = false;
    {
        std::ofstream output(path, std::ios::trunc);
        if (output) {
            for (const auto& outputLine : lines) output << outputLine << "\n";
            written = true;
        }
    }
    if (wasReadonly)
        SetFileAttributesA(path.c_str(), attributes);
    return written;
}

bool UpdateGameResolution(int width, int height) {
    char documents[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents))) {
        return false;
    }
    const std::array<std::string, 2> candidates = {
        std::string(documents) + "\\My Games\\Borderlands Game of the Year\\WillowGame\\Config\\WillowEngine.ini",
        std::string(documents) + "\\My Games\\Borderlands Game of the Year Enhanced\\WillowGame\\Config\\WillowEngine.ini"
    };
    for (const auto& path : candidates) {
        std::ifstream input(path);
        if (!input) continue;
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        const bool foundWidth = ReplaceIniValue(lines, "ResX", width);
        const bool foundHeight = ReplaceIniValue(lines, "ResY", height);
        if (!foundWidth || !foundHeight) continue;
        return WriteIniLines(path, lines);
    }
    return false;
}

bool DisableGameWeaponBob() {
    char documents[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr,
                                SHGFP_TYPE_CURRENT, documents))) {
        return false;
    }
    const std::array<std::string, 2> candidates = {
        std::string(documents) + "\\My Games\\Borderlands Game of the Year\\WillowGame\\Config\\WillowGame.ini",
        std::string(documents) + "\\My Games\\Borderlands Game of the Year Enhanced\\WillowGame\\Config\\WillowGame.ini"
    };
    for (const auto& path : candidates) {
        std::ifstream input(path);
        if (!input) continue;
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        if (!ReplaceIniValue(lines, "bWeaponBob", 0)) continue;
        return WriteIniLines(path, lines);
    }
    return false;
}

void SaveSettings(HWND window) {
    const std::string path = ConfigPath();
    bl1gotyvr::configurator::SettingsValues values;
    for (size_t i = 0; i < kFields.size(); ++i) {
        char text[128] = {};
        GetDlgItemTextA(window, kFields[i].id, text, sizeof(text));
        values.fields[i] = text;
    }
    auto checked = [&](int id) {
        return IsDlgButtonChecked(window, id) == BST_CHECKED;
    };
    values.sameFrameStereo = checked(kSameFrameCheck);
    values.reverseEyes = checked(kReverseEyesCheck);
    values.logging = checked(kLoggingCheck);
    values.dot = checked(kDotCheck);
    values.hmdDirectedMovement = checked(kHmdDirectionCheck);
    values.physicalCrouch = checked(kPhysicalCrouchCheck);
    values.postFx = checked(kPostFxCheck);
    values.fsr = checked(kFsrCheck);
    std::string error;
    if (!bl1gotyvr::configurator::SaveModSettings(path.c_str(), values, error)) {
        MessageBoxA(window, error.c_str(), "Settings not saved", MB_OK | MB_ICONWARNING);
        return;
    }
    int width = 0, height = 0;
    bl1gotyvr::configurator::ParseField(kFields[0], values.fields[0].c_str(), width);
    bl1gotyvr::configurator::ParseField(kFields[1], values.fields[1].c_str(), height);

    const bool gameIniUpdated = UpdateGameResolution(width, height);
    const bool weaponBobDisabled = DisableGameWeaponBob();
    const char* message = gameIniUpdated && weaponBobDisabled
        ? "Settings saved. Native weapon bob is disabled. Restart the game after changing display settings."
        : "Settings saved. A game INI was not found; verify resolution and bWeaponBob=false before restarting.";
    MessageBoxA(window, message, "BL1 GOTY VR Config", MB_OK | MB_ICONINFORMATION);
}

void CreateLabel(HWND window, const char* text, int x, int y, int width) {
    CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE,
                    x, y, width, 22, window, nullptr, nullptr, nullptr);
}

void CreateCheckbox(HWND window, const char* text, int id, int x, int y) {
    CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    x, y, 190, 24, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        CreateWindowExA(0, "BUTTON", "Display and optics", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        16, 12, 500, 270, window, nullptr, nullptr, nullptr);
        CreateWindowExA(0, "BUTTON", "Tracking and rendering", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        16, 290, 500, 320, window, nullptr, nullptr, nullptr);

        CreateLabel(window, "Render preset", 34, 42, 90);
        const char* presetLabels[] = {"Low", "Medium", "High", "Ultra", "Mega"};
        for (size_t i = 0; i < kRenderPresets.size(); ++i) {
            CreateWindowExA(0, "BUTTON", presetLabels[i], WS_CHILD | WS_VISIBLE,
                            120 + static_cast<int>(i) * 76, 37, 70, 27, window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRenderPresets[i].id)),
                            nullptr, nullptr);
        }

        CreateWindowExA(0, "BUTTON", "Post-processing (native)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        16, 616, 500, 136, window, nullptr, nullptr, nullptr);

        int postIndex = 0;
        for (size_t i = 0; i < kFields.size(); ++i) {
            int x = 34;
            int y = 78 + static_cast<int>(i) * 32;
            if (std::string(kFields[i].section) == "PostFx") {
                x = (postIndex / 3) ? 274 : 34;
                y = 646 + (postIndex % 3) * 32;
                ++postIndex;
            } else if (i >= 9) {
                x = 274;
                y = 322 + (static_cast<int>(i) - 9) * 32;
            } else if (i >= 6) {
                y = 322 + (static_cast<int>(i) - 6) * 32;
            }
            CreateLabel(window, kFields[i].label, x, y, 128);
            CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL |
                                (i < 2 ? ES_READONLY : 0),
                            x + 132, y - 3, 88, 25, window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFields[i].id)),
                            nullptr, nullptr);
        }

        CreateCheckbox(window, "Post FX enabled", kPostFxCheck, 34, 724);
        CreateCheckbox(window, "FSR1 upscale", kFsrCheck, 274, 724);

        CreateCheckbox(window, "Same-frame stereo", kSameFrameCheck, 34, 772);
        CreateCheckbox(window, "Reverse eyes", kReverseEyesCheck, 274, 772);
        CreateCheckbox(window, "Debug logging", kLoggingCheck, 274, 802);
        CreateCheckbox(window, "Show aim dot", kDotCheck, 34, 832);
        CreateCheckbox(window, "HMD-directed movement", kHmdDirectionCheck, 274, 832);
        CreateCheckbox(window, "Physical crouch", kPhysicalCrouchCheck, 274, 862);

        CreateWindowExA(0, "BUTTON", "Save settings", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        154, 890, 110, 34, window,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButton)), nullptr, nullptr);
        CreateWindowExA(0, "BUTTON", "Defaults", WS_CHILD | WS_VISIBLE,
                        278, 890, 100, 34, window,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDefaultsButton)), nullptr, nullptr);
        CreateLabel(window, "Convergence 10 = recommended; 0 = parallel. Applies live after Save.",
                    66, 936, 430);
        LoadSettings(window);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kSaveButton) SaveSettings(window);
        if (LOWORD(wParam) == kDefaultsButton) SetDefaults(window);
        if (LOWORD(wParam) >= kLowPreset && LOWORD(wParam) <= kMegaUltraPreset)
            ApplyRenderPreset(window, LOWORD(wParam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    WNDCLASSEXA windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = "BL1GOTYVRConfigWindow";
    if (!RegisterClassExA(&windowClass)) return 1;

    HWND window = CreateWindowExA(0, windowClass.lpszClassName, "Borderlands GOTY Enhanced VR Config",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 550, 1006, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageA(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return static_cast<int>(message.wParam);
}
