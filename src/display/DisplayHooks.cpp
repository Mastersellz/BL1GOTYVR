#include "DisplayHooks.hpp"

#include "../config/Config.hpp"
#include "../core/VRMod.hpp"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace bl1gotyvr::display {
namespace {

using EnumDisplaySettingsAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*);
using EnumDisplaySettingsWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
using GetSystemMetricsFn = int(WINAPI*)(int);
using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
using GetClientRectFn = BOOL(WINAPI*)(HWND, LPRECT);
using GetMonitorInfoAFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using GetMonitorInfoWFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);

EnumDisplaySettingsAFn s_enumDisplaySettingsA = nullptr;
EnumDisplaySettingsWFn s_enumDisplaySettingsW = nullptr;
GetSystemMetricsFn s_getSystemMetrics = nullptr;
GetSystemMetricsForDpiFn s_getSystemMetricsForDpi = nullptr;
GetClientRectFn s_getClientRect = nullptr;
GetMonitorInfoAFn s_getMonitorInfoA = nullptr;
GetMonitorInfoWFn s_getMonitorInfoW = nullptr;
std::atomic<HWND> s_gameWindow{nullptr};
std::atomic<std::uint32_t> s_targetResolution{0};
std::atomic<bool> s_windowResolutionApplied{false};
char s_primaryDeviceA[CCHDEVICENAME] = {};
wchar_t s_primaryDeviceW[CCHDEVICENAME] = {};

bool IsPrimaryDisplay(LPCSTR deviceName) {
    return !deviceName || !deviceName[0] ||
        (s_primaryDeviceA[0] && _stricmp(deviceName, s_primaryDeviceA) == 0);
}

bool IsPrimaryDisplay(LPCWSTR deviceName) {
    return !deviceName || !deviceName[0] ||
        (s_primaryDeviceW[0] && _wcsicmp(deviceName, s_primaryDeviceW) == 0);
}

bool IsGameWindow(HWND window) {
    if (!window) return false;
    if (window == s_gameWindow.load(std::memory_order_relaxed)) return true;
    char className[64] = {};
    if (!GetClassNameA(window, className, sizeof(className))) return false;
    return _stricmp(className, "LaunchUnrealUWindowsClient") == 0 ||
           _stricmp(className, "WndEngineUnreal") == 0;
}

BOOL WINAPI HookedEnumDisplaySettingsA(LPCSTR deviceName, DWORD modeNumber,
                                        DEVMODEA* mode) {
    const BOOL result = s_enumDisplaySettingsA(deviceName, modeNumber, mode);
    const std::uint32_t target = s_targetResolution.load();
    if (result && mode && target && IsPrimaryDisplay(deviceName) &&
        (modeNumber == ENUM_CURRENT_SETTINGS || modeNumber == ENUM_REGISTRY_SETTINGS)) {
        mode->dmPelsWidth = target;
        mode->dmPelsHeight = target;
        mode->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
    }
    return result;
}

BOOL WINAPI HookedEnumDisplaySettingsW(LPCWSTR deviceName, DWORD modeNumber,
                                        DEVMODEW* mode) {
    const BOOL result = s_enumDisplaySettingsW(deviceName, modeNumber, mode);
    const std::uint32_t target = s_targetResolution.load();
    if (result && mode && target && IsPrimaryDisplay(deviceName) &&
        (modeNumber == ENUM_CURRENT_SETTINGS || modeNumber == ENUM_REGISTRY_SETTINGS)) {
        mode->dmPelsWidth = target;
        mode->dmPelsHeight = target;
        mode->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
    }
    return result;
}

int WINAPI HookedGetSystemMetrics(int index) {
    const std::uint32_t target = s_targetResolution.load();
    if (target && (index == SM_CXSCREEN || index == SM_CXFULLSCREEN ||
                   index == SM_CYSCREEN || index == SM_CYFULLSCREEN))
        return static_cast<int>(target);
    return s_getSystemMetrics(index);
}

int WINAPI HookedGetSystemMetricsForDpi(int index, UINT dpi) {
    const std::uint32_t target = s_targetResolution.load();
    if (target && (index == SM_CXSCREEN || index == SM_CXFULLSCREEN ||
                   index == SM_CYSCREEN || index == SM_CYFULLSCREEN))
        return static_cast<int>(target);
    return s_getSystemMetricsForDpi(index, dpi);
}

BOOL WINAPI HookedGetClientRect(HWND window, LPRECT rect) {
    const BOOL result = s_getClientRect(window, rect);
    const std::uint32_t target = s_targetResolution.load();
    if (result && rect && target && IsGameWindow(window)) {
        rect->left = rect->top = 0;
        rect->right = static_cast<LONG>(target);
        rect->bottom = static_cast<LONG>(target);
    }
    return result;
}

void SpoofMonitorRect(LPRECT rect) {
    const std::uint32_t target = s_targetResolution.load();
    if (!rect || !target) return;
    rect->right = rect->left + static_cast<LONG>(target);
    rect->bottom = rect->top + static_cast<LONG>(target);
}

