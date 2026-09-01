#include "OpenXRContext.hpp"
#include "../core/VRMod.hpp"
#include "../config/Config.hpp"
#include "../input/XRInput.hpp"
#include "../render/HudBlitter.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <d3d11_4.h>

namespace bl1gotyvr { namespace xr {

static bool ReadSelectedRuntime(wchar_t path[1024], const char*& source) {
    path[0] = L'\0';
    source = "none";
    if (GetEnvironmentVariableW(L"XR_RUNTIME_JSON", path, 1024) > 0) {
        source = "XR_RUNTIME_JSON";
        return true;
    }
    DWORD bytes = 1024 * sizeof(wchar_t);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ,
        nullptr, path, &bytes);
    if (status == ERROR_SUCCESS && path[0]) {
        source = "ActiveRuntime";
        return true;
    }
    return false;
}

bool IsSteamRuntimeSelected() {
    wchar_t path[1024] = {};
    const char* source = nullptr;
    if (!ReadSelectedRuntime(path, source)) return false;
    for (wchar_t* current = path; *current; ++current)
        *current = static_cast<wchar_t>(towlower(*current));
    return wcsstr(path, L"steamvr") != nullptr ||
        wcsstr(path, L"steamxr") != nullptr;
}

static void LogSelectedRuntime() {
    wchar_t path[1024] = {};
    const char* source = nullptr;
    if (!ReadSelectedRuntime(path, source)) {
        Log("[OpenXR] No runtime selection found in XR_RUNTIME_JSON or ActiveRuntime");
        return;
    }
    Log("[OpenXR] Selected runtime (%s): %ls [SteamVR=%d]", source, path,
        IsSteamRuntimeSelected() ? 1 : 0);
}

static XrFovf CenterVerticalFov(const XrFovf& source) {
    XrFovf centered = source;
    const float halfSpan = (tanf(source.angleUp) - tanf(source.angleDown)) * 0.5f;
    if (std::isfinite(halfSpan) && halfSpan > 0.0f) {
        const float angle = atanf(halfSpan);
        centered.angleUp = angle;
        centered.angleDown = -angle;
    }
    return centered;
}

OpenXRContext& OpenXRContext::Instance() {
    static OpenXRContext ctx;
    return ctx;
}

bool OpenXRContext::GetProjectionCrop(int eye, float sourceAspect, float& scaleX, float& scaleY,
                                       float& offsetX, float& offsetY,
                                       float& horizontalFovDegrees) const {
    if (!m_viewsValid || eye < 0 || eye > 1 || sourceAspect <= 0.0f) return false;
    return GetProjectionCrop(m_views[eye], sourceAspect, scaleX, scaleY, offsetX, offsetY,
                             horizontalFovDegrees);
}

bool OpenXRContext::GetProjectionCrop(const XrView& view, float sourceAspect,
                                       float& scaleX, float& scaleY,
                                       float& offsetX, float& offsetY,
                                       float& horizontalFovDegrees) const {
    if (sourceAspect <= 0.0f) return false;
    const XrFovf fov = view.fov;
    const float tanLeft = tanf(fov.angleLeft);
    const float tanRight = tanf(fov.angleRight);
    const float tanUp = tanf(fov.angleUp);
    const float tanDown = tanf(fov.angleDown);
    // Prefer the game's measured source image half-tangents. Fall back to the
    // eye-derived estimate only until the render path reports the real ones.
    float sourceHalfY = 0.0f, sourceHalfX = 0.0f;
    if (!GetSourceProjectionTans(sourceHalfX, sourceHalfY)) {
        sourceHalfY = (std::max)(fabsf(tanUp), fabsf(tanDown));
        sourceHalfX = sourceHalfY * sourceAspect;
    }
    // A symmetric UE3 source projection must cover the wider side of each
    // asymmetric OpenXR eye. Otherwise negative UVs clamp to a shimmering edge.
    sourceHalfX = (std::max)(sourceHalfX,
        (std::max)(fabsf(tanLeft), fabsf(tanRight)));
    if (sourceHalfX <= 0.0f || sourceHalfY <= 0.0f) return false;
    scaleX = (tanRight - tanLeft) / (2.0f * sourceHalfX);
    scaleY = (tanUp - tanDown) / (2.0f * sourceHalfY);
    offsetX = (tanLeft + sourceHalfX) / (2.0f * sourceHalfX);
    // Keep the game horizon centered. Quest's asymmetric vertical FOV would
    // otherwise crop twice as much from the top and make the camera appear to
    // point at the floor when the source projection is square.
    offsetY = (1.0f - scaleY) * 0.5f;
    horizontalFovDegrees = 2.0f * atanf(sourceHalfX) * 57.29577951308232f;
    constexpr float kUvTolerance = 1.0e-4f;
    return scaleX > 0.0f && scaleX <= 1.0f && scaleY > 0.0f && scaleY <= 1.0f &&
        offsetX >= -kUvTolerance && offsetY >= -kUvTolerance &&
        offsetX + scaleX <= 1.0f + kUvTolerance &&
        offsetY + scaleY <= 1.0f + kUvTolerance;
}

void OpenXRContext::SetSourceProjectionTans(float halfTanX, float halfTanY) {
    if (!std::isfinite(halfTanX) || !std::isfinite(halfTanY) ||
        halfTanX <= 0.0f || halfTanY <= 0.0f) return;
    static std::atomic<bool> loggedSourceTans{false};
    if (!loggedSourceTans.exchange(true)) {
        Log("[OpenXR] Game source projection half-tans measured: "
            "halfX=%.4f halfY=%.4f (FOV=%.1f)",
            halfTanX, halfTanY,
            2.0f * atanf(halfTanX) * 57.29577951308232f);
    }
    m_sourceHalfTanX.store(halfTanX, std::memory_order_relaxed);
    m_sourceHalfTanY.store(halfTanY, std::memory_order_relaxed);
}

bool OpenXRContext::GetSourceProjectionTans(float& halfTanX, float& halfTanY) const {
    halfTanX = m_sourceHalfTanX.load(std::memory_order_relaxed);
    halfTanY = m_sourceHalfTanY.load(std::memory_order_relaxed);
    return halfTanX > 0.0f && halfTanY > 0.0f;
}

void OpenXRContext::MarkEyeRendered(int eye) {
    if (eye < 0 || eye > 1 || !m_viewsValid) return;
    AcquireSRWLockExclusive(&m_poseLock);
    m_renderedViews[eye] = m_views[eye];
    m_renderedViewValid[eye] = true;
    ReleaseSRWLockExclusive(&m_poseLock);
}

bool OpenXRContext::GetPoseSnapshot(float headPosition[3], float headRotation[4],
                                    XrView views[2]) const {
    AcquireSRWLockShared(&m_poseLock);
    const bool valid = m_poseValid && m_viewsValid.load();
    if (valid) {
        memcpy(headPosition, m_headPosition, sizeof(m_headPosition));
        memcpy(headRotation, m_headRotation, sizeof(m_headRotation));
        views[0] = m_views[0];
        views[1] = m_views[1];
    }
    ReleaseSRWLockShared(&m_poseLock);
    return valid;
}

XrResult OpenXRContext::CheckResult(XrResult result, const char* call) {
    if (result != XR_SUCCESS) {
        Log("[OpenXR] %s failed: %d", call, (int)result);
    }
    return result;
}

void OpenXRContext::RequestRecovery(const char* call, XrResult result) {
    bool expected = false;
    if (m_recoveryRequested.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        Log("[OpenXR] Recovery requested by %s: %d", call ? call : "unknown",
            static_cast<int>(result));
    }
}

bool OpenXRContext::ObserveFrameResult(const char* call, XrResult result) {
    if (result == XR_SESSION_LOSS_PENDING) {
        RequestRecovery(call, result);
        return true;
    }
    if (XR_FAILED(result)) {
        Log("[OpenXR] %s failed: %d", call, static_cast<int>(result));
        RequestRecovery(call, result);
        return false;
    }
    return true;
}

bool OpenXRContext::WaitForSwapchainImage(XrSwapchain swapchain, const char* label) {
    constexpr XrDuration kWaitSlice = 50LL * 1000LL * 1000LL;
    constexpr int kMaxWaitSlices = 40;
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = kWaitSlice;
    for (int attempt = 0; attempt < kMaxWaitSlices; ++attempt) {
        const XrResult result = xrWaitSwapchainImage(swapchain, &waitInfo);
        if (result == XR_TIMEOUT_EXPIRED) {
            PollEvents();
            if (NeedsRecovery()) return false;
            continue;
        }
        return ObserveFrameResult(label, result);
    }
    Log("[OpenXR] %s timed out for 2 seconds", label);
    RequestRecovery(label, XR_TIMEOUT_EXPIRED);
    return false;
}

void OpenXRContext::RequestSessionExit() {
    if (m_session == XR_NULL_HANDLE || !m_deviceBound.load(std::memory_order_acquire)) return;
    const XrResult result = xrRequestExitSession(m_session);
    if (XR_FAILED(result) && result != XR_ERROR_SESSION_NOT_RUNNING) {
        Log("[OpenXR] xrRequestExitSession failed: %d", static_cast<int>(result));
    }
}

bool OpenXRContext::Initialize(ID3D11Device* device, DXGI_FORMAT backbufferFormat) {
    if (m_initialized.load(std::memory_order_acquire)) return true;

    m_recoveryRequested.store(false, std::memory_order_release);
    m_sessionState.store(XR_SESSION_STATE_UNKNOWN, std::memory_order_release);
    m_deviceBound.store(false, std::memory_order_release);
    m_endSessionPending.store(false, std::memory_order_release);

    Log("[OpenXR] Initializing...");

    m_device = device;
    m_device->AddRef();
    m_device->GetImmediateContext(&m_context);
    m_swapchainFormat = backbufferFormat;
    m_nearPlane = config::Get().near_plane;
    m_farPlane = config::Get().far_plane;

    if (!CreateInstance()) { Shutdown(); return false; }
    if (!CreateSession()) { Shutdown(); return false; }
    if (!CreateSpaces()) { Shutdown(); return false; }
    if (!CreateSwapchains()) { Shutdown(); return false; }

    // Initialize XR input system
    if (!input::XRInput::Instance().Initialize(
            m_instance, m_session, m_stageSpace, m_touchControllerPlusSupported)) {
        Log("[OpenXR] XR input initialization failed");
        m_initialized.store(true, std::memory_order_release);
        Shutdown();
        return false;
    }

    m_initialized.store(true, std::memory_order_release);
    Log("[OpenXR] Initialized successfully (IPD=%.1fmm)", m_ipd * 1000.0f);
    return true;
}

void OpenXRContext::Shutdown() {
    if (!m_initialized.load(std::memory_order_acquire) &&
        m_instance == XR_NULL_HANDLE && m_session == XR_NULL_HANDLE &&
        !m_device && !m_context) return;
    Log("[OpenXR] Shutting down...");

    // Shutdown XR input
    input::XRInput::Instance().Shutdown();

    // Destroy swapchains
    InvalidateHudResources();
    AcquireSRWLockExclusive(&m_reticleLock);
    DestroyReticleSwapchain();
    ReleaseSRWLockExclusive(&m_reticleLock);
    if (m_leftEye.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_leftEye.swapchain);
        m_leftEye.swapchain = XR_NULL_HANDLE;
    }
    if (m_rightEye.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_rightEye.swapchain);
        m_rightEye.swapchain = XR_NULL_HANDLE;
    }
    m_leftEye.images.clear();
    m_rightEye.images.clear();

    // Destroy spaces
    if (m_stageSpace != XR_NULL_HANDLE) { xrDestroySpace(m_stageSpace); m_stageSpace = XR_NULL_HANDLE; }
    if (m_viewSpace != XR_NULL_HANDLE) { xrDestroySpace(m_viewSpace); m_viewSpace = XR_NULL_HANDLE; }

    // xrEndSession is only legal after the runtime reports STOPPING. Destroying
    // a session is sufficient for recovery when that transition never arrives.
    if (m_session != XR_NULL_HANDLE) {
        const XrSessionState state = m_sessionState.load(std::memory_order_acquire);
        if (state == XR_SESSION_STATE_STOPPING &&
            m_endSessionPending.exchange(false, std::memory_order_acq_rel)) {
            const XrResult endResult = xrEndSession(m_session);
            if (XR_FAILED(endResult))
                Log("[OpenXR] xrEndSession during teardown failed: %d",
                    static_cast<int>(endResult));
        }
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
    }

    // Destroy instance
    if (m_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(m_instance);
        m_instance = XR_NULL_HANDLE;
    }

    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }

    AcquireSRWLockExclusive(&m_poseLock);
    m_frameActive = false;
    m_predictedDisplayTime.store(0, std::memory_order_release);
    m_viewsValid = false;
    m_poseValid = false;
    m_renderedViewValid[0] = m_renderedViewValid[1] = false;
    memset(m_views, 0, sizeof(m_views));
    memset(m_renderedViews, 0, sizeof(m_renderedViews));
    ReleaseSRWLockExclusive(&m_poseLock);

    m_initialized.store(false, std::memory_order_release);
    m_deviceBound.store(false, std::memory_order_release);
    m_endSessionPending.store(false, std::memory_order_release);
    m_sessionState.store(XR_SESSION_STATE_UNKNOWN, std::memory_order_release);
    m_recoveryRequested.store(false, std::memory_order_release);
    m_systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
    m_runtimeName[0] = '\0';
    m_isSteamRuntime = false;
    m_isVdxr = false;
    m_integratedHud = false;
    m_refreshRateSupported = false;
    m_touchControllerPlusSupported = false;
    m_theaterAnchored = false;
    m_theaterPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    Log("[OpenXR] Shutdown complete");
}

