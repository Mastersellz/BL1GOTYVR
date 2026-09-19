#include <Windows.h>
#include <dxgi.h>

#include <cstdio>
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
HMODULE s_modModule = nullptr;
INIT_ONCE s_modLoadOnce = INIT_ONCE_STATIC_INIT;

void ProxyLog(const wchar_t* message) {
    OutputDebugStringW(L"[BL1GOTYVR Proxy] ");
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");

    // Mirror to a file next to the proxy so chain decisions are diagnosable
    // without a debugger attached.
    wchar_t logPath[MAX_PATH] = {};
    if (s_proxyModule && GetModuleFileNameW(s_proxyModule, logPath, MAX_PATH)) {
        wchar_t* filename = wcsrchr(logPath, L'\\');
        if (filename &&
            swprintf_s(filename + 1,
                       MAX_PATH - static_cast<size_t>(filename + 1 - logPath),
                       L"BL1GOTYVR.proxy.log") >= 0) {
            FILE* file = nullptr;
            if (_wfopen_s(&file, logPath, L"a, ccs=UTF-8") == 0 && file) {
                SYSTEMTIME time = {};
                GetLocalTime(&time);
                fwprintf(file, L"[%02u:%02u:%02u.%03u] %s\n", time.wHour,
                         time.wMinute, time.wSecond, time.wMilliseconds, message);
                fclose(file);
            }
        }
    }
}

bool SyncGameResolution() {
    wchar_t modIni[MAX_PATH] = {};
    if (!GetModuleFileNameW(s_proxyModule, modIni, MAX_PATH)) return false;
    wchar_t* filename = wcsrchr(modIni, L'\\');
    if (!filename) return false;
    wcscpy_s(filename + 1, MAX_PATH - static_cast<size_t>(filename + 1 - modIni),
             L"BL1GOTYVR.ini");

    const UINT width = GetPrivateProfileIntW(L"Display", L"Width", 0, modIni);
    const UINT height = GetPrivateProfileIntW(L"Display", L"Height", 0, modIni);
    if (width < 640 || width > 7680 || height < 480 || height > 4320) return false;

    wchar_t widthText[16] = {};
    wchar_t heightText[16] = {};
    swprintf_s(widthText, L"%u", width);
    swprintf_s(heightText, L"%u", height);
    constexpr const wchar_t* environmentRoots[] = {
        L"USERPROFILE", L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"
    };
    constexpr const wchar_t* gameFolders[] = {
        L"Borderlands Game of the Year", L"Borderlands Game of the Year Enhanced"
    };
    bool updated = false;
    for (const wchar_t* environmentRoot : environmentRoots) {
        wchar_t root[MAX_PATH] = {};
        const DWORD rootLength = GetEnvironmentVariableW(environmentRoot, root, MAX_PATH);
        if (!rootLength || rootLength >= MAX_PATH) continue;
        for (const wchar_t* gameFolder : gameFolders) {
            wchar_t gameIni[MAX_PATH] = {};
            if (swprintf_s(gameIni, L"%s\\Documents\\My Games\\%s\\WillowGame\\Config\\WillowEngine.ini",
                           root, gameFolder) < 0 ||
                GetFileAttributesW(gameIni) == INVALID_FILE_ATTRIBUTES) continue;
            const bool widthWritten = WritePrivateProfileStringW(
                L"SystemSettings", L"ResX", widthText, gameIni) != FALSE;
            const bool heightWritten = WritePrivateProfileStringW(
                L"SystemSettings", L"ResY", heightText, gameIni) != FALSE;
            if (widthWritten && heightWritten) {
                WritePrivateProfileStringW(nullptr, nullptr, nullptr, gameIni);
                updated = true;
            }
        }
    }
    return updated;
}

struct ForwardExport {
    const char* name;
    FARPROC* slot;
};

// Every DXGI export this proxy forwards. The forward target is either the
// System32 dxgi.dll or, when present, a mods\ proxy (ReShade/Luma) so both
// loaders coexist: the game sees one dxgi.dll and the chain forwards in order.
const ForwardExport kForwardExports[] = {
    {"ApplyCompatResolutionQuirking", &g_real_ApplyCompatResolutionQuirking},
    {"CompatString", &g_real_CompatString},
    {"CompatValue", &g_real_CompatValue},
    {"DXGIDumpJournal", &g_real_DXGIDumpJournal},
    {"PIXBeginCapture", &g_real_PIXBeginCapture},
    {"PIXEndCapture", &g_real_PIXEndCapture},
    {"PIXGetCaptureState", &g_real_PIXGetCaptureState},
    {"SetAppCompatStringPointer", &g_real_SetAppCompatStringPointer},
    {"UpdateHMDEmulationStatus", &g_real_UpdateHMDEmulationStatus},
    {"CreateDXGIFactory", &g_real_CreateDXGIFactory},
    {"CreateDXGIFactory1", &g_real_CreateDXGIFactory1},
    {"CreateDXGIFactory2", &g_real_CreateDXGIFactory2},
    {"DXGID3D10CreateDevice", &g_real_DXGID3D10CreateDevice},
    {"DXGID3D10CreateLayeredDevice", &g_real_DXGID3D10CreateLayeredDevice},
    {"DXGID3D10GetLayeredDeviceSize", &g_real_DXGID3D10GetLayeredDeviceSize},
    {"DXGID3D10RegisterLayers", &g_real_DXGID3D10RegisterLayers},
    {"DXGIDeclareAdapterRemovalSupport", &g_real_DXGIDeclareAdapterRemovalSupport},
    {"DXGIDisableVBlankVirtualization", &g_real_DXGIDisableVBlankVirtualization},
    {"DXGIGetDebugInterface1", &g_real_DXGIGetDebugInterface1},
    {"DXGIReportAdapterConfiguration", &g_real_DXGIReportAdapterConfiguration},
};

