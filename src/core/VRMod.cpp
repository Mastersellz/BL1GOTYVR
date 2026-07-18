#include "VRMod.hpp"
#include "globals.hpp"
#include "../config/Config.hpp"
#include <cstdio>
#include <cstdarg>
#include <Windows.h>
#include <objbase.h>

namespace bl1gotyvr {

namespace d3d11 { bool InstallHooks(); }
namespace camera { void StartScanner(); }

static HANDLE s_logFile = INVALID_HANDLE_VALUE;
static bool s_initialized = false;

void Log(const char* fmt, ...) {
    if (s_initialized && !config::Get().debug_logging) return;
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

bool IsInitialized() { return s_initialized; }
void SetInitialized(bool v) { s_initialized = v; }

static void OpenLog() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    // Replace exe name with BL1GOTYVR.log in same directory
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) {
        strcpy(lastSlash + 1, "BL1GOTYVR.log");
    }
    s_logFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Log("[BL1GOTYVR] Log opened: %s", path);
}

static void CloseLog() {
    Log("[BL1GOTYVR] Shutting down");
    if (s_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(s_logFile);
        s_logFile = INVALID_HANDLE_VALUE;
    }
}

static DWORD WINAPI InitializeThread(LPVOID) {
    Sleep(250);
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Log("[BL1GOTYVR] Init thread CoInitializeEx: 0x%08X", comInit);

    if (!d3d11::InstallHooks()) {
        Log("[BL1GOTYVR] ERROR: D3D11 hooks were not installed");
    } else {
        Log("[BL1GOTYVR] D3D11 hooks installed");
    }

    camera::StartScanner();
    Log("[BL1GOTYVR] Camera scanner started");
    Log("[BL1GOTYVR] OpenXR will initialize on first Present");
    SetInitialized(true);
    if (SUCCEEDED(comInit)) CoUninitialize();
    return 0;
}

} // namespace bl1gotyvr

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    using namespace bl1gotyvr;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        OpenLog();
        Log("[BL1GOTYVR] DLL attached to process");

        char configPath[MAX_PATH] = {};
        GetModuleFileNameA(hModule, configPath, MAX_PATH);
        if (char* slash = strrchr(configPath, '\\')) {
            strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - configPath),
                     "BL1GOTYVR.ini");
        }
        config::Load(configPath);

        HANDLE initThread = CreateThread(nullptr, 0, InitializeThread, nullptr, 0, nullptr);
        if (initThread) CloseHandle(initThread);
        else Log("[BL1GOTYVR] ERROR: Initialization thread creation failed: %lu", GetLastError());
    }
    else if (reason == DLL_PROCESS_DETACH) {
        CloseLog();
    }
    return TRUE;
}
