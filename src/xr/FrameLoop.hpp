#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <cstdint>
#include <atomic>

namespace bl1gotyvr { namespace xr {

struct StereoRenderTicket {
    bool valid = false;
    uint64_t pairSerial = 0;
    int eye = 0;
    bool baseCameraValid = false;
    bool projectionCorrection = true;
    float renderAspect = 1.0f;
    float baseLocation[3] = {};
    int32_t baseRotation[3] = {};
    float baseFov = 0.0f;
    float headPosition[3] = {};
    float headRotation[4] = {};
    bool rightAimValid = false;
    float rightAimPosition[3] = {};
    float rightAimRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float aimPitchDegrees = 0.0f;
    float aimYawDegrees = 0.0f;
    uint64_t armTargetGeneration = 0;
    XrView views[2] = {};
};

class FrameLoop {
public:
    static FrameLoop& Instance();

    void Initialize();
    void Shutdown();

    // Main entry from Present hook
    void OnPresent(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain);

    bool IsVRActive() const { return m_vrActive; }
    bool IsDesktopTestMode() const { return m_desktopTestMode.load(); }
    uint64_t GetFrameCount() const { return m_frameCount; }
    int GetRenderEye() const;
    bool AcquireRenderTicket(StereoRenderTicket& ticket);
    void CommitRenderedEye(const StereoRenderTicket& ticket);
    void AbortStereoPair();
    bool IsProjectionCorrectionEnabled() const { return m_projectionCorrection; }
    bool BeginSequentialRender();
    void SetSequentialRenderEye(int eye);
    bool CaptureSequentialEye(int eye);
    void FinishSequentialRender();
    bool IsSequentialFramePending() const { return m_sequentialFramePending.load(); }
    void CaptureWorldBeforeHud(uint64_t pairSerial, int eye);

    // Desktop test mode
    void ToggleDesktopTestMode();
    void UpdateDesktopControls();
    void GetDesktopHeadPose(float position[3], float rotation[4]) const;

    // Backbuffer resources
    void InvalidateBackbufferResources();

    // Stereo capture for desktop test mode
    void SaveStereoCapture();

private:
    FrameLoop() = default;

    bool EnsureEyeTextures(ID3D11Device* device, ID3D11Texture2D* source,
                           bool sideBySideSource = false);
    bool CopyTextureToEye(ID3D11DeviceContext* context, ID3D11Texture2D* source, int eye,
                           bool sideBySideSource = false, int sourceEye = -1,
                           ID3D11Texture2D* hudOverlay = nullptr);
    bool EnsureSwapchainUploadTexture(ID3D11Device* device,
                                      const D3D11_TEXTURE2D_DESC& destinationDesc,
                                      int eye);
    bool EnsureBlitResources(ID3D11Device* device);
    bool BlitTexture(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                     ID3D11Texture2D* destination, int eye,
                     bool sideBySideSource = false, int sourceEye = -1);
    bool EnsureDesktopDuplication(ID3D11Device* device, IDXGISwapChain* swapChain);
    ID3D11Texture2D* CaptureDesktopFrame(ID3D11Device* device,
                                         ID3D11DeviceContext* context,
                                         IDXGISwapChain* swapChain);
    ID3D11Texture2D* CaptureGdiFrame(ID3D11Device* device,
                                     ID3D11DeviceContext* context,
                                     IDXGISwapChain* swapChain);
    void OnDesktopPresent(ID3D11Device* device, ID3D11DeviceContext* context,
                          IDXGISwapChain* swapChain);
    void ValidateDesktopStereoPair(ID3D11Device* device, ID3D11DeviceContext* context);
    bool ConsumeRenderedTicket(StereoRenderTicket& ticket);
    bool EnsureHudExtractionTexture(ID3D11Device* device, ID3D11Texture2D* source);
    bool ValidateHudPair(ID3D11Device* device, ID3D11DeviceContext* context,
                         ID3D11Texture2D* finalFrame, ID3D11Texture2D* worldFrame,
                         uint64_t pairSerial);
    bool CompositeHudIntoProjection(ID3D11DeviceContext* context,
                                    ID3D11Texture2D* hudTexture,
                                    ID3D11Texture2D* target, int eye);
    void ResetHudCaptureMetadata();
    void ResetStereoPair();
    void StartWaitWorker();
    void StopWaitWorker();
    static DWORD WINAPI WaitWorkerProc(void* context);