HMODULE s_forwardModule = nullptr;
HMODULE s_systemModule = nullptr;

void ProxyLogJoined(const wchar_t* prefix, const wchar_t* path) {
    wchar_t buffer[MAX_PATH * 2] = {};
    if (swprintf_s(buffer, L"%s%s", prefix, path) >= 0) ProxyLog(buffer);
}

bool BuildProxyRelativePath(wchar_t (&path)[MAX_PATH], const wchar_t* relative) {
    if (!GetModuleFileNameW(s_proxyModule, path, MAX_PATH)) return false;
    wchar_t* filename = wcsrchr(path, L'\\');
    if (!filename) return false;
    return swprintf_s(filename + 1, MAX_PATH - static_cast<size_t>(filename + 1 - path),
                      relative) >= 0;
}

// Resolves the forward table, preferring `chainPath` per export and falling
// back to the System32 module for any export the chain does not provide.
// Chain proxies (ReShade/Luma) export only what they need, so requiring the
// full table used to unload them mid-startup. Only if a core factory export
// is missing is the chain dropped entirely.
bool ResolveForwardTable(const wchar_t* chainPath, const wchar_t* systemPath) {
    HMODULE system = LoadLibraryW(systemPath);
    if (!system || system == s_proxyModule) {
        if (system) FreeLibrary(system);
        return false;
    }

    HMODULE chain = nullptr;
    if (chainPath) {
        chain = LoadLibraryW(chainPath);
        if (chain == s_proxyModule) chain = nullptr;
        if (chain) {
            constexpr const char* kCoreExports[] = {
                "CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2"};
            for (const char* core : kCoreExports) {
                if (!GetProcAddress(chain, core)) {
                    ProxyLog(L"Chain proxy lacks a core factory export; dropping it");
                    FreeLibrary(chain);
                    chain = nullptr;
                    break;
                }
            }
        }
    }

    FARPROC resolved[_countof(kForwardExports)] = {};
    size_t chainProvided = 0;
    for (size_t index = 0; index < _countof(kForwardExports); ++index) {
        FARPROC proc = chain ? GetProcAddress(chain, kForwardExports[index].name)
                             : nullptr;
        if (proc) {
            ++chainProvided;
        } else {
            proc = GetProcAddress(system, kForwardExports[index].name);
        }
        if (!proc) {
            if (chain) FreeLibrary(chain);
            FreeLibrary(system);
            return false;
        }
        resolved[index] = proc;
    }
    for (size_t index = 0; index < _countof(kForwardExports); ++index)
        *kForwardExports[index].slot = resolved[index];

    s_forwardModule = chain ? chain : system;
    s_systemModule = system;
    if (chain) {
        wchar_t counts[96] = {};
        swprintf_s(counts, L"Chain proxy provided %zu of %zu exports",
                   chainProvided, _countof(kForwardExports));
        ProxyLog(counts);
    }
    return true;
}

bool TryChainModsProxy(const wchar_t* relative, const wchar_t* systemPath) {
    wchar_t path[MAX_PATH] = {};
    if (!BuildProxyRelativePath(path, relative)) return false;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    if (!ResolveForwardTable(path, systemPath)) {
        ProxyLogJoined(L"Chain candidate unusable, ignoring: ", path);
        return false;
    }
    ProxyLogJoined(L"Chained DXGI forwarding through ", path);
    return true;
}

