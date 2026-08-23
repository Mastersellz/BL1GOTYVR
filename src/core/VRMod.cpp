#include "VRMod.hpp"
#include "globals.hpp"
#include "CommandSystem.hpp"
#include "../config/Config.hpp"
#include "../camera/CameraHook.hpp"
#include "../d3d11/D3D11Hooks.hpp"
#include "../display/DisplayHooks.hpp"
#include "../input/AimHook.hpp"
#include "../input/InputHook.hpp"
#include "../input/WeaponAimSystem.hpp"
#include "../player/ArmIKSystem.hpp"
#include <cstdio>
#include <cstdarg>
#include <string>
#include <Windows.h>
#include <objbase.h>

static HANDLE s_logFile = INVALID_HANDLE_VALUE;
static HANDLE s_displayReadyEvent = nullptr;
static bool s_initialized = false;

void bl1gotyvr::Log(const char* fmt, ...) {
    if (s_initialized && !bl1gotyvr::config::Get().debug_logging) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    if (s_logFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(s_logFile, buf, (DWORD)strlen(buf), &written, nullptr);
        WriteFile(s_logFile, "\r\n", 2, &written, nullptr);
    }
}

static void LogInternal(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    bl1gotyvr::Log("%s", buf);
}

static void OpenLog() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) {
        strcpy(lastSlash + 1, "BL1GOTYVR.log");
    }
    s_logFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LogInternal("[BL1GOTYVR] Log opened: %s", path);
}

static void CloseLog() {
    LogInternal("[BL1GOTYVR] Shutting down");
    if (s_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(s_logFile);
        s_logFile = INVALID_HANDLE_VALUE;
    }
}