bool OpenXRContext::CreateInstance() {
    Log("[OpenXR] Creating instance...");

    // Respect the loader's standard environment/registry selection. Runtime
    // classification is diagnostic and must never silently replace it.
    m_isSteamRuntime = IsSteamRuntimeSelected();
    LogSelectedRuntime();

    // Enumerate extensions
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount);
    for (auto& e : exts) e.type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasD3D11 = false;
    bool hasRefreshRate = false;
    bool hasTouchControllerPlus = false;
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) {
            hasD3D11 = true;
        }
        if (strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0)
            hasRefreshRate = true;
        if (strcmp(e.extensionName, XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME) == 0)
            hasTouchControllerPlus = true;
    }
    if (!hasD3D11) {
        Log("[OpenXR] ERROR: XR_KHR_D3D11_ENABLE not supported");
        return false;
    }

    std::vector<const char*> enabledExts = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    if (hasRefreshRate) enabledExts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (hasTouchControllerPlus)
        enabledExts.push_back(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
    m_refreshRateSupported = hasRefreshRate;
    m_touchControllerPlusSupported = hasTouchControllerPlus;

    XrInstanceCreateInfo ci = {};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
    ci.enabledExtensionNames = enabledExts.data();

    XrApplicationInfo& appInfo = ci.applicationInfo;
    strcpy(appInfo.applicationName, "BL1GOTYVR");
    strcpy(appInfo.engineName, "Unreal Engine 3");
    appInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    XrResult r = xrCreateInstance(&ci, &m_instance);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] ERROR: xrCreateInstance = %d", (int)r);
        return false;
    }
    XrInstanceProperties properties = {XR_TYPE_INSTANCE_PROPERTIES};
    r = xrGetInstanceProperties(m_instance, &properties);
    if (XR_SUCCEEDED(r)) {
        strcpy_s(m_runtimeName, properties.runtimeName);
        char normalized[XR_MAX_RUNTIME_NAME_SIZE] = {};
        strcpy_s(normalized, properties.runtimeName);
        for (char* current = normalized; *current; ++current)
            *current = static_cast<char>(std::tolower(static_cast<unsigned char>(*current)));
        m_isVdxr = strstr(normalized, "vdxr") != nullptr ||
            strstr(normalized, "virtual desktop") != nullptr ||
            strstr(normalized, "virtualdesktop") != nullptr;
        m_isSteamRuntime = m_isSteamRuntime || strstr(normalized, "steamvr") != nullptr ||
            strstr(normalized, "steamxr") != nullptr;
        const bool metaCompatibility =
            strstr(normalized, "meta compatibility mode") != nullptr;
        m_integratedHud = m_isVdxr || metaCompatibility;
        Log("[OpenXR] Instance created: runtime='%s' version=%u.%u.%u "
            "steamvr=%d vdxr=%d integratedHud=%d API=1.0",
            properties.runtimeName,
            static_cast<unsigned>(XR_VERSION_MAJOR(properties.runtimeVersion)),
            static_cast<unsigned>(XR_VERSION_MINOR(properties.runtimeVersion)),
            static_cast<unsigned>(XR_VERSION_PATCH(properties.runtimeVersion)),
            m_isSteamRuntime ? 1 : 0, m_isVdxr ? 1 : 0, m_integratedHud ? 1 : 0);
    } else {
        Log("[OpenXR] Instance created; xrGetInstanceProperties=%d", (int)r);
    }
    return true;
}