    bool m_vrActive = false;
    bool m_initialized = false;
    bool m_poseSeeded = false;
    HANDLE m_waitWorker = nullptr;
    HANDLE m_waitRequestEvent = nullptr;
    HANDLE m_waitReadyEvent = nullptr;
    std::atomic<bool> m_waitWorkerStop{false};
    std::atomic<bool> m_desktopTestMode{false};
    bool m_projectionCorrection = true;
    bool m_gdiLatencyCorrection = true;
    std::atomic<float> m_renderAspect{1.0f};
    uint64_t m_frameCount = 0;
    uint64_t m_lastNativeMultiviewGeneration = 0;
    bool m_submittingNativeEyes = false;

    mutable SRWLOCK m_stereoPairLock = SRWLOCK_INIT;
    mutable SRWLOCK m_captureLock = SRWLOCK_INIT;
    uint64_t m_nextPairSerial = 1;
    int m_nextRenderEye = 0;
    StereoRenderTicket m_activePair = {};
    StereoRenderTicket m_renderedTicketQueue[64] = {};
    uint32_t m_renderedTicketRead = 0;
    uint32_t m_renderedTicketWrite = 0;
    uint32_t m_renderedTicketCount = 0;
    uint64_t m_capturePairSerial = 0;
    uint64_t m_eyeCaptureSerial[2] = {};
    bool m_eyeCaptureWasBackbuffer[2] = {};
    uint8_t m_eyeCaptureMask = 0;
    UINT m_captureWidth = 0;
    UINT m_captureHeight = 0;
    DXGI_FORMAT m_captureFormat = DXGI_FORMAT_UNKNOWN;
    UINT m_captureSamples = 0;
    XrView m_capturePairViews[2] = {};
    XrView m_submissionViews[2] = {};
    bool m_submissionViewsValid = false;
    bool m_submissionRightAimValid = false;
    float m_submissionRightAimRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float m_submissionAimPitchDegrees = 0.0f;
    float m_submissionAimYawDegrees = 0.0f;
    float m_lastDotSettings[2][4] = {};
    bool m_submissionProjectionCorrection = true;
    float m_submissionRenderAspect = 1.0f;

    // Intermediate textures for stereo capture
    ID3D11Texture2D* m_eyeTextures[2] = {};
    ID3D11Texture2D* m_swapchainUploadTextures[2] = {};
    ID3D11Texture2D* m_worldBeforeHudTextures[2] = {};
    ID3D11Texture2D* m_hudExtractionTexture = nullptr;
    ID3D11Texture2D* m_hudValidationTexture = nullptr;
    ID3D11Texture2D* m_hudValidationStaging = nullptr;
    bool m_hudWorldValidated = false;
    uint64_t m_nextHudValidationPair = 0;
    uint64_t m_worldCapturePairSerial = 0;
    uint64_t m_worldCaptureSerial[2] = {};
    uint8_t m_worldCaptureMask = 0;
    ID3D11VertexShader* m_blitVertexShader = nullptr;
    ID3D11PixelShader* m_blitPixelShader = nullptr;
    ID3D11SamplerState* m_blitSampler = nullptr;
    ID3D11Buffer* m_blitConstants = nullptr;
    ID3D11DepthStencilState* m_blitDepthState = nullptr;
    ID3D11RasterizerState* m_blitRasterizerState = nullptr;
    IDXGIOutputDuplication* m_desktopDuplication = nullptr;
    bool m_desktopDuplicationUnavailable = false;
    ID3D11Texture2D* m_desktopCaptureTexture = nullptr;
    RECT m_desktopOutputRect = {};
    HDC m_gdiMemoryDc = nullptr;
    HBITMAP m_gdiBitmap = nullptr;
    void* m_gdiPixels = nullptr;
    UINT m_gdiWidth = 0;
    UINT m_gdiHeight = 0;
    ID3D11Texture2D* m_gdiCaptureTexture = nullptr;
    std::atomic<int> m_sequentialRenderEye{-1};
    std::atomic<uint8_t> m_sequentialCaptureMask{0};
    std::atomic<bool> m_sequentialFramePending{false};
    std::atomic<bool> m_sequentialCaptureFailed{false};

    // Desktop test mode state
    float m_desktopYaw = 0;
    float m_desktopPitch = 0;
    float m_desktopRoll = 0;
    float m_desktopPosition[3] = {};
    mutable SRWLOCK m_desktopPoseLock = SRWLOCK_INIT;
    uint8_t m_desktopCaptureMask = 0;
    uint64_t m_desktopPairSerial = 0;
};

}} // namespace bl1gotyvr::xr
