#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdint>
#include <atomic>

namespace bl1gotyvr { namespace xr {

class FrameLoop {
public:
    static FrameLoop& Instance();

    void Initialize();
    void Shutdown();

    // Main entry from Present hook
    void OnPresent(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain);

    bool IsVRActive() const { return m_vrActive; }
    bool IsDesktopTestMode() const { return m_desktopTestMode; }
    uint64_t GetFrameCount() const { return m_frameCount; }
    int GetRenderEye() const;
    bool IsProjectionCorrectionEnabled() const { return m_projectionCorrection; }
    bool BeginSequentialRender();
    void SetSequentialRenderEye(int eye);
    bool CaptureSequentialEye(int eye);
    void FinishSequentialRender();
    bool IsSequentialFramePending() const { return m_sequentialFramePending.load(); }

    // Desktop test mode
    void ToggleDesktopTestMode();
    void UpdateDesktopControls();

    // Backbuffer resources
    void InvalidateBackbufferResources();

    // Stereo capture for desktop test mode
    void SaveStereoCapture();

private:
    FrameLoop() = default;

    void EnsureEyeTextures(ID3D11Device* device, ID3D11Texture2D* source);
    bool CopyTextureToEye(ID3D11DeviceContext* context, ID3D11Texture2D* source, int eye);
    bool EnsureBlitResources(ID3D11Device* device);
    bool BlitTexture(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                     ID3D11Texture2D* destination, int eye);
    bool EnsureDesktopDuplication(ID3D11Device* device, IDXGISwapChain* swapChain);
    ID3D11Texture2D* CaptureDesktopFrame(ID3D11Device* device,
                                         ID3D11DeviceContext* context,
                                         IDXGISwapChain* swapChain);
    ID3D11Texture2D* CaptureGdiFrame(ID3D11Device* device,
                                     ID3D11DeviceContext* context,
                                     IDXGISwapChain* swapChain);

    bool m_vrActive = false;
    bool m_initialized = false;
    bool m_desktopTestMode = false;
    bool m_projectionCorrection = false;
    bool m_gdiLatencyCorrection = true;
    uint64_t m_frameCount = 0;

    // Intermediate textures for stereo capture
    ID3D11Texture2D* m_eyeTextures[2] = {};
    ID3D11VertexShader* m_blitVertexShader = nullptr;
    ID3D11PixelShader* m_blitPixelShader = nullptr;
    ID3D11SamplerState* m_blitSampler = nullptr;
    ID3D11Buffer* m_blitConstants = nullptr;
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
    float m_desktopPosition[3] = {};
};

}} // namespace bl1gotyvr::xr