bool OpenXRContext::CreateSession() {
    Log("[OpenXR] Creating session...");

    // Get system
    XrSystemGetInfo sysInfo = {};
    sysInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrResult r = xrGetSystem(m_instance, &sysInfo, &m_systemId);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] ERROR: xrGetSystem = %d (no HMD?)", (int)r);
        return false;
    }
    m_systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
    r = xrGetSystemProperties(m_instance, m_systemId, &m_systemProperties);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] WARNING: xrGetSystemProperties = %d; HUD layers disabled", (int)r);
        m_systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
    } else {
        Log("[OpenXR] Runtime limits: maxLayers=%u maxSwapchain=%ux%u",
            m_systemProperties.graphicsProperties.maxLayerCount,
            m_systemProperties.graphicsProperties.maxSwapchainImageWidth,
            m_systemProperties.graphicsProperties.maxSwapchainImageHeight);
    }

    // Get D3D11 graphics requirements
    PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(m_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&getReqs);
    if (!getReqs) {
        Log("[OpenXR] ERROR: xrGetD3D11GraphicsRequirementsKHR unavailable");
        return false;
    }
    XrGraphicsRequirementsD3D11KHR reqs = {
        XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    r = getReqs(m_instance, m_systemId, &reqs);
    if (XR_FAILED(r)) {
        Log("[OpenXR] ERROR: xrGetD3D11GraphicsRequirementsKHR = %d", (int)r);
        return false;
    }
    const UINT creationFlags = m_device->GetCreationFlags();
    const D3D_FEATURE_LEVEL featureLevel = m_device->GetFeatureLevel();
    Log("[OpenXR] Game D3D11 device: flags=0x%X singleThreaded=%d feature=0x%X "
        "required=0x%X", creationFlags,
        (creationFlags & D3D11_CREATE_DEVICE_SINGLETHREADED) ? 1 : 0,
        static_cast<unsigned>(featureLevel), static_cast<unsigned>(reqs.minFeatureLevel));
    if (featureLevel < reqs.minFeatureLevel) {
        Log("[OpenXR] ERROR: game D3D11 feature level is below runtime requirement");
        return false;
    }

    if (m_isSteamRuntime) {
        ID3D11Multithread* multithread = nullptr;
        const HRESULT multithreadResult = m_context->QueryInterface(
            __uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread));
        if (FAILED(multithreadResult) || !multithread) {
            Log("[SteamVR] ERROR: ID3D11Multithread unavailable: 0x%08X",
                multithreadResult);
            return false;
        }
        const BOOL wasProtected = multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
        Log("[SteamVR] D3D11 immediate-context protection enabled (previous=%d)",
            wasProtected ? 1 : 0);
    }

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC adapterDesc = {};
    const bool adapterKnown = SUCCEEDED(m_device->QueryInterface(
        __uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter &&
        SUCCEEDED(adapter->GetDesc(&adapterDesc));
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
    if (adapterKnown) {
        const bool luidMatches = adapterDesc.AdapterLuid.HighPart == reqs.adapterLuid.HighPart &&
            adapterDesc.AdapterLuid.LowPart == reqs.adapterLuid.LowPart;
        Log("[OpenXR] Game adapter='%ls' LUID=%08X:%08X runtime=%08X:%08X match=%d",
            adapterDesc.Description, static_cast<unsigned>(adapterDesc.AdapterLuid.HighPart),
            adapterDesc.AdapterLuid.LowPart,
            static_cast<unsigned>(reqs.adapterLuid.HighPart), reqs.adapterLuid.LowPart,
            luidMatches ? 1 : 0);
        if (!luidMatches) {
            Log("[OpenXR] ERROR: game and OpenXR runtime selected different GPUs");
            return false;
        }
    }

    // Create session with D3D11 binding
    XrGraphicsBindingD3D11KHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
    binding.device = m_device;

    XrSessionCreateInfo sessionCI = {};
    sessionCI.type = XR_TYPE_SESSION_CREATE_INFO;
    sessionCI.next = &binding;
    sessionCI.systemId = m_systemId;

    r = xrCreateSession(m_instance, &sessionCI, &m_session);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] ERROR: xrCreateSession = %d", (int)r);
        return false;
    }

    // The graphics device is attached, but the session is not running until
    // the runtime reports READY and xrBeginSession succeeds.
    m_deviceBound.store(false, std::memory_order_release);
    ConfigureRefreshRate();
    if (NeedsRecovery()) return false;
    Log("[OpenXR] Session created");
    return true;
}

void OpenXRContext::ConfigureRefreshRate() {
    if (!m_refreshRateSupported || m_session == XR_NULL_HANDLE) {
        Log("[OpenXR] XR_FB_display_refresh_rate unavailable; runtime controls pacing");
        return;
    }
    PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates = nullptr;
    PFN_xrGetDisplayRefreshRateFB getRate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
    xrGetInstanceProcAddr(m_instance, "xrEnumerateDisplayRefreshRatesFB",
        reinterpret_cast<PFN_xrVoidFunction*>(&enumerateRates));
    xrGetInstanceProcAddr(m_instance, "xrGetDisplayRefreshRateFB",
        reinterpret_cast<PFN_xrVoidFunction*>(&getRate));
    xrGetInstanceProcAddr(m_instance, "xrRequestDisplayRefreshRateFB",
        reinterpret_cast<PFN_xrVoidFunction*>(&requestRate));
    if (!enumerateRates || !getRate || !requestRate) {
        Log("[OpenXR] Display refresh extension functions unavailable");
        return;
    }
    uint32_t rateCount = 0;
    XrResult result = enumerateRates(m_session, 0, &rateCount, nullptr);
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrEnumerateDisplayRefreshRatesFB", result);
    if (result != XR_SUCCESS || !rateCount) {
        Log("[OpenXR] xrEnumerateDisplayRefreshRatesFB failed: %d",
            static_cast<int>(result));
        return;
    }
    std::vector<float> rates(rateCount);
    result = enumerateRates(m_session, rateCount, &rateCount, rates.data());
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrEnumerateDisplayRefreshRatesFB", result);
    if (result != XR_SUCCESS) return;
    float currentRate = 0.0f;
    result = getRate(m_session, &currentRate);
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrGetDisplayRefreshRateFB", result);
    if (result != XR_SUCCESS) return;
    char supported[256] = {};
    size_t used = 0;
    for (float rate : rates) {
        const int written = snprintf(supported + used, sizeof(supported) - used,
            used ? ", %.0f" : "%.0f", rate);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(supported) - used) break;
        used += static_cast<size_t>(written);
    }
    const float configured = config::Get().openxr_refresh_rate_hz;
    Log("[OpenXR] Display refresh: current=%.1fHz supported=[%s] requested=%.1fHz",
        currentRate, supported, configured);
    if (configured <= 0.0f) return;
    const auto nearest = std::min_element(rates.begin(), rates.end(),
        [configured](float left, float right) {
            return fabsf(left - configured) < fabsf(right - configured);
        });
    if (nearest == rates.end()) return;
    result = requestRate(m_session, *nearest);
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrRequestDisplayRefreshRateFB", result);
    if (result != XR_SUCCESS) {
        Log("[OpenXR] xrRequestDisplayRefreshRateFB(%.1f) failed: %d", *nearest,
            static_cast<int>(result));
    } else {
        Log("[OpenXR] Requested display refresh rate %.1fHz", *nearest);
    }
}

bool OpenXRContext::CreateSpaces() {
    Log("[OpenXR] Creating reference spaces...");

    // LOCAL is the portable application space used by BFVR on SteamVR and
    // VDXR. Keep the existing member name to avoid touching camera call sites.
    XrReferenceSpaceCreateInfo spaceCI = {};
    spaceCI.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    spaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceCI.poseInReferenceSpace = {};
    spaceCI.poseInReferenceSpace.orientation.w = 1.0f;

    XrResult r = xrCreateReferenceSpace(m_session, &spaceCI, &m_stageSpace);
    if (r == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrCreateReferenceSpace(LOCAL)", r);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] ERROR: xrCreateReferenceSpace(LOCAL) = %d", (int)r);
        return false;
    }

    // View space
    spaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    r = xrCreateReferenceSpace(m_session, &spaceCI, &m_viewSpace);
    if (r == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrCreateReferenceSpace(VIEW)", r);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] WARNING: xrCreateReferenceSpace(VIEW) = %d", (int)r);
        if (r == XR_SESSION_LOSS_PENDING) return false;
    }

    Log("[OpenXR] Reference spaces created");
    return true;
}