BOOL WINAPI HookedGetMonitorInfoA(HMONITOR monitor, LPMONITORINFO info) {
    const BOOL result = s_getMonitorInfoA(monitor, info);
    if (result && info && (info->dwFlags & MONITORINFOF_PRIMARY)) {
        SpoofMonitorRect(&info->rcMonitor);
        SpoofMonitorRect(&info->rcWork);
    }
    return result;
}

BOOL WINAPI HookedGetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info) {
    const BOOL result = s_getMonitorInfoW(monitor, info);
    if (result && info && (info->dwFlags & MONITORINFOF_PRIMARY)) {
        SpoofMonitorRect(&info->rcMonitor);
        SpoofMonitorRect(&info->rcWork);
    }
    return result;
}

void FindPrimaryDisplayName() {
    const HMONITOR monitor = MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXA infoA = {};
    infoA.cbSize = sizeof(infoA);
    if (GetMonitorInfoA(monitor, &infoA)) strcpy_s(s_primaryDeviceA, infoA.szDevice);
    MONITORINFOEXW infoW = {};
    infoW.cbSize = sizeof(infoW);
    if (GetMonitorInfoW(monitor, &infoW)) wcscpy_s(s_primaryDeviceW, infoW.szDevice);
}

} // namespace

bool Initialize() {
    const auto& settings = config::Get();
    if (settings.render_width != settings.render_height || settings.render_width < 512) {
        Log("[Display] Square resolution spoof disabled for %dx%d",
            settings.render_width, settings.render_height);
        return true;
    }
    const std::uint32_t target = static_cast<std::uint32_t>(settings.render_width);
    s_targetResolution.store(target);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    FindPrimaryDisplayName();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void* enumA = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsA")) : nullptr;
    void* enumW = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsW")) : nullptr;
    void* metrics = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "GetSystemMetrics")) : nullptr;
    void* metricsDpi = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "GetSystemMetricsForDpi")) : nullptr;
    void* clientRect = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "GetClientRect")) : nullptr;
    void* monitorA = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoA")) : nullptr;
    void* monitorW = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoW")) : nullptr;
    if (!enumA || !enumW || !metrics || !metricsDpi || !clientRect || !monitorA || !monitorW ||
        MH_CreateHook(enumA, &HookedEnumDisplaySettingsA, reinterpret_cast<void**>(&s_enumDisplaySettingsA)) != MH_OK ||
        MH_CreateHook(enumW, &HookedEnumDisplaySettingsW, reinterpret_cast<void**>(&s_enumDisplaySettingsW)) != MH_OK ||
        MH_CreateHook(metrics, &HookedGetSystemMetrics, reinterpret_cast<void**>(&s_getSystemMetrics)) != MH_OK ||
        MH_CreateHook(metricsDpi, &HookedGetSystemMetricsForDpi, reinterpret_cast<void**>(&s_getSystemMetricsForDpi)) != MH_OK ||
        MH_CreateHook(clientRect, &HookedGetClientRect, reinterpret_cast<void**>(&s_getClientRect)) != MH_OK ||
        MH_CreateHook(monitorA, &HookedGetMonitorInfoA, reinterpret_cast<void**>(&s_getMonitorInfoA)) != MH_OK ||
        MH_CreateHook(monitorW, &HookedGetMonitorInfoW, reinterpret_cast<void**>(&s_getMonitorInfoW)) != MH_OK ||
        MH_EnableHook(enumA) != MH_OK || MH_EnableHook(enumW) != MH_OK ||
        MH_EnableHook(metrics) != MH_OK || MH_EnableHook(metricsDpi) != MH_OK ||
        MH_EnableHook(clientRect) != MH_OK || MH_EnableHook(monitorA) != MH_OK ||
        MH_EnableHook(monitorW) != MH_OK) {
        Log("[Display] ERROR: square display hook installation failed");
        return false;
    }
    Log("[Display] Square display spoof active: %ux%u", target, target);
    return true;
}

void SetGameWindow(HWND window) {
    if (window) s_gameWindow.store(window);
}

void ApplyGameWindowResolution(HWND window) {
    const std::uint32_t target = s_targetResolution.load();
    if (!window || !target) return;
    bool expected = false;
    if (!s_windowResolutionApplied.compare_exchange_strong(expected, true)) return;
    SetGameWindow(window);
    RECT client = {}, outer = {};
    int borderWidth = 0, borderHeight = 0;
    if (s_getClientRect && s_getClientRect(window, &client) && GetWindowRect(window, &outer)) {
        borderWidth = (outer.right - outer.left) - (client.right - client.left);
        borderHeight = (outer.bottom - outer.top) - (client.bottom - client.top);
    }
    const int width = static_cast<int>(target) + (std::max)(0, borderWidth);
    const int height = static_cast<int>(target) + (std::max)(0, borderHeight);
    if (!SetWindowPos(window, nullptr, 0, 0, width, height,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
        s_windowResolutionApplied.store(false);
        Log("[Display] ERROR: game window resize failed");
        return;
    }
    Log("[Display] Requested game client=%ux%u outer=%dx%d",
        target, target, width, height);
}

std::uint32_t TargetResolution() {
    return s_targetResolution.load();
}

} // namespace bl1gotyvr::display