bool ResolveRealDxgi() {
    // Optional chain: when a ReShade/Luma proxy sits in mods\, forward through
    // it so it initializes as if it were the root dxgi.dll. Controlled by
    // [Compat] ChainProxy in BL1GOTYVR.ini: auto (default) | kuma | reshade | none.
    wchar_t preference[32] = {};
    wchar_t iniPath[MAX_PATH] = {};
    if (BuildProxyRelativePath(iniPath, L"BL1GOTYVR.ini"))
        GetPrivateProfileStringW(L"Compat", L"ChainProxy", L"auto", preference,
                                 _countof(preference), iniPath);

    wchar_t systemDirectory[MAX_PATH] = {};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return false;
    wchar_t systemPath[MAX_PATH] = {};
    if (swprintf_s(systemPath, L"%s\\dxgi.dll", systemDirectory) < 0) return false;

    const bool disabled = _wcsicmp(preference, L"none") == 0;
    const bool preferReshade = _wcsicmp(preference, L"reshade") == 0;
    const bool preferKuma = _wcsicmp(preference, L"kuma") == 0;
    const bool automatic = !disabled && !preferReshade && !preferKuma;
    if (!disabled) {
        // Kuma/Luma first, then the ReShade layouts (reshade-shaders is the
        // folder ReShade finds its own ReShade.ini and presets in).
        const wchar_t* kumaPath = L"mods\\kuma\\dxgi.dll";
        const wchar_t* reshadeShadersPath = L"mods\\reshade-shaders\\dxgi.dll";
        const wchar_t* reshadePath = L"mods\\reshade\\dxgi.dll";
        if (preferReshade) {
            if (TryChainModsProxy(reshadeShadersPath, systemPath)) return true;
            if (TryChainModsProxy(reshadePath, systemPath)) return true;
        } else {
            if (TryChainModsProxy(kumaPath, systemPath)) return true;
            if (automatic && TryChainModsProxy(reshadeShadersPath, systemPath)) return true;
            if (automatic && TryChainModsProxy(reshadePath, systemPath)) return true;
        }
    }

    if (!ResolveForwardTable(nullptr, systemPath)) return false;
    ProxyLogJoined(L"Forwarding DXGI to ", systemPath);
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
    s_modModule = mod;

    using WaitForDisplayHooksFn = BOOL(WINAPI*)(DWORD);
    auto waitForDisplayHooks = reinterpret_cast<WaitForDisplayHooksFn>(
        GetProcAddress(mod, "BL1GOTYVR_WaitForDisplayHooks"));
    if (waitForDisplayHooks && !waitForDisplayHooks(5000))
        ProxyLog(L"Timed out waiting for display hooks; continuing startup");
    else
        ProxyLog(L"BL1GOTYVR.dll loaded automatically");
    return TRUE;
}

void NotifyModOfFactory(void* factory) {
    if (!factory || !s_modModule) return;
    using ObserveFactoryFn = BOOL(WINAPI*)(IUnknown*);
    auto observe = reinterpret_cast<ObserveFactoryFn>(
        GetProcAddress(s_modModule, "BL1GOTYVR_ObserveDxgiFactory"));
    if (!observe || !observe(static_cast<IUnknown*>(factory)))
        ProxyLog(L"Could not install the DXGI factory resolution guard");
}

void EnsureModLoaded() {
    InitOnceExecuteOnce(&s_modLoadOnce, LoadModOnce, nullptr, nullptr);
}

} // namespace

// Lets the loaded mod create its temporary probe swapchain through the SYSTEM
// dxgi instead of a chained ReShade/Luma proxy. Creating it through a chain
// proxy installs that proxy's Present wrapper in the probe's vtable, which
// made the mod hook the proxy wrapper (outermost) and capture frames before
// the post-processing pass. With the real present, the proxy wrapper stays
// outermost and the mod captures the already processed image.
extern "C" __declspec(dllexport) FARPROC WINAPI BL1GOTYVR_GetSystemDxgiProc(const char* name) {
    if (!name || !s_systemModule) return nullptr;
    return GetProcAddress(s_systemModule, name);
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** factory) {
    EnsureModLoaded();
    using Function = HRESULT(WINAPI*)(REFIID, void**);
    const HRESULT result = reinterpret_cast<Function>(g_real_CreateDXGIFactory)(riid, factory);
    if (SUCCEEDED(result) && factory) NotifyModOfFactory(*factory);
    return result;
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** factory) {
    EnsureModLoaded();
    using Function = HRESULT(WINAPI*)(REFIID, void**);
    const HRESULT result = reinterpret_cast<Function>(g_real_CreateDXGIFactory1)(riid, factory);
    if (SUCCEEDED(result) && factory) NotifyModOfFactory(*factory);
    return result;
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    EnsureModLoaded();
    using Function = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const HRESULT result = reinterpret_cast<Function>(g_real_CreateDXGIFactory2)(flags, riid, factory);
    if (SUCCEEDED(result) && factory) NotifyModOfFactory(*factory);
    return result;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        s_proxyModule = module;
        DisableThreadLibraryCalls(module);
        if (SyncGameResolution())
            ProxyLog(L"Synchronized the game resolution with BL1GOTYVR.ini");
        else
            ProxyLog(L"Could not synchronize the game resolution");
        if (!ResolveRealDxgi()) {
            ProxyLog(L"Failed to resolve the real System32 dxgi.dll");
            return FALSE;
        }
    }
    return TRUE;
}