static void RegisterCommands() {
    auto& cmd = bl1gotyvr::CommandSystem::Instance();

    cmd.RegisterCommand("recenter", [](::std::string) {
        bl1gotyvr::camera::RequestRecenter();
        LogInternal("[Command] Camera recentered");
    }, "Recenter camera (integer yaw system)");

    cmd.RegisterCommand("aim", [](::std::string args) {
        auto& aim = bl1gotyvr::input::AimHook::Instance();
        if (args == "on") {
            aim.SetEnabled(true);
            LogInternal("[Command] Decoupled aim enabled");
        } else if (args == "off") {
            aim.SetEnabled(false);
            LogInternal("[Command] Decoupled aim disabled");
        } else if (args == "recenter") {
            aim.Recenter();
            LogInternal("[Command] Aim recentered");
        } else if (args == "status") {
            LogInternal("[Command] Aim: %s", aim.IsEnabled() ? "enabled" : "disabled");
        } else {
            LogInternal("[Command] Usage: aim <on|off|recenter|status>");
        }
    }, "Control decoupled aim system");

    cmd.RegisterCommand("aimtrim", [](::std::string args) {
        auto& aim = bl1gotyvr::input::AimHook::Instance();
        int32_t pitch = 0, yaw = 0, roll = 0;

        if (sscanf_s(args.c_str(), "%d %d %d", &pitch, &yaw, &roll) == 3) {
            aim.SetTrim(pitch, yaw, roll);
            LogInternal("[Command] Aim trim set: pitch=%d yaw=%d roll=%d", pitch, yaw, roll);
        } else {
            aim.GetTrim(pitch, yaw, roll);
            LogInternal("[Command] Current aim trim: pitch=%d yaw=%d roll=%d", pitch, yaw, roll);
            LogInternal("[Command] Usage: aimtrim <pitch> <yaw> <roll>");
        }
    }, "Set/get aim trim offsets");

    cmd.RegisterCommand("ballistics", [](::std::string args) {
        auto& aim = bl1gotyvr::input::WeaponAimSystem::Instance();
        if (args == "on") {
            aim.SetBallisticOverrideEnabled(true);
        } else if (args == "off") {
            aim.SetBallisticOverrideEnabled(false);
        } else if (args != "status" && !args.empty()) {
            LogInternal("[Command] Usage: ballistics <on|off|status>");
            return;
        }
        LogInternal("[Command] Ballistic aim override: %s writes=%llu",
            aim.IsBallisticOverrideEnabled() ? "enabled" : "disabled",
            static_cast<unsigned long long>(aim.GetOverrideCount()));
    }, "Control guarded GetAdjustedAim VR override");

    cmd.RegisterCommand("arms", [](::std::string args) {
        auto& arms = bl1gotyvr::player::ArmIKSystem::Instance();
        if (args == "on") {
            arms.SetEnabled(true);
        } else if (args == "off") {
            arms.SetEnabled(false);
        } else if (args == "rescan") {
            arms.RequestRescan();
        } else if (args == "sim on") {
            arms.SetSimulationEnabled(true);
        } else if (args == "sim off") {
            arms.SetSimulationEnabled(false);
        } else if (args == "status" || args.empty()) {
            const auto status = arms.GetStatus();
            LogInternal("[Command] ArmIK: enabled=%d simulation=%d hook=%d "
                "calls=%llu applies=%llu "
                "rig=%d component=%p mesh=%p bones=%d right=%d/%d/%d "
                "left=%d/%d/%d solve=%llu",
                arms.IsEnabled(), arms.IsSimulationEnabled(), status.poseHookInstalled,
                static_cast<unsigned long long>(status.poseHookCalls),
                static_cast<unsigned long long>(status.poseHookApplies), status.rigValid,
                reinterpret_cast<void*>(status.component),
                reinterpret_cast<void*>(status.skeletalMesh), status.boneCount,
                status.rightShoulder, status.rightElbow, status.rightWrist,
                status.leftShoulder, status.leftElbow, status.leftWrist,
                static_cast<unsigned long long>(status.solvedGeneration));
        } else {
            LogInternal("[Command] Usage: arms <on|off|rescan|sim on|sim off|status>");
        }
    }, "Control first-person arm IK");

    cmd.RegisterCommand("visibility", [](::std::string args) {
        auto& arms = bl1gotyvr::player::ArmIKSystem::Instance();
        if (args == "scan") {
            bl1gotyvr::camera::RequestVisibilityInventoryRefresh();
            return;
        }
        if (args == "on") {
            arms.SetVisibilityEnabled(true);
            return;
        }
        if (args == "off") {
            arms.SetVisibilityEnabled(false);
            return;
        }
        if (args != "status" && !args.empty()) {
            LogInternal("[Command] Usage: visibility <on|off|scan|status>");
            return;
        }

        const auto inventory = arms.GetComponentInventory();
        LogInternal("[Command] Visibility inventory: generation=%llu entries=%zu "
            "pawnValid=%d weaponValid=%d controller=%p pawn=%p weapon=%p "
            "weaponComponents=%d truncated=%d writes=%s",
            static_cast<unsigned long long>(inventory.generation), inventory.count,
            inventory.pawnIdentityValid, inventory.weaponIdentityValid,
            reinterpret_cast<void*>(inventory.controller),
            reinterpret_cast<void*>(inventory.pawn),
            reinterpret_cast<void*>(inventory.weapon), inventory.weaponComponentCount,
            inventory.truncatedComponentCount,
            arms.IsVisibilityEnabled() ? "enabled" : "disabled");
        for (size_t index = 0; index < inventory.count; ++index) {
            const auto& entry = inventory.entries[index];
            const char* role = "unknown";
            switch (entry.role) {
            case bl1gotyvr::player::ComponentRole::ProtectedWeapon:
                role = "protected_weapon";
                break;
            case bl1gotyvr::player::ComponentRole::PawnBody:
                role = "pawn_body";
                break;
            case bl1gotyvr::player::ComponentRole::ProbableFirstPersonArms:
                role = "probable_first_person_arms";
                break;
            default:
                break;
            }
            LogInternal("[Command]   [%zu] %s component=%p object=%s outer=%s mesh=%s "
                "bones=%d distance=%.1f pawn=%d weapon=%d torso=%d lower=%d arms=%d/%d",
                index, role, reinterpret_cast<void*>(entry.component), entry.objectName,
                entry.outerName, entry.meshName, entry.boneCount, entry.cameraDistance,
                entry.exactPawnOuter, entry.exactWeaponOuter, entry.torsoSignature,
                entry.lowerBodySignature, entry.rightArmChain, entry.leftArmChain);
        }
    }, "Control validated player body and arms visibility");

    cmd.RegisterCommand("ipd", [](::std::string args) {
        float mm = 0;
        if (sscanf_s(args.c_str(), "%f", &mm) == 1 && mm > 0) {
            bl1gotyvr::config::Get().ipd_mm = mm;
            LogInternal("[Command] IPD set to %.1f mm", mm);
        } else {
            LogInternal("[Command] Current IPD: %.1f mm", bl1gotyvr::config::Get().ipd_mm);
            LogInternal("[Command] Usage: ipd <millimeters>");
        }
    }, "Get/set IPD in millimeters");

    cmd.RegisterCommand("fov", [](::std::string args) {
        float deg = 0;
        if (sscanf_s(args.c_str(), "%f", &deg) == 1 && deg > 0) {
            bl1gotyvr::config::Get().fov_degrees = deg;
            LogInternal("[Command] FOV set to %.1f degrees", deg);
        } else {
            LogInternal("[Command] Current FOV: %.1f degrees", bl1gotyvr::config::Get().fov_degrees);
            LogInternal("[Command] Usage: fov <degrees>");
        }
    }, "Get/set FOV in degrees");

    cmd.RegisterCommand("scale", [](::std::string args) {
        float pos = 0, rot = 0;
        if (sscanf_s(args.c_str(), "%f %f", &pos, &rot) == 2) {
            bl1gotyvr::config::Get().positional_scale = pos;
            bl1gotyvr::config::Get().rotation_scale = rot;
            LogInternal("[Command] Scale set: pos=%.2f rot=%.2f", pos, rot);
        } else {
            LogInternal("[Command] Current scale: pos=%.2f rot=%.2f",
                 bl1gotyvr::config::Get().positional_scale, bl1gotyvr::config::Get().rotation_scale);
            LogInternal("[Command] Usage: scale <positional> <rotational>");
        }
    }, "Get/set position and roll rotation scale");

    cmd.RegisterCommand("headyaw", [](::std::string args) {
        float v = 0;
        if (sscanf_s(args.c_str(), "%f", &v) == 1) {
            bl1gotyvr::config::Get().head_yaw_scale = v;
            LogInternal("[Command] Head yaw scale set to %.2f (%s)", v,
                v == 0.0f ? "FREE LOOK" : v >= 1.0f ? "full coupling" : "partial");
        } else {
            LogInternal("[Command] Current head yaw scale: %.2f (%s)",
                bl1gotyvr::config::Get().head_yaw_scale,
                bl1gotyvr::config::Get().head_yaw_scale == 0.0f ? "FREE LOOK" : "");
            LogInternal("[Command] Usage: headyaw <0.0-1.0> (0=free look, 1=full)");
        }
    }, "Head yaw scale: 0=look freely, 1=rotate camera with head");

    cmd.RegisterCommand("headpitch", [](::std::string args) {
        float v = 0;
        if (sscanf_s(args.c_str(), "%f", &v) == 1) {
            bl1gotyvr::config::Get().head_pitch_scale = v;
            LogInternal("[Command] Head pitch scale set to %.2f", v);
        } else {
            LogInternal("[Command] Current head pitch scale: %.2f",
                bl1gotyvr::config::Get().head_pitch_scale);
            LogInternal("[Command] Usage: headpitch <0.0-1.0>");
        }
    }, "Head pitch scale: 0=look freely, 1=rotate camera with head");

    cmd.RegisterCommand("status", [](::std::string) {
        const auto arms = bl1gotyvr::player::ArmIKSystem::Instance().GetStatus();
        LogInternal("[Command] BL1GOTYVR Status:");
        LogInternal("[Command]   Camera: %s", bl1gotyvr::camera::IsCameraFound() ? "found" : "not found");
        LogInternal("[Command]   Multiview: %s", bl1gotyvr::camera::IsNativeMultiviewActive() ? "active" : "inactive");
        LogInternal("[Command]   Aim: %s", bl1gotyvr::input::AimHook::Instance().IsEnabled() ? "enabled" : "disabled");
        LogInternal("[Command]   IPD: %.1f mm", bl1gotyvr::config::Get().ipd_mm);
        LogInternal("[Command]   FOV: %.1f deg", bl1gotyvr::config::Get().fov_degrees);
        LogInternal("[Command]   HeadYaw: %.2f %s", bl1gotyvr::config::Get().head_yaw_scale,
            bl1gotyvr::config::Get().head_yaw_scale == 0.0f ? "(FREE LOOK)" : "");
        LogInternal("[Command]   HeadPitch: %.2f", bl1gotyvr::config::Get().head_pitch_scale);
        LogInternal("[Command]   ArmIK: %s simulation=%d hook=%d calls=%llu applies=%llu component=%p "
            "bones=%d right=%d/%d/%d left=%d/%d/%d solve=%llu",
            arms.rigValid ? "ready" : "discovering",
            bl1gotyvr::player::ArmIKSystem::Instance().IsSimulationEnabled(),
            arms.poseHookInstalled,
            static_cast<unsigned long long>(arms.poseHookCalls),
            static_cast<unsigned long long>(arms.poseHookApplies),
            reinterpret_cast<void*>(arms.component),
            arms.boneCount, arms.rightShoulder, arms.rightElbow, arms.rightWrist,
            arms.leftShoulder, arms.leftElbow, arms.leftWrist,
            static_cast<unsigned long long>(arms.solvedGeneration));
    }, "Show system status");

    cmd.RegisterCommand("help", [&cmd](::std::string) {
        cmd.ListCommands();
    }, "List all commands");

    LogInternal("[Command] Commands registered");
}

