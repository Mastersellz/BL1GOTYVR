#include <Windows.h>
#include <dxgi.h>

#include <cwchar>

extern "C" {
FARPROC g_real_ApplyCompatResolutionQuirking = nullptr;
FARPROC g_real_CompatString = nullptr;
FARPROC g_real_CompatValue = nullptr;
FARPROC g_real_DXGIDumpJournal = nullptr;
FARPROC g_real_PIXBeginCapture = nullptr;
FARPROC g_real_PIXEndCapture = nullptr;
FARPROC g_real_PIXGetCaptureState = nullptr;
FARPROC g_real_SetAppCompatStringPointer = nullptr;
FARPROC g_real_UpdateHMDEmulationStatus = nullptr;
FARPROC g_real_CreateDXGIFactory = nullptr;
FARPROC g_real_CreateDXGIFactory1 = nullptr;
FARPROC g_real_CreateDXGIFactory2 = nullptr;
FARPROC g_real_DXGID3D10CreateDevice = nullptr;
FARPROC g_real_DXGID3D10CreateLayeredDevice = nullptr;
FARPROC g_real_DXGID3D10GetLayeredDeviceSize = nullptr;
FARPROC g_real_DXGID3D10RegisterLayers = nullptr;
FARPROC g_real_DXGIDeclareAdapterRemovalSupport = nullptr;
FARPROC g_real_DXGIDisableVBlankVirtualization = nullptr;
FARPROC g_real_DXGIGetDebugInterface1 = nullptr;
FARPROC g_real_DXGIReportAdapterConfiguration = nullptr;
}

namespace {

HMODULE s_proxyModule = nullptr;
INIT_ONCE s_modLoadOnce = INIT_ONCE_STATIC_INIT;

void ProxyLog(const wchar_t* message) {
    OutputDebugStringW(L"[BL1GOTYVR Proxy] ");
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
}

bool ResolveRealDxgi() {
    wchar_t systemDirectory[MAX_PATH] = {};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return false;

    wchar_t path[MAX_PATH] = {};
    if (swprintf_s(path, L"%s\\dxgi.dll", systemDirectory) < 0) return false;
    HMODULE realDxgi = LoadLibraryW(path);
    if (!realDxgi || realDxgi == s_proxyModule) return false;

#define RESOLVE_DXGI(name) \
    g_real_##name = GetProcAddress(realDxgi, #name); \
    if (!g_real_##name) return false
    RESOLVE_DXGI(ApplyCompatResolutionQuirking);
    RESOLVE_DXGI(CompatString);
    RESOLVE_DXGI(CompatValue);
    RESOLVE_DXGI(DXGIDumpJournal);
    RESOLVE_DXGI(PIXBeginCapture);
    RESOLVE_DXGI(PIXEndCapture);
    RESOLVE_DXGI(PIXGetCaptureState);
    RESOLVE_DXGI(SetAppCompatStringPointer);
    RESOLVE_DXGI(UpdateHMDEmulationStatus);
    RESOLVE_DXGI(CreateDXGIFactory);
    RESOLVE_DXGI(CreateDXGIFactory1);
    RESOLVE_DXGI(CreateDXGIFactory2);
    RESOLVE_DXGI(DXGID3D10CreateDevice);
    RESOLVE_DXGI(DXGID3D10CreateLayeredDevice);
    RESOLVE_DXGI(DXGID3D10GetLayeredDeviceSize);
    RESOLVE_DXGI(DXGID3D10RegisterLayers);
    RESOLVE_DXGI(DXGIDeclareAdapterRemovalSupport);
    RESOLVE_DXGI(DXGIDisableVBlankVirtualization);
    RESOLVE_DXGI(DXGIGetDebugInterface1);
    RESOLVE_DXGI(DXGIReportAdapterConfiguration);
#undef RESOLVE_DXGI
    return true;
}

BOOL CALLBACK LoadModOnce(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t modPath[MAX_PATH] = {};
    if (!GetModuleFileNameW(s_proxyModule, modPath, MAX_PATH)) return TRUE;
    wchar_t* filename = wcsrchr(modPath, L'\\');
    if (!filename) return TRUE;
    wcscpy_s(filename + 1, MAX_PATH - static_cast<size_t>(filename + 1 - modPath),
             L"BL1GOTYVR.dll");

    HMODULE mod = LoadLibraryW(modPath);
    if (!mod) {
        ProxyLog(L"BL1GOTYVR.dll could not be loaded; continuing with native DXGI");
        return TRUE;
    }

    using WaitForDisplayHooksFn = BOOL(WINAPI*)(DWORD);
    auto waitForDisplayHooks = reinterpret_cast<WaitForDisplayHooksFn>(
        GetProcAddress(mod, "BL1GOTYVR_WaitForDisplayHooks"));
    if (waitForDisplayHooks && !waitForDisplayHooks(5000))
        ProxyLog(L"Timed out waiting for display hooks; continuing startup");
    else
        ProxyLog(L"BL1GOTYVR.dll loaded automatically");
    return TRUE;
}

void EnsureModLoaded() {
    InitOnceExecuteOnce(&s_modLoadOnce, LoadModOnce, nullptr, nullptr);
}

} // namespace

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** factory) {
    EnsureModLoaded();
    using Function = HRESULT(WINAPI*)(REFIID, void**);
    return reinterpret_cast<Function>(g_real_CreateDXGIFactory)(riid, factory);
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** factory) {
    EnsureModLoaded();
    using Function = HRESULT(WINAPI*)(REFIID, void**);
    return reinterpret_cast<Function>(g_real_CreateDXGIFactory1)(riid, factory);
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    EnsureModLoaded();
    using Function = HRESULT(WINAPI*)(UINT, REFIID, void**);
    return reinterpret_cast<Function>(g_real_CreateDXGIFactory2)(flags, riid, factory);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        s_proxyModule = module;
        DisableThreadLibraryCalls(module);
        if (!ResolveRealDxgi()) {
            ProxyLog(L"Failed to resolve the real System32 dxgi.dll");
            return FALSE;
        }
    }
    return TRUE;
}