// Format equivalence groups for cross-runtime compatibility (SteamVR, VDXR, WMR).
// BGRA and RGBA are visually identical but have different DXGI enum values.
bool AreFormatsCompatible(DXGI_FORMAT a, DXGI_FORMAT b) {
    if (a == b) return true;
    // BGRA ↔ RGBA equivalence
    if ((a == DXGI_FORMAT_B8G8R8A8_UNORM && b == DXGI_FORMAT_R8G8B8A8_UNORM) ||
        (a == DXGI_FORMAT_R8G8B8A8_UNORM && b == DXGI_FORMAT_B8G8R8A8_UNORM) ||
        (a == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && b == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ||
        (a == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && b == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ||
        (a == DXGI_FORMAT_B8G8R8A8_UNORM && b == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ||
        (a == DXGI_FORMAT_R8G8B8A8_UNORM && b == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ||
        (a == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && b == DXGI_FORMAT_R8G8B8A8_UNORM) ||
        (a == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && b == DXGI_FORMAT_B8G8R8A8_UNORM))
        return true;
    // SRGB ↔ UNORM equivalence (same channel order)
    if ((a == DXGI_FORMAT_R8G8B8A8_UNORM && b == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ||
        (a == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && b == DXGI_FORMAT_R8G8B8A8_UNORM) ||
        (a == DXGI_FORMAT_B8G8R8A8_UNORM && b == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ||
        (a == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && b == DXGI_FORMAT_B8G8R8A8_UNORM))
        return true;
    return false;
}

static bool IsSrgbFormat(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

static uint32_t FindSupportedFormat(ID3D11Device* device, const int64_t* supported,
                                     uint32_t count, DXGI_FORMAT desired) {
    // Pass 1: exact match
    for (uint32_t i = 0; i < count; i++) {
        if (supported[i] == (int64_t)desired) return i;
    }
    // BFVR's tested SteamVR/VDXR order. The game backbuffer contains
    // display-encoded color, so prefer an sRGB composition format.
    constexpr DXGI_FORMAT preferred[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM
    };
    for (DXGI_FORMAT candidate : preferred) {
        for (uint32_t i = 0; i < count; ++i) {
            if (supported[i] == static_cast<int64_t>(candidate)) return i;
        }
    }
    // FLOAT fallback when no standard 8-bit color format is available.
    for (uint32_t i = 0; i < count; i++) {
        if (supported[i] == (int64_t)DXGI_FORMAT_R16G16B16A16_FLOAT) return i;
    }
    // Last resort: any compatible family, then the runtime's first format.
    for (uint32_t i = 0; i < count; i++) {
        if (AreFormatsCompatible(static_cast<DXGI_FORMAT>(supported[i]), desired)) return i;
    }
    return 0;
}

bool OpenXRContext::CreateSwapchains() {
    Log("[OpenXR] Creating eye swapchains...");

    // Get recommended swapchain sizes
    uint32_t viewConfigCount = 0;
    XrResult result = xrEnumerateViewConfigurations(
        m_instance, m_systemId, 0, &viewConfigCount, nullptr);
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrEnumerateViewConfigurations", result);
    if (result != XR_SUCCESS || !viewConfigCount) return false;
    std::vector<XrViewConfigurationType> viewTypes(viewConfigCount);
    result = xrEnumerateViewConfigurations(m_instance, m_systemId, viewConfigCount,
        &viewConfigCount, viewTypes.data());
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrEnumerateViewConfigurations", result);
    if (result != XR_SUCCESS || std::find(viewTypes.begin(), viewTypes.end(),
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) == viewTypes.end()) {
        Log("[OpenXR] ERROR: PRIMARY_STEREO view configuration unavailable");
        return false;
    }

    // Get view config views for PRIMARY_STEREO
    uint32_t viewCount = 0;
    result = xrEnumerateViewConfigurationViews(m_instance, m_systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrEnumerateViewConfigurationViews", result);
    if (result != XR_SUCCESS || viewCount < 2) return false;
    std::vector<XrViewConfigurationView> viewConfigs(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    result = xrEnumerateViewConfigurationViews(m_instance, m_systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount,
        viewConfigs.data());

    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrEnumerateViewConfigurationViews", result);
    if (result != XR_SUCCESS || viewCount < 2) {
        Log("[OpenXR] ERROR: Expected 2 views, got %u", viewCount);
        return false;
    }

    // Enumerate supported swapchain formats
    uint32_t formatCount = 0;
    result = xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrEnumerateSwapchainFormats", result);
    if (result != XR_SUCCESS || !formatCount) {
        Log("[OpenXR] ERROR: no swapchain formats: %d", static_cast<int>(result));
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    result = xrEnumerateSwapchainFormats(
        m_session, formatCount, &formatCount, formats.data());
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrEnumerateSwapchainFormats", result);
    if (result != XR_SUCCESS || !formatCount) return false;

    const DXGI_FORMAT requestedFormat = m_swapchainFormat;
    // Log all supported formats for runtime debugging (VDXR vs SteamVR)
    for (uint32_t i = 0; i < formatCount && i < 16; i++) {
        const DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(formats[i]);
        Log("[OpenXR] Supported swapchain format[%u]: %u (compatible=%d)",
            i, (uint32_t)fmt, AreFormatsCompatible(fmt, requestedFormat));
    }
    uint32_t formatIdx = FindSupportedFormat(m_device, formats.data(), formatCount, requestedFormat);
    int64_t selectedFormat = formats[formatIdx];
    m_swapchainFormat = static_cast<DXGI_FORMAT>(selectedFormat);
    Log("[OpenXR] Swapchain format: %u (requested %u, compatible=%d, total=%u)",
        (uint32_t)selectedFormat, (uint32_t)requestedFormat,
        AreFormatsCompatible(m_swapchainFormat, requestedFormat), formatCount);

    // Create per-eye swapchains
    EyeData* eyes[2] = { &m_leftEye, &m_rightEye };
    for (int i = 0; i < 2; i++) {
        XrSwapchainCreateInfo sci = {};
        sci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        sci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                         XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                         XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = selectedFormat;
        sci.sampleCount = 1;
        const float resolutionScale = config::Get().resolution_scale;
        sci.width = static_cast<uint32_t>(std::clamp(
            viewConfigs[i].recommendedImageRectWidth * resolutionScale,
            256.0f, static_cast<float>(viewConfigs[i].maxImageRectWidth)));
        sci.height = static_cast<uint32_t>(std::clamp(
            viewConfigs[i].recommendedImageRectHeight * resolutionScale,
            256.0f, static_cast<float>(viewConfigs[i].maxImageRectHeight)));
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;

        XrResult r = xrCreateSwapchain(m_session, &sci, &eyes[i]->swapchain);
        if (r == XR_SESSION_LOSS_PENDING)
            RequestRecovery("xrCreateSwapchain(eye)", r);
        if (r != XR_SUCCESS) {
            Log("[OpenXR] ERROR: xrCreateSwapchain(eye %d) = %d", i, (int)r);
            return false;
        }

        eyes[i]->width = sci.width;
        eyes[i]->height = sci.height;
        eyes[i]->submittedWidth = sci.width;
        eyes[i]->submittedHeight = sci.height;

        // Get swapchain images
        uint32_t imgCount = 0;
        r = xrEnumerateSwapchainImages(eyes[i]->swapchain, 0, &imgCount, nullptr);
        if (r == XR_SESSION_LOSS_PENDING)
            RequestRecovery("xrEnumerateSwapchainImages(eye)", r);
        if (r != XR_SUCCESS || !imgCount) {
            Log("[OpenXR] ERROR: xrEnumerateSwapchainImages count eye %d = %d", i,
                static_cast<int>(r));
            return false;
        }
        eyes[i]->images.resize(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        r = xrEnumerateSwapchainImages(eyes[i]->swapchain, imgCount, &imgCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(eyes[i]->images.data()));
        if (r == XR_SESSION_LOSS_PENDING)
            RequestRecovery("xrEnumerateSwapchainImages(eye)", r);
        if (r != XR_SUCCESS) return false;

        Log("[OpenXR] Eye %d: %ux%u, %u images", i, sci.width, sci.height, imgCount);

        // Log actual swapchain image texture desc for runtime debugging
        if (imgCount > 0 && eyes[i]->images[0].texture) {
            D3D11_TEXTURE2D_DESC texDesc = {};
            eyes[i]->images[0].texture->GetDesc(&texDesc);
            Log("[OpenXR] Eye %d swapchain image: fmt=%u %ux%u samples=%u bind=0x%X usage=%u",
                i, texDesc.Format, texDesc.Width, texDesc.Height,
                texDesc.SampleDesc.Count, texDesc.BindFlags, texDesc.Usage);
        }
    }

    // Compute IPD from eye poses
    // (will be updated each frame from LocateViews)
    Log("[OpenXR] Swapchains created");
    return true;
}

bool OpenXRContext::CanSubmitHud() const {
    const DXGI_FORMAT format = m_swapchainFormat;
    const bool alphaFormat = format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    return m_initialized.load(std::memory_order_acquire) && !NeedsRecovery() &&
        config::Get().hud_enabled && alphaFormat &&
        m_viewSpace != XR_NULL_HANDLE &&
        m_systemProperties.graphicsProperties.maxLayerCount >= 2;
}

bool OpenXRContext::WantsHudCapture() const {
    return m_initialized.load(std::memory_order_acquire) && !NeedsRecovery() &&
        config::Get().hud_enabled &&
        !config::Get().debug_force_no_hud_layer;
}

bool OpenXRContext::ShouldSeparateHud() const {
    // VDXR showed the HUD quad while the pre-HUD projection snapshots were
    // black. Submit the complete final eye images there; SteamVR keeps the
    // dedicated VIEW-space HUD layer.
    return CanSubmitHud() && !m_integratedHud &&
        !config::Get().debug_force_no_hud_layer;
}

bool OpenXRContext::ShouldBakeHud() const {
    return WantsHudCapture() && (m_integratedHud || !CanSubmitHud()) &&
        !config::Get().debug_force_no_hud_layer;
}

void OpenXRContext::DestroyHudSwapchain() {
    if (m_hud.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_hud.swapchain);
        m_hud.swapchain = XR_NULL_HANDLE;
    }
    m_hud.images.clear();
    m_hud.width = m_hud.height = 0;
    m_hud.submittedWidth = m_hud.submittedHeight = 0;
    m_hudPrepared = false;
    m_hudPreparedPairSerial = 0;
}

void OpenXRContext::InvalidateHudResources() {
    AcquireSRWLockExclusive(&m_hudLock);
    DestroyHudSwapchain();
    ReleaseSRWLockExclusive(&m_hudLock);
}

bool OpenXRContext::CreateHudSwapchain(uint32_t width, uint32_t height) {
    if (!CanSubmitHud() || !width || !height ||
        width > m_systemProperties.graphicsProperties.maxSwapchainImageWidth ||
        height > m_systemProperties.graphicsProperties.maxSwapchainImageHeight) {
        return false;
    }
    DestroyHudSwapchain();

    XrSwapchainCreateInfo createInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
        XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = static_cast<int64_t>(m_swapchainFormat);
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    XrResult result = xrCreateSwapchain(m_session, &createInfo, &m_hud.swapchain);
    if (result == XR_SESSION_LOSS_PENDING) {
        RequestRecovery("xrCreateSwapchain(HUD)", result);
        return false;
    }
    if (result != XR_SUCCESS) {
        m_hud.swapchain = XR_NULL_HANDLE;
        Log("[HUD] xrCreateSwapchain failed: %d", (int)result);
        return false;
    }

    uint32_t imageCount = 0;
    result = xrEnumerateSwapchainImages(m_hud.swapchain, 0, &imageCount, nullptr);
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrEnumerateSwapchainImages(HUD)", result);
    if (result != XR_SUCCESS || !imageCount) {
        DestroyHudSwapchain();
        return false;
    }
    m_hud.images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    result = xrEnumerateSwapchainImages(m_hud.swapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(m_hud.images.data()));
    if (result == XR_SESSION_LOSS_PENDING)
        RequestRecovery("xrEnumerateSwapchainImages(HUD)", result);
    if (result != XR_SUCCESS) {
        DestroyHudSwapchain();
        return false;
    }
    m_hud.width = m_hud.submittedWidth = width;
    m_hud.height = m_hud.submittedHeight = height;
    Log("[HUD] OpenXR quad swapchain ready: %ux%u images=%u", width, height, imageCount);
    return true;
}

bool OpenXRContext::PrepareHudTexture(ID3D11Texture2D* texture,
                                      uint64_t pairSerial) {
    if (!texture || !pairSerial || !CanSubmitHud() || !m_context) return false;
    const auto& hudConfig = config::Get();
    const float hudOpacity = hudConfig.hud_opacity;
    const float hudDistance = hudConfig.hud_distance;
    const float hudWidthDegrees = hudConfig.hud_width_degrees;
    const float hudScale = hudConfig.hud_scale;
    const float hudHorizontalOffset = hudConfig.hud_horizontal_offset;
    const float hudVerticalOffset = hudConfig.hud_vertical_offset;
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    texture->GetDesc(&sourceDesc);
    if (!sourceDesc.Width || !sourceDesc.Height || sourceDesc.SampleDesc.Count != 1)
        return false;

    AcquireSRWLockExclusive(&m_hudLock);
    m_hudPrepared = false;
    m_hudPreparedPairSerial = 0;
    if ((m_hud.swapchain == XR_NULL_HANDLE || m_hud.width != sourceDesc.Width ||
         m_hud.height != sourceDesc.Height) &&
        !CreateHudSwapchain(sourceDesc.Width, sourceDesc.Height)) {
        ReleaseSRWLockExclusive(&m_hudLock);
        return false;
    }

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(m_hud.swapchain, &acquireInfo, &imageIndex);
    if (!ObserveFrameResult("xrAcquireSwapchainImage(HUD)", result)) {
        ReleaseSRWLockExclusive(&m_hudLock);
        return false;
    }
    if (!WaitForSwapchainImage(m_hud.swapchain, "xrWaitSwapchainImage(HUD)")) {
        // The image was acquired but never became waitable. Recovery destroys
        // the affected session; releasing here would violate OpenXR call order.
        ReleaseSRWLockExclusive(&m_hudLock);
        return false;
    }
    bool copied = !NeedsRecovery() && imageIndex < m_hud.images.size() &&
        m_hud.images[imageIndex].texture &&
        render::HudBlitter::Instance().Blit(
            m_device, m_context, texture, m_hud.images[imageIndex].texture,
            hudOpacity);
    if (copied) m_context->Flush();
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult releaseResult = xrReleaseSwapchainImage(m_hud.swapchain, &releaseInfo);
    const bool releaseObserved = ObserveFrameResult(
        "xrReleaseSwapchainImage(HUD)", releaseResult);
    copied = copied && releaseObserved && releaseResult == XR_SUCCESS;
    if (copied) {
        m_hudPrepared = true;
        m_hudPreparedPairSerial = pairSerial;
        m_hudPreparedDistance = hudDistance;
        m_hudPreparedWidthDegrees = hudWidthDegrees;
        m_hudPreparedScale = hudScale;
        m_hudPreparedOpacity = hudOpacity;
        m_hudPreparedHorizontalOffset = hudHorizontalOffset;
        m_hudPreparedVerticalOffset = hudVerticalOffset;
    }
    ReleaseSRWLockExclusive(&m_hudLock);
    return copied;
}

void OpenXRContext::DestroyReticleSwapchain() {
    if (m_reticle.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_reticle.swapchain);
        m_reticle.swapchain = XR_NULL_HANDLE;
    }
    m_reticle.images.clear();
    m_reticle.width = m_reticle.height = 0;
    m_reticle.submittedWidth = m_reticle.submittedHeight = 0;
    m_reticlePainted = false;
    m_reticlePrepared = false;
    m_reticlePreparedPairSerial = 0;
}

bool OpenXRContext::CreateReticleSwapchain() {
    constexpr uint32_t kReticleSize = 64;
    const bool alphaFormat = m_swapchainFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
        m_swapchainFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        m_swapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
        m_swapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_deviceBound.load(std::memory_order_acquire) || !alphaFormat ||
        !m_context || m_stageSpace == XR_NULL_HANDLE ||
        m_systemProperties.graphicsProperties.maxLayerCount < 2) {
        return false;
    }
    if (m_reticle.swapchain != XR_NULL_HANDLE) return true;

    XrSwapchainCreateInfo createInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
        XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = static_cast<int64_t>(m_swapchainFormat);
    createInfo.sampleCount = 1;
    createInfo.width = kReticleSize;
    createInfo.height = kReticleSize;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    XrResult result = xrCreateSwapchain(m_session, &createInfo, &m_reticle.swapchain);
    if (!ObserveFrameResult("xrCreateSwapchain(reticle)", result) ||
        result != XR_SUCCESS) {
        m_reticle.swapchain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t imageCount = 0;
    result = xrEnumerateSwapchainImages(
        m_reticle.swapchain, 0, &imageCount, nullptr);
    if (!ObserveFrameResult("xrEnumerateSwapchainImages(reticle count)", result) ||
        result != XR_SUCCESS || !imageCount) {
        DestroyReticleSwapchain();
        return false;
    }
    m_reticle.images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    result = xrEnumerateSwapchainImages(m_reticle.swapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(m_reticle.images.data()));
    if (!ObserveFrameResult("xrEnumerateSwapchainImages(reticle)", result) ||
        result != XR_SUCCESS) {
        DestroyReticleSwapchain();
        return false;
    }
    m_reticle.width = m_reticle.submittedWidth = kReticleSize;
    m_reticle.height = m_reticle.submittedHeight = kReticleSize;
    Log("[AimRay] OpenXR endpoint reticle ready: %ux%u images=%u",
        kReticleSize, kReticleSize, imageCount);
    return true;
}

bool OpenXRContext::PaintReticle() {
    if (m_reticlePainted) return true;
    if (m_reticle.swapchain == XR_NULL_HANDLE || !m_context) return false;
    constexpr uint32_t kReticleSize = 64;
    std::vector<uint32_t> pixels(kReticleSize * kReticleSize, 0);
    const float center = (kReticleSize - 1) * 0.5f;
    for (uint32_t y = 0; y < kReticleSize; ++y) {
        for (uint32_t x = 0; x < kReticleSize; ++x) {
            const float dx = static_cast<float>(x) - center;
            const float dy = static_cast<float>(y) - center;
            const float radius = sqrtf(dx * dx + dy * dy);
            float alpha = 0.0f;
            uint8_t color = 0;
            if (radius <= 13.0f) {
                alpha = (std::min)(1.0f, 14.0f - radius);
                color = 255;
            } else if (radius <= 23.0f) {
                alpha = (std::min)(1.0f, 24.0f - radius);
            }
            const uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
            pixels[y * kReticleSize + x] =
                (a << 24) | (static_cast<uint32_t>(color) << 16) |
                (static_cast<uint32_t>(color) << 8) | color;
        }
    }

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(
        m_reticle.swapchain, &acquireInfo, &imageIndex);
    if (!ObserveFrameResult("xrAcquireSwapchainImage(reticle)", result) ||
        result != XR_SUCCESS || imageIndex >= m_reticle.images.size()) return false;
    if (!WaitForSwapchainImage(m_reticle.swapchain,
                               "xrWaitSwapchainImage(reticle)")) return false;
    if (!m_reticle.images[imageIndex].texture) {
        XrSwapchainImageReleaseInfo releaseInfo = {
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult releaseResult = xrReleaseSwapchainImage(
            m_reticle.swapchain, &releaseInfo);
        ObserveFrameResult("xrReleaseSwapchainImage(reticle null)", releaseResult);
        return false;
    }
    m_context->UpdateSubresource(m_reticle.images[imageIndex].texture, 0, nullptr,
        pixels.data(), kReticleSize * sizeof(uint32_t), 0);
    m_context->Flush();
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    result = xrReleaseSwapchainImage(m_reticle.swapchain, &releaseInfo);
    m_reticlePainted = ObserveFrameResult(
        "xrReleaseSwapchainImage(reticle)", result) && result == XR_SUCCESS;
    return m_reticlePainted;
}

bool OpenXRContext::PrepareReticleAt(const float position[3], const float forward[3],
                                     float visualDistance,
                                     float angularSizeDegrees,
                                     uint64_t pairSerial) {
    if (!position || !forward || !pairSerial ||
        !std::isfinite(visualDistance) || visualDistance < 0.5f ||
        !std::isfinite(angularSizeDegrees) || angularSizeDegrees <= 0.0f) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(position[axis]) || !std::isfinite(forward[axis]))
            return false;
    }
    float direction[3] = {forward[0], forward[1], forward[2]};
    const float length = sqrtf(direction[0] * direction[0] +
        direction[1] * direction[1] + direction[2] * direction[2]);
    if (!std::isfinite(length) || length < 1.0e-5f) return false;
    for (float& value : direction) value /= length;

    float up[3] = {0.0f, 1.0f, 0.0f};
    if (fabsf(direction[1]) > 0.99f) {
        up[0] = 0.0f; up[1] = 0.0f; up[2] = 1.0f;
    }
    const float zAxis[3] = {-direction[0], -direction[1], -direction[2]};
    float xAxis[3] = {
        up[1] * zAxis[2] - up[2] * zAxis[1],
        up[2] * zAxis[0] - up[0] * zAxis[2],
        up[0] * zAxis[1] - up[1] * zAxis[0]};
    const float xLength = sqrtf(xAxis[0] * xAxis[0] +
        xAxis[1] * xAxis[1] + xAxis[2] * xAxis[2]);
    if (!std::isfinite(xLength) || xLength < 1.0e-5f) return false;
    for (float& value : xAxis) value /= xLength;
    const float yAxis[3] = {
        zAxis[1] * xAxis[2] - zAxis[2] * xAxis[1],
        zAxis[2] * xAxis[0] - zAxis[0] * xAxis[2],
        zAxis[0] * xAxis[1] - zAxis[1] * xAxis[0]};

    XrQuaternionf orientation = {};
    const float trace = xAxis[0] + yAxis[1] + zAxis[2];
    if (trace > 0.0f) {
        const float s = sqrtf(trace + 1.0f) * 2.0f;
        orientation.w = 0.25f * s;
        orientation.x = (yAxis[2] - zAxis[1]) / s;
        orientation.y = (zAxis[0] - xAxis[2]) / s;
        orientation.z = (xAxis[1] - yAxis[0]) / s;
    } else if (xAxis[0] > yAxis[1] && xAxis[0] > zAxis[2]) {
        const float s = sqrtf(1.0f + xAxis[0] - yAxis[1] - zAxis[2]) * 2.0f;
        orientation.w = (yAxis[2] - zAxis[1]) / s;
        orientation.x = 0.25f * s;
        orientation.y = (yAxis[0] + xAxis[1]) / s;
        orientation.z = (zAxis[0] + xAxis[2]) / s;
    } else if (yAxis[1] > zAxis[2]) {
        const float s = sqrtf(1.0f + yAxis[1] - xAxis[0] - zAxis[2]) * 2.0f;
        orientation.w = (zAxis[0] - xAxis[2]) / s;
        orientation.x = (yAxis[0] + xAxis[1]) / s;
        orientation.y = 0.25f * s;
        orientation.z = (zAxis[1] + yAxis[2]) / s;
    } else {
        const float s = sqrtf(1.0f + zAxis[2] - xAxis[0] - yAxis[1]) * 2.0f;
        orientation.w = (xAxis[1] - yAxis[0]) / s;
        orientation.x = (zAxis[0] + xAxis[2]) / s;
        orientation.y = (zAxis[1] + yAxis[2]) / s;
        orientation.z = 0.25f * s;
    }

    AcquireSRWLockExclusive(&m_reticleLock);
    m_reticlePrepared = false;
    m_reticlePreparedPairSerial = 0;
    if (!CreateReticleSwapchain() || !PaintReticle()) {
        ReleaseSRWLockExclusive(&m_reticleLock);
        return false;
    }
    m_reticlePose.orientation = orientation;
    m_reticlePose.position = {position[0], position[1], position[2]};
    constexpr float kDegreesToRadians = 0.01745329251994329577f;
    m_reticleSize = 2.0f * visualDistance * tanf(
        angularSizeDegrees * kDegreesToRadians * 0.5f);
    m_reticlePrepared = std::isfinite(m_reticleSize) && m_reticleSize > 0.0f;
    m_reticlePreparedPairSerial = m_reticlePrepared ? pairSerial : 0;
    ReleaseSRWLockExclusive(&m_reticleLock);
    return m_reticlePrepared;
}

void OpenXRContext::PollEvents() {
    if (m_instance == XR_NULL_HANDLE) return;
    AcquireSRWLockExclusive(&m_eventLock);
    while (true) {
        XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult pollResult = xrPollEvent(m_instance, &event);
        if (pollResult == XR_EVENT_UNAVAILABLE) break;
        if (XR_FAILED(pollResult)) {
            Log("[OpenXR] xrPollEvent failed: %d", static_cast<int>(pollResult));
            RequestRecovery("xrPollEvent", pollResult);
            break;
        }
        if (event.type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
            const auto& lost = *reinterpret_cast<XrEventDataEventsLost*>(&event);
            Log("[OpenXR] WARNING: runtime lost %u events", lost.lostEventCount);
            continue;
        }
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            RequestRecovery("XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING",
                            XR_ERROR_INSTANCE_LOST);
            continue;
        }
        if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            input::XRInput::Instance().LogCurrentInteractionProfiles();
            continue;
        }
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;

        const auto& stateChanged =
            *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
        if (stateChanged.session != XR_NULL_HANDLE && stateChanged.session != m_session)
            continue;
        const XrSessionState state = stateChanged.state;
        m_sessionState.store(state, std::memory_order_release);
        Log("[OpenXR] Session state changed: %d", static_cast<int>(state));

        switch (state) {
        case XR_SESSION_STATE_READY:
            if (!m_deviceBound.load(std::memory_order_acquire)) {
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult beginResult = xrBeginSession(m_session, &beginInfo);
                ObserveFrameResult("xrBeginSession", beginResult);
                if (beginResult == XR_SUCCESS) {
                    m_deviceBound.store(true, std::memory_order_release);
                    Log("[OpenXR] Session READY - began");
                } else if (XR_FAILED(beginResult)) {
                    Log("[OpenXR] ERROR: xrBeginSession = %d", (int)beginResult);
                }
            }
            break;
        case XR_SESSION_STATE_STOPPING:
            m_deviceBound.store(false, std::memory_order_release);
            m_endSessionPending.store(true, std::memory_order_release);
            Log("[OpenXR] Session STOPPING - ending after frame worker stops");
            RequestRecovery("session stopped", XR_ERROR_SESSION_NOT_RUNNING);
            break;
        case XR_SESSION_STATE_LOSS_PENDING:
        case XR_SESSION_STATE_EXITING:
            m_deviceBound.store(false, std::memory_order_release);
            RequestRecovery("session state loss", XR_SESSION_LOSS_PENDING);
            break;
        default:
            break;
        }
    }
    ReleaseSRWLockExclusive(&m_eventLock);
}

bool OpenXRContext::WaitForFrame() {
    PollEvents();
    if (NeedsRecovery() || !m_deviceBound.load(std::memory_order_acquire)) return false;

    XrFrameWaitInfo waitInfo = {};
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
    m_frameState = {XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(m_session, &waitInfo, &m_frameState);
    if (!ObserveFrameResult("xrWaitFrame", r) || r == XR_SESSION_LOSS_PENDING)
        return false;
    m_predictedDisplayTime.store(m_frameState.predictedDisplayTime,
                                 std::memory_order_release);
    return XR_SUCCEEDED(r);
}

bool OpenXRContext::BeginFrame() {
    if (m_frameActive.load(std::memory_order_acquire)) return true;
    if (NeedsRecovery()) return false;

    XrFrameBeginInfo beginInfo = {};
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
    XrResult r = xrBeginFrame(m_session, &beginInfo);
    if (!ObserveFrameResult("xrBeginFrame", r)) return false;
    if (r == XR_FRAME_DISCARDED)
        Log("[OpenXR] xrBeginFrame discarded previous frame; continuing");

    m_frameActive = true;
    // The SteamVR compositor pump runs independently from game rendering.
    // Keep the previous coherent pose readable until xrLocateViews publishes
    // the next one instead of creating a brief invalid window every 11 ms.
    if (!m_isSteamRuntime) m_viewsValid = false;
    AcquireSRWLockExclusive(&m_hudLock);
    if (!m_isSteamRuntime) {
        m_hudPrepared = false;
        m_hudPreparedPairSerial = 0;
    }
    ReleaseSRWLockExclusive(&m_hudLock);
    return true;
}

bool OpenXRContext::LocateViews() {
    if (!m_frameActive.load(std::memory_order_acquire) ||
        !m_frameState.shouldRender || NeedsRecovery()) return false;

    XrViewState viewState = {};
    viewState.type = XR_TYPE_VIEW_STATE;

    XrViewLocateInfo locInfo = {};
    locInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
    locInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locInfo.space = m_stageSpace;
    locInfo.displayTime = m_frameState.predictedDisplayTime;

    XrView views[2] = {};
    views[0].type = XR_TYPE_VIEW;
    views[1].type = XR_TYPE_VIEW;
    uint32_t viewCount = 0;

    XrResult r = xrLocateViews(m_session, &locInfo, &viewState, 2, &viewCount, views);
    if (!ObserveFrameResult("xrLocateViews", r) || viewCount < 2) return false;

    AcquireSRWLockExclusive(&m_poseLock);
    m_poseValid = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                  (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    if (!m_poseValid) {
        m_viewsValid = false;
        ReleaseSRWLockExclusive(&m_poseLock);
        return false;
    }

    // Extract per-eye poses.
    for (int i = 0; i < 2; i++) {
        m_eyePositions[i][0] = views[i].pose.position.x;
        m_eyePositions[i][1] = views[i].pose.position.y;
        m_eyePositions[i][2] = views[i].pose.position.z;
        m_eyeRotations[i][0] = views[i].pose.orientation.x;
        m_eyeRotations[i][1] = views[i].pose.orientation.y;
        m_eyeRotations[i][2] = views[i].pose.orientation.z;
        m_eyeRotations[i][3] = views[i].pose.orientation.w;

    }

    // Head position = average of eyes
    for (int c = 0; c < 3; c++) {
        m_headPosition[c] = (m_eyePositions[0][c] + m_eyePositions[1][c]) * 0.5f;
    }
    float rightOrientation[4] = {
        m_eyeRotations[1][0], m_eyeRotations[1][1],
        m_eyeRotations[1][2], m_eyeRotations[1][3]
    };
    const float orientationDot =
        m_eyeRotations[0][0] * rightOrientation[0] +
        m_eyeRotations[0][1] * rightOrientation[1] +
        m_eyeRotations[0][2] * rightOrientation[2] +
        m_eyeRotations[0][3] * rightOrientation[3];
    if (orientationDot < 0.0f) {
        for (float& component : rightOrientation) component = -component;
    }
    for (int component = 0; component < 4; ++component)
        m_headRotation[component] = m_eyeRotations[0][component] + rightOrientation[component];
    const float orientationLength = sqrtf(
        m_headRotation[0] * m_headRotation[0] + m_headRotation[1] * m_headRotation[1] +
        m_headRotation[2] * m_headRotation[2] + m_headRotation[3] * m_headRotation[3]);
    if (orientationLength > 1.0e-6f) {
        for (float& component : m_headRotation) component /= orientationLength;
    } else {
        m_headRotation[0] = m_headRotation[1] = m_headRotation[2] = 0.0f;
        m_headRotation[3] = 1.0f;
    }
    for (int eye = 0; eye < 2; ++eye) {
        m_views[eye] = views[eye];
        m_views[eye].pose.orientation = {
            m_headRotation[0], m_headRotation[1], m_headRotation[2], m_headRotation[3]
        };
    }

    // IPD
    float dx = m_eyePositions[1][0] - m_eyePositions[0][0];
    float dy = m_eyePositions[1][1] - m_eyePositions[0][1];
    float dz = m_eyePositions[1][2] - m_eyePositions[0][2];
    m_ipd = sqrtf(dx * dx + dy * dy + dz * dz);

    // Compute view matrices: conjugate(rot) * translate(-pos)
    static bool loggedFov = false;
    if (!loggedFov) {
        constexpr float kRadiansToDegrees = 57.29577951308232f;
        for (int eye = 0; eye < 2; ++eye) {
            const XrFovf& fov = views[eye].fov;
            Log("[OpenXR] Eye %d FOV: left=%.2f right=%.2f up=%.2f down=%.2f "
                "total=%.2fx%.2f degrees", eye, fov.angleLeft * kRadiansToDegrees,
                fov.angleRight * kRadiansToDegrees, fov.angleUp * kRadiansToDegrees,
                fov.angleDown * kRadiansToDegrees,
                (fov.angleRight - fov.angleLeft) * kRadiansToDegrees,
                (fov.angleUp - fov.angleDown) * kRadiansToDegrees);
        }
        loggedFov = true;
    }
    for (int eye = 0; eye < 2; eye++) {
        float qx = m_eyeRotations[eye][0];
        float qy = m_eyeRotations[eye][1];
        float qz = m_eyeRotations[eye][2];
        float qw = m_eyeRotations[eye][3];

        // Conjugate quaternion
        float cqx = -qx, cqy = -qy, cqz = -qz, cqw = qw;

        // Quaternion to rotation matrix (row-major)
        float r00 = 1 - 2*(cqy*cqy + cqz*cqz);
        float r01 = 2*(cqx*cqy - cqz*cqw);
        float r02 = 2*(cqx*cqz + cqy*cqw);
        float r10 = 2*(cqx*cqy + cqz*cqw);
        float r11 = 1 - 2*(cqx*cqx + cqz*cqz);
        float r12 = 2*(cqy*cqz - cqx*cqw);
        float r20 = 2*(cqx*cqz - cqy*cqw);
        float r21 = 2*(cqy*cqz + cqx*cqw);
        float r22 = 1 - 2*(cqx*cqx + cqy*cqy);

        float tx = -r00*m_eyePositions[eye][0] - r01*m_eyePositions[eye][1] - r02*m_eyePositions[eye][2];
        float ty = -r10*m_eyePositions[eye][0] - r11*m_eyePositions[eye][1] - r12*m_eyePositions[eye][2];
        float tz = -r20*m_eyePositions[eye][0] - r21*m_eyePositions[eye][1] - r22*m_eyePositions[eye][2];

        float (*v)[4] = m_viewMatrices[eye];
        v[0][0] = r00; v[0][1] = r01; v[0][2] = r02; v[0][3] = 0;
        v[1][0] = r10; v[1][1] = r11; v[1][2] = r12; v[1][3] = 0;
        v[2][0] = r20; v[2][1] = r21; v[2][2] = r22; v[2][3] = 0;
        v[3][0] = tx;  v[3][1] = ty;  v[3][2] = tz;  v[3][3] = 1;

        // Projection matrix from FOV angles (D3D row-major)
        const XrFovf& fov = views[eye].fov;
        float tanL = tanf(fov.angleLeft);
        float tanR = tanf(fov.angleRight);
        float tanU = tanf(fov.angleUp);
        float tanD = tanf(fov.angleDown);

        float width = tanR - tanL;
        float height = tanU - tanD;

        float (*p)[4] = m_projectionMatrices[eye];
        memset(p, 0, sizeof(float) * 16);
        p[0][0] = 2.0f / width;
        p[1][1] = 2.0f / height;
        p[2][0] = (tanR + tanL) / width;
        p[2][1] = (tanU + tanD) / height;
        p[2][2] = -(m_farPlane / (m_farPlane - m_nearPlane));
        p[2][3] = -1.0f;
        p[3][2] = -(m_farPlane * m_nearPlane / (m_farPlane - m_nearPlane));
    }

    m_viewsValid = true;
    ReleaseSRWLockExclusive(&m_poseLock);
    return true;
}

bool OpenXRContext::EndFrame(bool submitProjectionLayer,
                             const XrView* exactRenderedViews,
                             bool submitHudLayer,
                             uint64_t hudPairSerial,
                             bool submitReticleLayer,
                             uint64_t reticlePairSerial) {
    if (!m_frameActive.load(std::memory_order_acquire)) return false;

    XrFrameEndInfo endInfo = {};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = m_frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XrCompositionLayerProjectionView projViews[2] = {};
    XrCompositionLayerProjection projLayer = {};
    XrCompositionLayerQuad hudLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad reticleLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[3] = {};
    bool hudLockHeld = false;
    bool reticleLockHeld = false;

    if (submitProjectionLayer && m_viewsValid && ShouldRender()) {
        if (m_theaterAnchored) {
            m_theaterAnchored = false;
            Log("[OpenXR] Theater mode ended; returning to stereo projection");
        }
        for (int i = 0; i < 2; i++) {
            EyeData& eye = (i == 0) ? m_leftEye : m_rightEye;
            XrView submittedView = {};
            if (exactRenderedViews) {
                submittedView = exactRenderedViews[i];
            } else {
                AcquireSRWLockShared(&m_poseLock);
                submittedView = m_useRenderedViewPoses && m_renderedViewValid[i]
                    ? m_renderedViews[i] : m_views[i];
                ReleaseSRWLockShared(&m_poseLock);
            }

            projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projViews[i].pose = submittedView.pose;
            // The blit centers the asymmetric runtime FOV vertically. Describe
            // those exact rays to the compositor or rotational reprojection
            // makes the world swim with head yaw/pitch/roll.
            projViews[i].fov = CenterVerticalFov(submittedView.fov);
            projViews[i].subImage.swapchain = eye.swapchain;
            projViews[i].subImage.imageArrayIndex = 0;
            projViews[i].subImage.imageRect.offset = { 0, 0 };
            projViews[i].subImage.imageRect.extent = { (int32_t)eye.submittedWidth, (int32_t)eye.submittedHeight };
        }

        projLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        projLayer.space = m_stageSpace;
        projLayer.viewCount = 2;
        projLayer.views = projViews;

        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projLayer);
        endInfo.layerCount = 1;
        endInfo.layers = layers;

        const bool reserveReticleLayer = submitReticleLayer && reticlePairSerial;
        const bool hudFitsWithReticle = !reserveReticleLayer ||
            m_systemProperties.graphicsProperties.maxLayerCount >= 3;
        if (submitHudLayer && hudPairSerial && hudFitsWithReticle &&
            (m_isSteamRuntime || ShouldSeparateHud())) {
            AcquireSRWLockShared(&m_hudLock);
            hudLockHeld = true;
            if (m_hudPrepared && m_hudPreparedPairSerial == hudPairSerial &&
                m_hud.swapchain != XR_NULL_HANDLE) {
                hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                hudLayer.space = m_viewSpace;
                hudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hudLayer.subImage.swapchain = m_hud.swapchain;
                hudLayer.subImage.imageRect.offset = {0, 0};
                hudLayer.subImage.imageRect.extent = {
                    static_cast<int32_t>(m_hud.width),
                    static_cast<int32_t>(m_hud.height)};
                hudLayer.subImage.imageArrayIndex = 0;
                hudLayer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                hudLayer.pose.position = {
                    m_hudPreparedHorizontalOffset,
                    m_hudPreparedVerticalOffset,
                    -m_hudPreparedDistance};
                constexpr float kDegreesToRadians = 0.01745329251994329577f;
                const float baseWidth = 2.0f * m_hudPreparedDistance * tanf(
                    m_hudPreparedWidthDegrees * kDegreesToRadians * 0.5f);
                hudLayer.size.width = baseWidth * m_hudPreparedScale;
                hudLayer.size.height = hudLayer.size.width *
                    static_cast<float>(m_hud.height) /
                    static_cast<float>(m_hud.width);
                layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudLayer);
                endInfo.layerCount = 2;
                static bool loggedHudLayer = false;
                if (!loggedHudLayer) {
                    Log("[HUD] VIEW-space quad active: distance=%.2fm width=%.1fdeg "
                        "scale=%.2f opacity=%.2f",
                        m_hudPreparedDistance, m_hudPreparedWidthDegrees,
                        m_hudPreparedScale, m_hudPreparedOpacity);
                    loggedHudLayer = true;
                }
            }
        }
        if (submitReticleLayer && reticlePairSerial &&
            endInfo.layerCount < m_systemProperties.graphicsProperties.maxLayerCount) {
            AcquireSRWLockShared(&m_reticleLock);
            reticleLockHeld = true;
            if (m_reticlePrepared &&
                m_reticlePreparedPairSerial == reticlePairSerial &&
                m_reticle.swapchain != XR_NULL_HANDLE) {
                reticleLayer.layerFlags =
                    XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                    XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                reticleLayer.space = m_stageSpace;
                reticleLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                reticleLayer.subImage.swapchain = m_reticle.swapchain;
                reticleLayer.subImage.imageRect.offset = {0, 0};
                reticleLayer.subImage.imageRect.extent = {
                    static_cast<int32_t>(m_reticle.width),
                    static_cast<int32_t>(m_reticle.height)};
                reticleLayer.subImage.imageArrayIndex = 0;
                reticleLayer.pose = m_reticlePose;
                reticleLayer.size = {m_reticleSize, m_reticleSize};
                layers[endInfo.layerCount++] =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                        &reticleLayer);
                static bool loggedReticleLayer = false;
                if (!loggedReticleLayer) {
                    Log("[AimRay] OpenXR endpoint quad active");
                    loggedReticleLayer = true;
                }
            }
        }
    } else {
        static uint64_t noLayerCount = 0;
        if (++noLayerCount <= 5 || noLayerCount % 300 == 0)
            Log("[OpenXR] EndFrame: NO LAYERS (submit=%d views=%d shouldRender=%d sessionState=%d count=%llu)",
                (int)submitProjectionLayer, (int)m_viewsValid, (int)ShouldRender(),
                GetSessionState(), noLayerCount);
    }

    XrResult r = xrEndFrame(m_session, &endInfo);
    if (reticleLockHeld) ReleaseSRWLockShared(&m_reticleLock);
    if (hudLockHeld) ReleaseSRWLockShared(&m_hudLock);
    const bool resultObserved = ObserveFrameResult("xrEndFrame", r);
    if (!resultObserved || r == XR_SESSION_LOSS_PENDING) {
        Log("[OpenXR] xrEndFrame FAILED: layers=%d result=%d",
            (int)endInfo.layerCount, (int)r);
    } else {
        static uint64_t endFrameOk = 0;
        if (++endFrameOk <= 5 || endFrameOk % 600 == 0) {
            Log("[OpenXR] xrEndFrame OK: layers=%d shouldRender=%d (count=%llu)",
                (int)endInfo.layerCount, (int)m_frameState.shouldRender, endFrameOk);
        }
    }

    m_frameActive = false;
    AcquireSRWLockExclusive(&m_hudLock);
    // SteamVR's compositor pump can present the same completed projection
    // pair more than once. Keep its matching HUD swapchain image available
    // until the game publishes the next complete pair.
    if (!m_isSteamRuntime) {
        m_hudPrepared = false;
        m_hudPreparedPairSerial = 0;
    }
    ReleaseSRWLockExclusive(&m_hudLock);
    AcquireSRWLockExclusive(&m_reticleLock);
    if (!m_isSteamRuntime) {
        m_reticlePrepared = false;
        m_reticlePreparedPairSerial = 0;
    }
    ReleaseSRWLockExclusive(&m_reticleLock);
    return r == XR_SUCCESS;
}

bool OpenXRContext::EndFrameTheater(float sourceAspect, float distance,
                                    float width) {
    if (!m_frameActive.load(std::memory_order_acquire)) return false;

    const float aspect = std::clamp(sourceAspect, 0.5f, 3.0f);
    const float safeDistance = std::clamp(distance, 0.5f, 10.0f);
    const float safeWidth = std::clamp(width, 0.5f, 10.0f);
    if (!m_theaterAnchored) {
        AcquireSRWLockShared(&m_poseLock);
        const float x = m_headRotation[0];
        const float y = m_headRotation[1];
        const float z = m_headRotation[2];
        const float w = m_headRotation[3];
        const float yaw = atan2f(2.0f * (w * y + x * z),
                                 1.0f - 2.0f * (y * y + z * z));
        m_theaterPose.orientation = {
            0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
        m_theaterPose.position = {
            m_headPosition[0] - sinf(yaw) * safeDistance,
            m_headPosition[1],
            m_headPosition[2] - cosf(yaw) * safeDistance};
        ReleaseSRWLockShared(&m_poseLock);
        m_theaterAnchored = true;
        Log("[OpenXR] Theater mode started: distance=%.1fm width=%.1fm aspect=%.3f",
            safeDistance, safeWidth, aspect);
    }

    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = m_stageSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = m_leftEye.swapchain;
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {
        static_cast<int32_t>(m_leftEye.submittedWidth),
        static_cast<int32_t>(m_leftEye.submittedHeight)};
    quad.subImage.imageArrayIndex = 0;
    quad.pose = m_theaterPose;
    quad.size = {safeWidth, safeWidth / aspect};

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = m_frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = ShouldRender() ? 1u : 0u;
    endInfo.layers = ShouldRender() ? layers : nullptr;

    const XrResult result = xrEndFrame(m_session, &endInfo);
    const bool resultObserved = ObserveFrameResult("xrEndFrame(theater)", result);
    if (!resultObserved || result == XR_SESSION_LOSS_PENDING) {
        Log("[OpenXR] xrEndFrame(theater) FAILED: %d", static_cast<int>(result));
    } else {
        static uint64_t theaterFrames = 0;
        ++theaterFrames;
        if (theaterFrames <= 3 || theaterFrames % 300 == 0) {
            Log("[OpenXR] Theater frame submitted (count=%llu)",
                static_cast<unsigned long long>(theaterFrames));
        }
    }

    m_frameActive = false;
    AcquireSRWLockExclusive(&m_hudLock);
    m_hudPrepared = false;
    m_hudPreparedPairSerial = 0;
    ReleaseSRWLockExclusive(&m_hudLock);
    return result == XR_SUCCESS;
}

}} // namespace bl1gotyvr::xr
