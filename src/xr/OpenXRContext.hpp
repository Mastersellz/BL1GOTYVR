#pragma once

// D3D11 headers must come before OpenXR platform headers
#include <d3d11.h>
#include <dxgi.h>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>
#include <atomic>

namespace bl1gotyvr { namespace xr {

// Format equivalence check for cross-runtime compatibility (SteamVR, VDXR, WMR).
bool AreFormatsCompatible(DXGI_FORMAT a, DXGI_FORMAT b);
bool IsSteamRuntimeSelected();

struct EyeData {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t submittedWidth = 0;
    uint32_t submittedHeight = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
};

class OpenXRContext {
public:
    static OpenXRContext& Instance();

    bool Initialize(ID3D11Device* device, DXGI_FORMAT backbufferFormat);
    void Shutdown();
    bool IsInitialized() const { return m_initialized.load(std::memory_order_acquire); }
    bool IsSteamRuntime() const { return m_isSteamRuntime; }
    bool NeedsRecovery() const { return m_recoveryRequested.load(std::memory_order_acquire); }
    void RequestRecovery(const char* call, XrResult result);
    bool ObserveFrameResult(const char* call, XrResult result);
    bool WaitForSwapchainImage(XrSwapchain swapchain, const char* label);
    void RequestSessionExit();

    // Frame lifecycle
    bool WaitForFrame();
    bool BeginFrame();
    bool LocateViews();
    void PollSessionEvents() { PollEvents(); }
    bool EndFrame(bool submitProjectionLayer = true,
                   const XrView* exactRenderedViews = nullptr,
                   bool submitHudLayer = false,
                   uint64_t hudPairSerial = 0);
    bool EndFrameTheater(float sourceAspect, float distance = 2.5f,
                         float width = 2.8f);
    bool CanSubmitHud() const;
    bool WantsHudCapture() const;
    bool ShouldSeparateHud() const;
    bool ShouldBakeHud() const;
    bool ShouldSubmitCurrentViews() const { return m_integratedHud; }
    bool PrepareHudTexture(ID3D11Texture2D* texture, uint64_t pairSerial);
    void InvalidateHudResources();

    bool IsFrameActive() const { return m_frameActive; }
    bool HasLocatedViews() const { return m_viewsValid; }
    bool ShouldRender() const { return m_frameState.shouldRender == XR_TRUE; }
    XrTime GetPredictedDisplayTime() const {
        return m_predictedDisplayTime.load(std::memory_order_acquire);
    }
    bool HasValidPose() const { return m_poseValid; }
    int GetSessionState() const { return (int)m_sessionState.load(std::memory_order_acquire); }
    bool GetPoseSnapshot(float headPosition[3], float headRotation[4],
                         XrView views[2]) const;

    // Eye data
    EyeData& GetLeftEye() { return m_leftEye; }
    EyeData& GetRightEye() { return m_rightEye; }

    // Pose
    float* GetHeadPosition() { return m_headPosition; }
    float* GetHeadRotation() { return m_headRotation; }  // quaternion [x,y,z,w]
    float* GetEyePosition(int eye) { return m_eyePositions[eye]; }
    float* GetEyeRotation(int eye) { return m_eyeRotations[eye]; }
    float (*GetViewMatrix(int eye))[4] { return m_viewMatrices[eye]; }
    float (*GetProjectionMatrix(int eye))[4] { return m_projectionMatrices[eye]; }
    float GetIPD() const { return m_ipd; }
    bool GetProjectionCrop(int eye, float sourceAspect, float& scaleX, float& scaleY,
                           float& offsetX, float& offsetY, float& horizontalFovDegrees) const;
    bool GetProjectionCrop(const XrView& view, float sourceAspect,
                           float& scaleX, float& scaleY, float& offsetX, float& offsetY,
                           float& horizontalFovDegrees) const;
    void SetSourceProjectionTans(float halfTanX, float halfTanY);
    bool GetSourceProjectionTans(float& halfTanX, float& halfTanY) const;
    void MarkEyeRendered(int eye);
    void SetUseRenderedViewPoses(bool enabled) { m_useRenderedViewPoses = enabled; }
    DXGI_FORMAT GetSwapchainFormat() const { return m_swapchainFormat; }

    // Device
    ID3D11Device* GetDevice() { return m_device; }

private:
    OpenXRContext() = default;

    bool CreateInstance();
    bool CreateSession();
    bool CreateSpaces();
    bool CreateSwapchains();
    void ConfigureRefreshRate();
    bool CreateHudSwapchain(uint32_t width, uint32_t height);
    void DestroyHudSwapchain();
    void PollEvents();

    XrResult CheckResult(XrResult result, const char* call);

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_deviceBound{false};
    std::atomic<bool> m_recoveryRequested{false};
    std::atomic<bool> m_endSessionPending{false};

    // OpenXR core
    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    std::atomic<XrSessionState> m_sessionState{XR_SESSION_STATE_UNKNOWN};
    XrSystemProperties m_systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
    char m_runtimeName[XR_MAX_RUNTIME_NAME_SIZE] = {};
    bool m_isSteamRuntime = false;
    bool m_isVdxr = false;
    bool m_integratedHud = false;
    bool m_refreshRateSupported = false;
    bool m_touchControllerPlusSupported = false;
    SRWLOCK m_eventLock = SRWLOCK_INIT;

    // Spaces
    XrSpace m_stageSpace = XR_NULL_HANDLE;
    XrSpace m_viewSpace = XR_NULL_HANDLE;

    // Frame
    XrFrameState m_frameState = {XR_TYPE_FRAME_STATE};
    std::atomic<XrTime> m_predictedDisplayTime{0};
    std::atomic<bool> m_frameActive{false};
    XrView m_views[2] = {};
    XrView m_renderedViews[2] = {};
    bool m_renderedViewValid[2] = {};
    bool m_useRenderedViewPoses = true;
    std::atomic<bool> m_viewsValid{false};
    bool m_theaterAnchored = false;
    XrPosef m_theaterPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

    // Eyes
    EyeData m_leftEye;
    EyeData m_rightEye;
    EyeData m_hud;
    bool m_hudPrepared = false;
    uint64_t m_hudPreparedPairSerial = 0;
    float m_hudPreparedDistance = 1.5f;
    float m_hudPreparedWidthDegrees = 50.0f;
    float m_hudPreparedScale = 1.0f;
    float m_hudPreparedOpacity = 1.0f;
    float m_hudPreparedHorizontalOffset = 0.0f;
    float m_hudPreparedVerticalOffset = 0.0f;
    SRWLOCK m_hudLock = SRWLOCK_INIT;

    // Computed matrices (row-major, 4x4)
    float m_viewMatrices[2][4][4] = {};
    float m_projectionMatrices[2][4][4] = {};

    // Game-rendered source image half-tangents (measured from FSceneView
    // projection). tanHalfX = 1/projection[0], tanHalfY = 1/projection[5].
    std::atomic<float> m_sourceHalfTanX{0.0f};
    std::atomic<float> m_sourceHalfTanY{0.0f};

    // Pose
    float m_headPosition[3] = {};
    float m_headRotation[4] = {};  // quaternion [x,y,z,w]
    float m_eyePositions[2][3] = {};
    float m_eyeRotations[2][4] = {};
    bool m_poseValid = false;
    mutable SRWLOCK m_poseLock = SRWLOCK_INIT;
    float m_ipd = 0.064f;
    float m_nearPlane = 0.1f;
    float m_farPlane = 10000.0f;

    // Device
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    DXGI_FORMAT m_swapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
};

}} // namespace bl1gotyvr::xr
