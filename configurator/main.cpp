#include <Windows.h>
#include <ShlObj.h>

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
constexpr int kRollCheck = 204;
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

constexpr std::array<Field, 20> kFields = {{
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
    {111, "Aim target distance (m)", "Dot", "ConvergenceDistance", "20.0", 1.0f, 100.0f, false},
    {112, "OpenXR refresh rate (Hz, 0=default)", "OpenXR", "RefreshRateHz", "72.0", 0.0f, 240.0f, false},
    {113, "Arm reach scale", "Hands", "ArmReachScale", "1.60", 1.0f, 2.0f, false},
    {114, "Hand cut position", "Visibility", "VanillaHandsCutThreshold", "70.0", 20.0f, 90.0f, false},
    {115, "Melee range scale", "Melee", "RangeMultiplier", "1.60", 1.0f, 5.0f, false},
    {120, "Post FX sharpen", "PostFx", "Sharpen", "0.35", 0.0f, 1.0f, false},
    {121, "Post FX contrast", "PostFx", "Contrast", "1.00", 0.5f, 2.0f, false},
    {122, "Post FX saturation", "PostFx", "Saturation", "1.00", 0.0f, 2.0f, false},
    {123, "Post FX brightness", "PostFx", "Brightness", "0.00", -0.5f, 0.5f, false},
    {124, "Post FX gamma", "PostFx", "Gamma", "1.00", 0.5f, 2.0f, false},
}};

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
    CheckDlgButton(window, kSameFrameCheck, BST_CHECKED);
    CheckDlgButton(window, kReverseEyesCheck, BST_UNCHECKED);
    CheckDlgButton(window, kRollCheck, BST_UNCHECKED);
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
        GetPrivateProfileIntA("Stereo", "SameFrameStereo", 1, path.c_str()) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(window, kReverseEyesCheck,
        GetPrivateProfileIntA("Stereo", "ReverseEyes", 0, path.c_str()) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(window, kRollCheck,
        GetPrivateProfileIntA("Tracking", "RollEnabled", 1, path.c_str()) ? BST_CHECKED : BST_UNCHECKED);
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

bool ReadField(HWND window, const Field& field, std::string& text, int& integerValue) {
    char value[32] = {};
    GetDlgItemTextA(window, field.id, value, sizeof(value));
    char* end = nullptr;
    const float parsed = strtof(value, &end);
    if (!end || *end != '\0' || !std::isfinite(parsed) ||
        parsed < field.minimum || parsed > field.maximum) return false;
    if (field.integer && parsed != static_cast<float>(static_cast<int>(parsed))) return false;
    text = value;
    integerValue = static_cast<int>(parsed);
    return true;
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
    int width = 0;
    int height = 0;
    std::string widthText;
    std::string heightText;
    if (!ReadField(window, kFields[0], widthText, width) ||
        !ReadField(window, kFields[1], heightText, height)) {
        MessageBoxA(window, "Select a valid square render preset.",
                    "Invalid resolution", MB_OK | MB_ICONWARNING);
        return;
    }
    bool supportedPreset = false;
    for (const auto& preset : kRenderPresets) {
        if (width == atoi(preset.width) && height == atoi(preset.height)) {
            supportedPreset = true;
            break;
        }
    }
    if (!supportedPreset) {
        MessageBoxA(window,
            "Unsupported resolution. Choose Low, Medium, High, Ultra, or Mega.",
            "Invalid resolution", MB_OK | MB_ICONWARNING);
        return;
    }
    for (const auto& field : kFields) {
        std::string text;
        int integerValue = 0;
        if (!ReadField(window, field, text, integerValue)) {
            std::string message = std::string("Invalid value for ") + field.label +
                ".\nAllowed range: " + std::to_string(field.minimum) + " - " +
                std::to_string(field.maximum) + ".";
            MessageBoxA(window, message.c_str(), "Invalid setting", MB_OK | MB_ICONWARNING);
            return;
        }
        WritePrivateProfileStringA(field.section, field.key, text.c_str(), path.c_str());
        if (field.id == 101) width = integerValue;
        if (field.id == 102) height = integerValue;
    }
    WritePrivateProfileStringA("Stereo", "SameFrameStereo",
        IsDlgButtonChecked(window, kSameFrameCheck) == BST_CHECKED ? "1" : "0", path.c_str());
    WritePrivateProfileStringA("Stereo", "ReverseEyes",
        IsDlgButtonChecked(window, kReverseEyesCheck) == BST_CHECKED ? "1" : "0", path.c_str());
    WritePrivateProfileStringA("Tracking", "RollEnabled",
        IsDlgButtonChecked(window, kRollCheck) == BST_CHECKED ? "1" : "0", path.c_str());
    WritePrivateProfileStringA("Debug", "Logging",
        IsDlgButtonChecked(window, kLoggingCheck) == BST_CHECKED ? "1" : "0", path.c_str());
    WritePrivateProfileStringA("Dot", "Enabled",
        IsDlgButtonChecked(window, kDotCheck) == BST_CHECKED ? "1" : "0", path.c_str());
    WritePrivateProfileStringA("Input", "HmdDirectedLocomotion",
        IsDlgButtonChecked(window, kHmdDirectionCheck) == BST_CHECKED ? "1" : "0",
        path.c_str());
    WritePrivateProfileStringA("Input", "PhysicalCrouch",
        IsDlgButtonChecked(window, kPhysicalCrouchCheck) == BST_CHECKED ? "1" : "0",
        path.c_str());

    // Post-processing values imply intent: enabling the effect when any value
    // is non-neutral avoids saving a tuned preset that silently does nothing
    // (the checkbox stays a hard off-switch only when everything is neutral).
    char postFxValues[5][32] = {};
    GetDlgItemTextA(window, 120, postFxValues[0], sizeof(postFxValues[0]));
    GetDlgItemTextA(window, 121, postFxValues[1], sizeof(postFxValues[1]));
    GetDlgItemTextA(window, 122, postFxValues[2], sizeof(postFxValues[2]));
    GetDlgItemTextA(window, 123, postFxValues[3], sizeof(postFxValues[3]));
    GetDlgItemTextA(window, 124, postFxValues[4], sizeof(postFxValues[4]));
    const float postFxSharpen = strtof(postFxValues[0], nullptr);
    const float postFxContrast = strtof(postFxValues[1], nullptr);
    const float postFxSaturation = strtof(postFxValues[2], nullptr);
    const float postFxBrightness = strtof(postFxValues[3], nullptr);
    const float postFxGamma = strtof(postFxValues[4], nullptr);
    const bool postFxValuesActive = postFxSharpen > 0.001f ||
        std::fabs(postFxContrast - 1.0f) > 0.001f ||
        std::fabs(postFxSaturation - 1.0f) > 0.001f ||
        std::fabs(postFxBrightness) > 0.001f ||
        std::fabs(postFxGamma - 1.0f) > 0.001f;
    const bool postFxEnabled = postFxValuesActive ||
        IsDlgButtonChecked(window, kPostFxCheck) == BST_CHECKED;
    WritePrivateProfileStringA("PostFx", "Enabled",
        postFxEnabled ? "1" : "0", path.c_str());
    CheckDlgButton(window, kPostFxCheck, postFxEnabled ? BST_CHECKED : BST_UNCHECKED);
    WritePrivateProfileStringA("PostFx", "FsrUpscale",
        IsDlgButtonChecked(window, kFsrCheck) == BST_CHECKED ? "1" : "0",
        path.c_str());

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

        for (size_t i = 0; i < kFields.size(); ++i) {
            int x = 34;
            int y = 78 + static_cast<int>(i) * 32;
            if (i >= 15) {
                const int postIndex = static_cast<int>(i) - 15;
                x = (postIndex / 3) ? 274 : 34;
                y = 646 + (postIndex % 3) * 32;
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
        CreateCheckbox(window, "Enable camera roll", kRollCheck, 34, 802);
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
