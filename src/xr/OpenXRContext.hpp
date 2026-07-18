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
    bool IsInitialized() const { return m_initialized; }

    // Frame lifecycle
    bool WaitForFrame();
    bool BeginFrame();
    bool LocateViews();
    bool EndFrame(bool submitProjectionLayer = true);

    bool IsFrameActive() const { return m_frameActive; }
    bool HasLocatedViews() const { return m_viewsValid; }
    bool ShouldRender() const { return m_frameState.shouldRender == XR_TRUE; }
    bool HasValidPose() const { return m_poseValid; }

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
    void MarkEyeRendered(int eye);
    void SetUseRenderedViewPoses(bool enabled) { m_useRenderedViewPoses = enabled; }

    // Device
    ID3D11Device* GetDevice() { return m_device; }

private:
    OpenXRContext() = default;

    bool CreateInstance();
    bool CreateSession();
    bool CreateSpaces();
    bool CreateSwapchains();
    void PollEvents();

    XrResult CheckResult(XrResult result, const char* call);

    bool m_initialized = false;
    bool m_deviceBound = false;

    // OpenXR core
    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;

    // Spaces
    XrSpace m_stageSpace = XR_NULL_HANDLE;
    XrSpace m_viewSpace = XR_NULL_HANDLE;

    // Frame
    XrFrameState m_frameState = {};
    std::atomic<bool> m_frameActive{false};
    XrView m_views[2] = {};
    XrView m_renderedViews[2] = {};
    bool m_renderedViewValid[2] = {};
    bool m_useRenderedViewPoses = true;
    std::atomic<bool> m_viewsValid{false};

    // Eyes
    EyeData m_leftEye;
    EyeData m_rightEye;

    // Computed matrices (row-major, 4x4)
    float m_viewMatrices[2][4][4] = {};
    float m_projectionMatrices[2][4][4] = {};

    // Pose
    float m_headPosition[3] = {};
    float m_headRotation[4] = {};  // quaternion [x,y,z,w]
    float m_eyePositions[2][3] = {};
    float m_eyeRotations[2][4] = {};
    bool m_poseValid = false;
    float m_ipd = 0.064f;
    float m_nearPlane = 0.1f;
    float m_farPlane = 10000.0f;

    // Device
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    DXGI_FORMAT m_swapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
};

}} // namespace bl1gotyvr::xr