static DWORD WINAPI InitializeThread(LPVOID) {
    if (!bl1gotyvr::display::Initialize())
        LogInternal("[BL1GOTYVR] ERROR: Display hooks were not installed");
    if (!bl1gotyvr::d3d11::InstallSteamVrDeviceCompatibility())
        LogInternal("[BL1GOTYVR] ERROR: SteamVR D3D11 compatibility was not installed");
    if (s_displayReadyEvent) SetEvent(s_displayReadyEvent);
    Sleep(250);
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LogInternal("[BL1GOTYVR] Init thread CoInitializeEx: 0x%08X", comInit);

    if (!bl1gotyvr::d3d11::InstallHooks()) {
        LogInternal("[BL1GOTYVR] ERROR: D3D11 hooks were not installed");
    } else {
        LogInternal("[BL1GOTYVR] D3D11 hooks installed");
    }

    bl1gotyvr::camera::StartScanner();
    LogInternal("[BL1GOTYVR] Camera scanner started");
    bl1gotyvr::input::InputHook::Instance().Install();
    bl1gotyvr::player::ArmIKSystem::Instance().StartDiscovery();

    RegisterCommands();
    LogInternal("[BL1GOTYVR] Debug commands registered");

    char cmdPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, cmdPath, MAX_PATH);
    if (char* slash = strrchr(cmdPath, '\\')) {
        strcpy(slash + 1, "BL1GOTYVR_commands.txt");
    }
    bl1gotyvr::CommandSystem::Instance().LoadCommandsFromFile(cmdPath);

    LogInternal("[BL1GOTYVR] OpenXR will initialize on first Present");
    s_initialized = true;
    if (SUCCEEDED(comInit)) CoUninitialize();
    return 0;
}

extern "C" __declspec(dllexport) BOOL WINAPI BL1GOTYVR_WaitForDisplayHooks(DWORD timeoutMs) {
    return s_displayReadyEvent &&
        WaitForSingleObject(s_displayReadyEvent, timeoutMs) == WAIT_OBJECT_0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        OpenLog();
        LogInternal("[BL1GOTYVR] DLL attached to process");
        s_displayReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        char configPath[MAX_PATH] = {};
        GetModuleFileNameA(hModule, configPath, MAX_PATH);
        if (char* slash = strrchr(configPath, '\\')) {
            strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - configPath),
                     "BL1GOTYVR.ini");
        }
        bl1gotyvr::config::Load(configPath);

        HANDLE initThread = CreateThread(nullptr, 0, InitializeThread, nullptr, 0, nullptr);
        if (initThread) CloseHandle(initThread);
        else LogInternal("[BL1GOTYVR] ERROR: Init thread failed: %lu", GetLastError());
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (s_displayReadyEvent) {
            CloseHandle(s_displayReadyEvent);
            s_displayReadyEvent = nullptr;
        }
        CloseLog();
    }
    return TRUE;
}
