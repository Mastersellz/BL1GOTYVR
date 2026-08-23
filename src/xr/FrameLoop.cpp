#include "FrameLoop.hpp"
#include "OpenXRContext.hpp"
#include "../core/VRMod.hpp"
#include "../core/globals.hpp"
#include "../core/CommandSystem.hpp"
#include "../input/InputHook.hpp"
#include "../input/WeaponAimSystem.hpp"
#include "../d3d11/D3D11Hooks.hpp"
#include "../camera/CameraHook.hpp"
#include "../config/Config.hpp"
#include "../input/XRInput.hpp"
#include "../render/HudBlitter.hpp"
#include <cstring>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace bl1gotyvr { namespace xr {

FrameLoop& FrameLoop::Instance() {
    static FrameLoop fl;
    return fl;
}

void FrameLoop::Initialize() {
    Log("[FrameLoop] Initializing...");
    m_vrActive = true;
    m_desktopTestMode = false;
    m_gdiLatencyCorrection = true;
    OpenXRContext::Instance().SetUseRenderedViewPoses(true);
    m_desktopDuplicationUnavailable = false;
    m_frameCount = 0;
    m_missingTicketPresents = 0;
    m_hasSubmittedStereoProjection = false;
    m_desktopCaptureMask = 0;
    m_desktopPairSerial = 0;
    m_poseSeeded = false;
    m_projectionCorrection = true;
    m_renderAspect = static_cast<float>(config::Get().render_width) /
        static_cast<float>((std::max)(1, config::Get().render_height));
    ResetStereoPair();
    m_initialized = true;
    Log("[FrameLoop] Initialized with ME2 pair-paced OpenXR stereo");
}

void FrameLoop::Shutdown() {
    Log("[FrameLoop] Shutting down...");
    StopWaitWorker();
    InvalidateBackbufferResources();
    if (m_blitConstants) { m_blitConstants->Release(); m_blitConstants = nullptr; }
    if (m_blitDepthState) { m_blitDepthState->Release(); m_blitDepthState = nullptr; }
    if (m_blitRasterizerState) { m_blitRasterizerState->Release(); m_blitRasterizerState = nullptr; }
    if (m_desktopCaptureTexture) {
        m_desktopCaptureTexture->Release();
        m_desktopCaptureTexture = nullptr;
    }
    if (m_desktopDuplication) {
        m_desktopDuplication->Release();
        m_desktopDuplication = nullptr;
    }
    m_desktopDuplicationUnavailable = false;
    if (m_gdiCaptureTexture) { m_gdiCaptureTexture->Release(); m_gdiCaptureTexture = nullptr; }
    if (m_gdiBitmap) { DeleteObject(m_gdiBitmap); m_gdiBitmap = nullptr; }
    if (m_gdiMemoryDc) { DeleteDC(m_gdiMemoryDc); m_gdiMemoryDc = nullptr; }
    m_gdiPixels = nullptr;
    if (m_blitSampler) { m_blitSampler->Release(); m_blitSampler = nullptr; }
    if (m_blitPixelShader) { m_blitPixelShader->Release(); m_blitPixelShader = nullptr; }
    if (m_blitVertexShader) { m_blitVertexShader->Release(); m_blitVertexShader = nullptr; }
    m_initialized = false;
    m_vrActive = false;
    m_poseSeeded = false;
}

DWORD WINAPI FrameLoop::WaitWorkerProc(void* context) {
    auto* frameLoop = static_cast<FrameLoop*>(context);
    while (!frameLoop->m_waitWorkerStop.load(std::memory_order_acquire)) {
        if (WaitForSingleObject(frameLoop->m_waitRequestEvent, INFINITE) != WAIT_OBJECT_0)
            break;
        if (frameLoop->m_waitWorkerStop.load(std::memory_order_acquire)) break;
        auto& xr = OpenXRContext::Instance();
        while (!frameLoop->m_waitWorkerStop.load(std::memory_order_acquire)) {
            if (xr.IsInitialized() && xr.WaitForFrame()) {
                SetEvent(frameLoop->m_waitReadyEvent);
                break;
            }
            Sleep(10);
        }
    }
    return 0;
}

void FrameLoop::StartWaitWorker() {
    if (m_waitWorker || !m_poseSeeded || !OpenXRContext::Instance().IsInitialized()) return;
    m_waitRequestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_waitReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_waitRequestEvent || !m_waitReadyEvent) {
        if (m_waitRequestEvent) CloseHandle(m_waitRequestEvent);
        if (m_waitReadyEvent) CloseHandle(m_waitReadyEvent);
        m_waitRequestEvent = m_waitReadyEvent = nullptr;
        return;
    }
    m_waitWorkerStop.store(false, std::memory_order_release);
    m_waitWorker = CreateThread(nullptr, 0, WaitWorkerProc, this, 0, nullptr);
    if (!m_waitWorker) {
        CloseHandle(m_waitRequestEvent);
        CloseHandle(m_waitReadyEvent);
        m_waitRequestEvent = m_waitReadyEvent = nullptr;
        return;
    }
    SetEvent(m_waitRequestEvent);
    Log("[FrameLoop] OpenXR wait worker started");
}

void FrameLoop::StopWaitWorker() {
    if (!m_waitWorker) return;
    m_waitWorkerStop.store(true, std::memory_order_release);
    SetEvent(m_waitRequestEvent);
    WaitForSingleObject(m_waitWorker, 2000);
    CloseHandle(m_waitWorker);
    CloseHandle(m_waitRequestEvent);
    CloseHandle(m_waitReadyEvent);
    m_waitWorker = nullptr;
    m_waitRequestEvent = m_waitReadyEvent = nullptr;
}

void FrameLoop::InvalidateBackbufferResources() {
    AcquireSRWLockExclusive(&m_captureLock);
    for (int i = 0; i < 2; i++) {
        if (m_eyeTextures[i]) { m_eyeTextures[i]->Release(); m_eyeTextures[i] = nullptr; }
        if (m_swapchainUploadTextures[i]) {
            m_swapchainUploadTextures[i]->Release();
            m_swapchainUploadTextures[i] = nullptr;
        }
        if (m_worldBeforeHudTextures[i]) {
            m_worldBeforeHudTextures[i]->Release();
            m_worldBeforeHudTextures[i] = nullptr;
        }
    }
    if (m_hudExtractionTexture) {
        m_hudExtractionTexture->Release();
        m_hudExtractionTexture = nullptr;
    }
    if (m_hudValidationTexture) {
        m_hudValidationTexture->Release();
        m_hudValidationTexture = nullptr;
    }
    if (m_hudValidationStaging) {
        m_hudValidationStaging->Release();
        m_hudValidationStaging = nullptr;
    }
    m_hudWorldValidated = false;
    m_nextHudValidationPair = 0;
    ResetHudCaptureMetadata();
    ReleaseSRWLockExclusive(&m_captureLock);
    render::HudBlitter::Instance().Shutdown();
    OpenXRContext::Instance().InvalidateHudResources();
    m_sequentialRenderEye = -1;
    m_sequentialCaptureMask = 0;
    m_sequentialFramePending = false;
    m_sequentialCaptureFailed = false;
    m_desktopCaptureMask = 0;
    ResetStereoPair();
}

bool FrameLoop::EnsureEyeTextures(ID3D11Device* device, ID3D11Texture2D* source,
                                  bool sideBySideSource) {
    D3D11_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);
    if (sideBySideSource) desc.Width /= 2;
    bool recreated = false;

    for (int i = 0; i < 2; i++) {
        if (m_eyeTextures[i]) {
            D3D11_TEXTURE2D_DESC oldDesc;
            m_eyeTextures[i]->GetDesc(&oldDesc);
            if (oldDesc.Width == desc.Width && oldDesc.Height == desc.Height &&
                oldDesc.Format == desc.Format && oldDesc.SampleDesc.Count == 1) {
                continue;  // texture matches
            }
            m_eyeTextures[i]->Release();
            m_eyeTextures[i] = nullptr;
            recreated = true;
        }

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = desc.Width;
        texDesc.Height = desc.Height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = desc.Format;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &m_eyeTextures[i]);
        recreated = true;
        if (FAILED(hr)) {
            Log("[FrameLoop] ERROR: CreateTexture2D(eye %d) = 0x%08X", i, hr);
        }
    }
    return recreated;
}

bool FrameLoop::EnsureSwapchainUploadTexture(
        ID3D11Device* device, const D3D11_TEXTURE2D_DESC& destinationDesc, int eye) {
    if (!device || eye < 0 || eye > 1 || !destinationDesc.Width || !destinationDesc.Height)
        return false;
    const DXGI_FORMAT format = OpenXRContext::Instance().GetSwapchainFormat();
    if (format == DXGI_FORMAT_UNKNOWN) return false;

    if (m_swapchainUploadTextures[eye]) {
        D3D11_TEXTURE2D_DESC current = {};
        m_swapchainUploadTextures[eye]->GetDesc(&current);
        if (current.Width == destinationDesc.Width &&
            current.Height == destinationDesc.Height && current.Format == format &&
            current.SampleDesc.Count == 1) return true;
        m_swapchainUploadTextures[eye]->Release();
        m_swapchainUploadTextures[eye] = nullptr;
    }

    D3D11_TEXTURE2D_DESC localDesc = {};
    localDesc.Width = destinationDesc.Width;
    localDesc.Height = destinationDesc.Height;
    localDesc.MipLevels = 1;
    localDesc.ArraySize = 1;
    localDesc.Format = format;
    localDesc.SampleDesc.Count = 1;
    localDesc.Usage = D3D11_USAGE_DEFAULT;
    localDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const HRESULT result = device->CreateTexture2D(
        &localDesc, nullptr, &m_swapchainUploadTextures[eye]);
    if (FAILED(result)) {
        Log("[FrameLoop] OpenXR local eye texture creation failed: eye=%d fmt=%u "
            "%ux%u result=0x%08X", eye, format, localDesc.Width, localDesc.Height, result);
        return false;
    }
    Log("[FrameLoop] OpenXR local eye texture ready: eye=%d fmt=%u %ux%u",
        eye, format, localDesc.Width, localDesc.Height);
    return true;
}

bool FrameLoop::EnsureBlitResources(ID3D11Device* device) {
    if (m_blitVertexShader && m_blitPixelShader && m_blitSampler && m_blitConstants &&
        m_blitDepthState && m_blitRasterizerState) return true;
    static constexpr char vertexShader[] = R"(
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    Output output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
})";
    static constexpr char pixelShader[] = R"(
Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
    cbuffer BlitSettings : register(b0) {
        float hdrSource; float eyeShift; float2 uvScale;
        float2 uvOffset; float2 padding;
        float4 dotSettings;
        float4 outputSettings;
    };
    float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
        float2 screenUv = uv;
        uv = saturate(uv * uvScale + uvOffset + float2(eyeShift, 0.0));
    float4 color = sourceTexture.Sample(sourceSampler, uv);
    if (hdrSource > 0.5) {
        float3 x = max(color.rgb, 0.0);
        color.rgb = saturate((x * (2.51 * x + 0.03)) /
                             (x * (2.43 * x + 0.59) + 0.14));
    } else if (outputSettings.y > 0.5) {
        float3 low = color.rgb / 12.92;
        float3 high = pow((color.rgb + 0.055) / 1.055, 2.4);
        color.rgb = lerp(high, low, step(color.rgb, 0.04045));
    }
    if (dotSettings.w > 0.5) {
        float2 delta = screenUv - dotSettings.xy;
        delta.x *= outputSettings.x;
        float distanceToDot = length(delta);
        if (distanceToDot < dotSettings.z)
            color = distanceToDot < dotSettings.z * 0.55
                ? float4(1.0, 1.0, 1.0, 1.0)
                : float4(0.0, 0.0, 0.0, 1.0);
    }
    return color;
})";

    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(vertexShader, sizeof(vertexShader), nullptr, nullptr, nullptr,
                            "main", "vs_5_0", 0, 0, &vertexBlob, &errors);
    if (FAILED(hr)) {
        Log("[FrameLoop] D3DCompile(VS) failed: 0x%08X%s%s", hr,
            errors ? " - " : "", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        return false;
    }
    if (errors) { errors->Release(); errors = nullptr; }
    hr = D3DCompile(pixelShader, sizeof(pixelShader), nullptr, nullptr, nullptr,
                    "main", "ps_5_0", 0, 0, &pixelBlob, &errors);
    if (FAILED(hr)) {
        Log("[FrameLoop] D3DCompile(PS) failed: 0x%08X%s%s", hr,
            errors ? " - " : "", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        vertexBlob->Release();
        return false;
    }
    if (errors) errors->Release();
    hr = device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                    nullptr, &m_blitVertexShader);
    if (SUCCEEDED(hr)) {
        hr = device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(),
                                       nullptr, &m_blitPixelShader);
    }
    vertexBlob->Release();
    pixelBlob->Release();
    if (FAILED(hr)) {
        Log("[FrameLoop] Create blit shader failed: 0x%08X", hr);
        return false;
    }

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampler, &m_blitSampler);
    if (FAILED(hr)) Log("[FrameLoop] CreateSamplerState failed: 0x%08X", hr);
    if (FAILED(hr)) return false;
    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = 64;
    constantsDesc.Usage = D3D11_USAGE_DEFAULT;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device->CreateBuffer(&constantsDesc, nullptr, &m_blitConstants);
    if (FAILED(hr)) Log("[FrameLoop] Create blit constants failed: 0x%08X", hr);
    if (FAILED(hr)) return false;
    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;
    hr = device->CreateDepthStencilState(&depthDesc, &m_blitDepthState);
    if (FAILED(hr)) {
        Log("[FrameLoop] Create blit depth state failed: 0x%08X", hr);
        return false;
    }
    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rasterizerDesc, &m_blitRasterizerState);
    if (FAILED(hr)) Log("[FrameLoop] Create blit rasterizer state failed: 0x%08X", hr);
    return SUCCEEDED(hr);
}

bool FrameLoop::BlitTexture(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                            ID3D11Texture2D* destination, int eye,
                            bool sideBySideSource, int sourceEye,
                            bool flatSource) {
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device || !EnsureBlitResources(device)) {
        if (device) device->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    D3D11_TEXTURE2D_DESC destinationDesc = {};
    source->GetDesc(&sourceDesc);
    destination->GetDesc(&destinationDesc);
    ID3D11ShaderResourceView* sourceView = nullptr;
    ID3D11RenderTargetView* destinationView = nullptr;
    auto typedFormat = [](DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return format;
        }
    };
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    // Use the typed format directly — GPU automatically handles BGRA↔RGBA
    // swizzle when the SRV format matches the texture's native channel order.
    // Forcing RGBA for BGRA textures causes red↔blue channel swap on VDXR/WMR.
    srvDesc.Format = typedFormat(sourceDesc.Format);
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    const DXGI_FORMAT selectedSwapchainFormat =
        OpenXRContext::Instance().GetSwapchainFormat();
    const bool destinationIsTypeless =
        destinationDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        destinationDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        destinationDesc.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS;
    rtvDesc.Format = destinationIsTypeless
        ? selectedSwapchainFormat : typedFormat(destinationDesc.Format);
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    HRESULT hr = device->CreateShaderResourceView(source, &srvDesc, &sourceView);
    if (FAILED(hr)) {
        Log("[FrameLoop] CreateShaderResourceView failed: 0x%08X fmt=%u samples=%u bind=0x%X",
            hr, sourceDesc.Format, sourceDesc.SampleDesc.Count, sourceDesc.BindFlags);
    } else {
        hr = device->CreateRenderTargetView(destination, &rtvDesc, &destinationView);
        if (FAILED(hr)) {
            Log("[FrameLoop] CreateRenderTargetView failed: 0x%08X fmt=%u samples=%u bind=0x%X",
                hr, destinationDesc.Format, destinationDesc.SampleDesc.Count, destinationDesc.BindFlags);
        }
    }
    device->Release();
    if (FAILED(hr)) {
        if (sourceView) sourceView->Release();
        return false;
    }

    ID3D11RenderTargetView* oldTarget = nullptr;
    ID3D11DepthStencilView* oldDepth = nullptr;
    ID3D11VertexShader* oldVS = nullptr;
    ID3D11PixelShader* oldPS = nullptr;
    ID3D11ShaderResourceView* oldResource = nullptr;
    ID3D11SamplerState* oldSampler = nullptr;
    ID3D11Buffer* oldConstants = nullptr;
    ID3D11InputLayout* oldLayout = nullptr;
    ID3D11BlendState* oldBlend = nullptr;
    ID3D11DepthStencilState* oldDepthState = nullptr;
    ID3D11RasterizerState* oldRasterizerState = nullptr;
    FLOAT oldBlendFactor[4] = {};
    UINT oldSampleMask = 0;
    UINT oldStencilRef = 0;
    D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->OMGetRenderTargets(1, &oldTarget, &oldDepth);
    context->RSGetViewports(&viewportCount, oldViewports);
    context->VSGetShader(&oldVS, nullptr, nullptr);
    context->PSGetShader(&oldPS, nullptr, nullptr);
    context->PSGetShaderResources(0, 1, &oldResource);
    context->PSGetSamplers(0, 1, &oldSampler);
    context->PSGetConstantBuffers(0, 1, &oldConstants);
    context->IAGetInputLayout(&oldLayout);
    context->IAGetPrimitiveTopology(&oldTopology);
    context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    context->OMGetDepthStencilState(&oldDepthState, &oldStencilRef);
    context->RSGetState(&oldRasterizerState);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(destinationDesc.Width);
    viewport.Height = static_cast<float>(destinationDesc.Height);
    viewport.MaxDepth = 1.0f;
    context->OMSetRenderTargets(1, &destinationView, nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_blitVertexShader, nullptr, 0);
    context->PSSetShader(m_blitPixelShader, nullptr, 0);
    context->PSSetShaderResources(0, 1, &sourceView);
    context->PSSetSamplers(0, 1, &m_blitSampler);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    context->OMSetDepthStencilState(m_blitDepthState, 0);
    context->RSSetState(m_blitRasterizerState);
    float convergenceShift = 0.0f;
    float uvScaleX = 1.0f, uvScaleY = 1.0f;
    float uvOffsetX = 0.0f, uvOffsetY = 0.0f;
    float contentScaleX = 1.0f, contentScaleY = 1.0f;
    float projectionFov = 0.0f;
    const auto& vrSettings = config::Get();
    const int sampledEye = sourceEye >= 0 ? sourceEye : eye;
    uint32_t principalWidth = 0, principalHeight = 0;
    const bool principalExtentValid = !flatSource && !sideBySideSource &&
        camera::GetPrincipalRenderExtent(principalWidth, principalHeight) &&
        principalWidth <= sourceDesc.Width && principalHeight <= sourceDesc.Height;
    if (principalExtentValid) {
        contentScaleX = static_cast<float>(principalWidth) / sourceDesc.Width;
        contentScaleY = static_cast<float>(principalHeight) / sourceDesc.Height;
    }
    const float textureAspect = sideBySideSource
        ? static_cast<float>(sourceDesc.Width / 2) / sourceDesc.Height
        : (principalExtentValid
            ? static_cast<float>(principalWidth) / principalHeight
            : static_cast<float>(sourceDesc.Width) / sourceDesc.Height);
    const float sourceAspect = textureAspect;
    const bool projectionCorrectionEnabled = !flatSource && (m_submissionViewsValid
        ? m_submissionProjectionCorrection : m_projectionCorrection);
    bool projectionCrop = false;
    if (projectionCorrectionEnabled) {
        auto& openXR = OpenXRContext::Instance();
        projectionCrop = m_submissionViewsValid
            ? openXR.GetProjectionCrop(m_submissionViews[sampledEye], sourceAspect,
                uvScaleX, uvScaleY, uvOffsetX, uvOffsetY, projectionFov)
            : openXR.GetProjectionCrop(sampledEye, sourceAspect,
                uvScaleX, uvScaleY, uvOffsetX, uvOffsetY, projectionFov);
    }
    if (sideBySideSource) {
        uvScaleX *= 0.5f;
        uvOffsetX = (sampledEye == 0 ? 0.0f : 0.5f) + uvOffsetX * 0.5f;
    } else if (!flatSource && !m_submittingNativeEyes && !projectionCrop &&
               vrSettings.convergence_m > 0.0f && eye >= 0 && eye < 2) {
        const float magnitude = (std::min)(0.20f, vrSettings.convergence_m * 0.01f);
        convergenceShift = eye == 0 ? -magnitude : magnitude;
    }
    if (!sideBySideSource) {
        uvScaleX *= contentScaleX;
        uvScaleY *= contentScaleY;
        uvOffsetX *= contentScaleX;
        uvOffsetY *= contentScaleY;
    }
    if (eye == 0) {
        static float loggedConvergence = -1.0f;
        if (loggedConvergence != vrSettings.convergence_m) {
            Log("[FrameLoop] Live convergence applied: %.2f%% (UV shift %.4f per eye)",
                vrSettings.convergence_m, convergenceShift);
            loggedConvergence = vrSettings.convergence_m;
        }
        static bool loggedProjectionCrop = false;
        if (projectionCrop && !loggedProjectionCrop) {
            Log("[FrameLoop] Projection crop: FOV=%.2f scale=(%.4f,%.4f) "
                "offset=(%.4f,%.4f) content=(%.4f,%.4f)", projectionFov,
                uvScaleX, uvScaleY, uvOffsetX, uvOffsetY, contentScaleX, contentScaleY);
            loggedProjectionCrop = true;
        }
    }
    float dotU = 0.5f, dotV = 0.5f, dotEnabled = 0.0f;
    if (!flatSource && m_submissionRightAimValid && m_submissionViewsValid &&
        input::InputHook::Instance().IsAimDotVisible() &&
        sampledEye >= 0 && sampledEye < 2) {
        auto rotate = [](const float quaternion[4], const float vector[3], float output[3]) {
            const float qx = quaternion[0], qy = quaternion[1];
            const float qz = quaternion[2], qw = quaternion[3];
            const float tx = 2.0f * (qy * vector[2] - qz * vector[1]);
            const float ty = 2.0f * (qz * vector[0] - qx * vector[2]);
            const float tz = 2.0f * (qx * vector[1] - qy * vector[0]);
            output[0] = vector[0] + qw * tx + (qy * tz - qz * ty);
            output[1] = vector[1] + qw * ty + (qz * tx - qx * tz);
            output[2] = vector[2] + qw * tz + (qx * ty - qy * tx);
        };
        float aimForward[3] = {};
        input::BuildCalibratedLocalForward(
            m_submissionAimPitchDegrees, m_submissionAimYawDegrees, aimForward);
        float trackingForward[3] = {};
        rotate(m_submissionRightAimRotation, aimForward, trackingForward);
        // The dot is drawn into the destination eye. sourceEye may refer to a
        // crossed/reversed capture texture and must not select the eye pose.
        const XrView& eyeView = m_submissionViews[eye];
        const float inverseEye[4] = {
            -eyeView.pose.orientation.x, -eyeView.pose.orientation.y,
            -eyeView.pose.orientation.z, eyeView.pose.orientation.w
        };
        float eyeLocal[3] = {};
        rotate(inverseEye, trackingForward, eyeLocal);
        if (eyeLocal[2] < -0.001f) {
            const float tangentX = eyeLocal[0] / -eyeLocal[2];
            const float tangentY = eyeLocal[1] / -eyeLocal[2];
            const float tanLeft = tanf(eyeView.fov.angleLeft);
            const float tanRight = tanf(eyeView.fov.angleRight);
            const float tanDown = tanf(eyeView.fov.angleDown);
            const float tanUp = tanf(eyeView.fov.angleUp);
            // Horizontal crop follows the asymmetric eye FOV. Vertical crop
            // deliberately centers the symmetric UE3 source projection, so
            // the marker must use that same centered span to match world hits.
            dotU = (tangentX - tanLeft) / (tanRight - tanLeft);
            const float verticalHalfSpan = (tanUp - tanDown) * 0.5f;
            dotV = (verticalHalfSpan - tangentY) / (verticalHalfSpan * 2.0f);
            dotEnabled = dotU >= 0.0f && dotU <= 1.0f &&
                          dotV >= 0.0f && dotV <= 1.0f ? 1.0f : 0.0f;
            static uint32_t rayProjectionLogs = 0;
            if (eye == 0 && (++rayProjectionLogs % 300) == 1) {
                Log("[AimRay] Quest aim q=(%.3f,%.3f,%.3f,%.3f) "
                    "trackingDir=(%.3f,%.3f,%.3f) eyeDir=(%.3f,%.3f,%.3f) "
                    "dot=(%.3f,%.3f)",
                    m_submissionRightAimRotation[0], m_submissionRightAimRotation[1],
                    m_submissionRightAimRotation[2], m_submissionRightAimRotation[3],
                    trackingForward[0], trackingForward[1], trackingForward[2],
                    eyeLocal[0], eyeLocal[1], eyeLocal[2], dotU, dotV);
            }
        }
    }
    const bool selectedSwapchainIsSrgb =
        selectedSwapchainFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        selectedSwapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        selectedSwapchainFormat == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    const bool sourceIsDisplayEncodedUnorm =
        sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        sourceDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        sourceDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        sourceDesc.Format == DXGI_FORMAT_B8G8R8X8_UNORM ||
        sourceDesc.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS;
    const float settings[16] = {
        sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1.0f : 0.0f,
        convergenceShift, uvScaleX, uvScaleY, uvOffsetX, uvOffsetY, 0.0f, 0.0f,
        dotU, dotV, 0.006f, dotEnabled,
        static_cast<float>(destinationDesc.Width) / destinationDesc.Height,
        selectedSwapchainIsSrgb && sourceIsDisplayEncodedUnorm ? 1.0f : 0.0f,
        0.0f, 0.0f
    };
    if (sampledEye >= 0 && sampledEye < 2) {
        m_lastDotSettings[sampledEye][0] = dotU;
        m_lastDotSettings[sampledEye][1] = dotV;
        m_lastDotSettings[sampledEye][2] = 0.006f;
        m_lastDotSettings[sampledEye][3] = dotEnabled;
    }
    context->UpdateSubresource(m_blitConstants, 0, nullptr, settings, 0, 0);
    context->PSSetConstantBuffers(0, 1, &m_blitConstants);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* nullResource = nullptr;
    context->PSSetShaderResources(0, 1, &nullResource);
    context->OMSetRenderTargets(1, &oldTarget, oldDepth);
    context->RSSetViewports(viewportCount, oldViewports);
    context->IASetInputLayout(oldLayout);
    context->IASetPrimitiveTopology(oldTopology);
    context->VSSetShader(oldVS, nullptr, 0);
    context->PSSetShader(oldPS, nullptr, 0);
    context->PSSetShaderResources(0, 1, &oldResource);
    context->PSSetSamplers(0, 1, &oldSampler);
    context->PSSetConstantBuffers(0, 1, &oldConstants);
    context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    context->OMSetDepthStencilState(oldDepthState, oldStencilRef);
    context->RSSetState(oldRasterizerState);

    if (oldTarget) oldTarget->Release();
    if (oldDepth) oldDepth->Release();
    if (oldVS) oldVS->Release();
    if (oldPS) oldPS->Release();
    if (oldResource) oldResource->Release();
    if (oldSampler) oldSampler->Release();
    if (oldConstants) oldConstants->Release();
    if (oldLayout) oldLayout->Release();
    if (oldBlend) oldBlend->Release();
    if (oldDepthState) oldDepthState->Release();
    if (oldRasterizerState) oldRasterizerState->Release();
    destinationView->Release();
    sourceView->Release();
    return true;
}

bool FrameLoop::EnsureDesktopDuplication(ID3D11Device* device, IDXGISwapChain* swapChain) {
    if (m_desktopDuplication) return true;
    if (m_desktopDuplicationUnavailable) return false;

    DXGI_SWAP_CHAIN_DESC swapDesc = {};
    if (FAILED(swapChain->GetDesc(&swapDesc)) || !swapDesc.OutputWindow) return false;
    const HMONITOR targetMonitor = MonitorFromWindow(swapDesc.OutputWindow, MONITOR_DEFAULTTONEAREST);

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(&dxgiDevice)))) return false;
    HRESULT hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) return false;

    IDXGIOutput* selectedOutput = nullptr;
    for (UINT index = 0; ; ++index) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(index, &output) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC outputDesc = {};
        output->GetDesc(&outputDesc);
        if (outputDesc.Monitor == targetMonitor) {
            selectedOutput = output;
            m_desktopOutputRect = outputDesc.DesktopCoordinates;
            break;
        }
        output->Release();
    }
    adapter->Release();
    if (!selectedOutput) return false;

    IDXGIOutput5* output5 = nullptr;
    hr = selectedOutput->QueryInterface(__uuidof(IDXGIOutput5),
                                        reinterpret_cast<void**>(&output5));
    if (SUCCEEDED(hr) && output5) {
        const DXGI_FORMAT formats[] = {
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R10G10B10A2_UNORM,
            DXGI_FORMAT_R16G16B16A16_FLOAT
        };
        hr = output5->DuplicateOutput1(device, 0, _countof(formats), formats,
                                       &m_desktopDuplication);
        output5->Release();
        if (SUCCEEDED(hr)) Log("[FrameLoop] Desktop Duplication1 initialized");
    }
    if (FAILED(hr) || !m_desktopDuplication) {
        IDXGIOutput1* output1 = nullptr;
        hr = selectedOutput->QueryInterface(__uuidof(IDXGIOutput1),
                                            reinterpret_cast<void**>(&output1));
        if (SUCCEEDED(hr) && output1) {
            hr = output1->DuplicateOutput(device, &m_desktopDuplication);
            output1->Release();
        }
    }
    selectedOutput->Release();
    if (FAILED(hr)) {
        m_desktopDuplicationUnavailable = true;
        static bool loggedDuplicationFailure = false;
        if (!loggedDuplicationFailure) {
            Log("[FrameLoop] DuplicateOutput failed: 0x%08X", hr);
            loggedDuplicationFailure = true;
        }
        return false;
    }
    Log("[FrameLoop] Desktop Duplication initialized for game monitor");
    return true;
}

ID3D11Texture2D* FrameLoop::CaptureDesktopFrame(ID3D11Device* device,
                                                 ID3D11DeviceContext* context,
                                                 IDXGISwapChain* swapChain) {
    if (!EnsureDesktopDuplication(device, swapChain))
        return CaptureGdiFrame(device, context, swapChain);

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    IDXGIResource* resource = nullptr;
    HRESULT hr = m_desktopDuplication->AcquireNextFrame(0, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (m_desktopCaptureTexture) m_desktopCaptureTexture->AddRef();
        return m_desktopCaptureTexture;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        m_desktopDuplication->Release();
        m_desktopDuplication = nullptr;
        m_desktopDuplicationUnavailable = false;
        return nullptr;
    }
    if (FAILED(hr) || !resource) return nullptr;

    ID3D11Texture2D* desktopTexture = nullptr;
    resource->QueryInterface(__uuidof(ID3D11Texture2D),
                             reinterpret_cast<void**>(&desktopTexture));
    resource->Release();
    if (!desktopTexture) {
        m_desktopDuplication->ReleaseFrame();
        return nullptr;
    }

    DXGI_SWAP_CHAIN_DESC swapDesc = {};
    RECT clientRect = {};
    POINT topLeft = {};
    swapChain->GetDesc(&swapDesc);
    GetClientRect(swapDesc.OutputWindow, &clientRect);
    ClientToScreen(swapDesc.OutputWindow, &topLeft);
    const LONG left = (std::max)(topLeft.x, m_desktopOutputRect.left);
    const LONG top = (std::max)(topLeft.y, m_desktopOutputRect.top);
    const LONG right = (std::min)(topLeft.x + clientRect.right, m_desktopOutputRect.right);
    const LONG bottom = (std::min)(topLeft.y + clientRect.bottom, m_desktopOutputRect.bottom);
    const UINT width = right > left ? static_cast<UINT>(right - left) : 0;
    const UINT height = bottom > top ? static_cast<UINT>(bottom - top) : 0;

    D3D11_TEXTURE2D_DESC desktopDesc = {};
    desktopTexture->GetDesc(&desktopDesc);
    if (width && height) {
        bool recreate = !m_desktopCaptureTexture;
        if (m_desktopCaptureTexture) {
            D3D11_TEXTURE2D_DESC oldDesc = {};
            m_desktopCaptureTexture->GetDesc(&oldDesc);
            recreate = oldDesc.Width != width || oldDesc.Height != height ||
                       oldDesc.Format != desktopDesc.Format;
        }
        if (recreate) {
            if (m_desktopCaptureTexture) m_desktopCaptureTexture->Release();
            D3D11_TEXTURE2D_DESC captureDesc = {};
            captureDesc.Width = width;
            captureDesc.Height = height;
            captureDesc.MipLevels = 1;
            captureDesc.ArraySize = 1;
            captureDesc.Format = desktopDesc.Format;
            captureDesc.SampleDesc.Count = 1;
            captureDesc.Usage = D3D11_USAGE_DEFAULT;
            captureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&captureDesc, nullptr, &m_desktopCaptureTexture)))
                m_desktopCaptureTexture = nullptr;
        }
        if (m_desktopCaptureTexture) {
            D3D11_BOX sourceBox = {};
            sourceBox.left = static_cast<UINT>(left - m_desktopOutputRect.left);
            sourceBox.top = static_cast<UINT>(top - m_desktopOutputRect.top);
            sourceBox.right = sourceBox.left + width;
            sourceBox.bottom = sourceBox.top + height;
            sourceBox.front = 0;
            sourceBox.back = 1;
            context->CopySubresourceRegion(m_desktopCaptureTexture, 0, 0, 0, 0,
                                           desktopTexture, 0, &sourceBox);
        }
    }
    desktopTexture->Release();
    m_desktopDuplication->ReleaseFrame();
    if (m_desktopCaptureTexture) m_desktopCaptureTexture->AddRef();
    return m_desktopCaptureTexture;
}

ID3D11Texture2D* FrameLoop::CaptureGdiFrame(ID3D11Device* device,
                                             ID3D11DeviceContext* context,
                                             IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC swapDesc = {};
    RECT clientRect = {};
    POINT topLeft = {};
    if (FAILED(swapChain->GetDesc(&swapDesc)) || !swapDesc.OutputWindow ||
        !GetClientRect(swapDesc.OutputWindow, &clientRect) ||
        !ClientToScreen(swapDesc.OutputWindow, &topLeft)) return nullptr;
    const UINT width = static_cast<UINT>((std::max)(0L, clientRect.right - clientRect.left));
    const UINT height = static_cast<UINT>((std::max)(0L, clientRect.bottom - clientRect.top));
    if (!width || !height) return nullptr;

    if (!m_gdiMemoryDc || width != m_gdiWidth || height != m_gdiHeight) {
        if (m_gdiCaptureTexture) { m_gdiCaptureTexture->Release(); m_gdiCaptureTexture = nullptr; }
        if (m_gdiBitmap) { DeleteObject(m_gdiBitmap); m_gdiBitmap = nullptr; }
        if (m_gdiMemoryDc) { DeleteDC(m_gdiMemoryDc); m_gdiMemoryDc = nullptr; }
        m_gdiPixels = nullptr;

        HDC screenDc = GetDC(nullptr);
        m_gdiMemoryDc = CreateCompatibleDC(screenDc);
        BITMAPINFO bitmapInfo = {};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
        bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        m_gdiBitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS,
                                       &m_gdiPixels, nullptr, 0);
        ReleaseDC(nullptr, screenDc);
        if (!m_gdiMemoryDc || !m_gdiBitmap || !m_gdiPixels) return nullptr;
        SelectObject(m_gdiMemoryDc, m_gdiBitmap);

        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&textureDesc, nullptr, &m_gdiCaptureTexture)))
            return nullptr;
        m_gdiWidth = width;
        m_gdiHeight = height;
        Log("[FrameLoop] GDI window capture initialized: %ux%u", width, height);
    }

    HDC screenDc = GetDC(nullptr);
    const BOOL copied = BitBlt(m_gdiMemoryDc, 0, 0, width, height, screenDc,
                               topLeft.x, topLeft.y, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screenDc);
    if (!copied) return nullptr;
    context->UpdateSubresource(m_gdiCaptureTexture, 0, nullptr, m_gdiPixels, width * 4, 0);
    m_gdiCaptureTexture->AddRef();
    return m_gdiCaptureTexture;
}

bool FrameLoop::CopyTextureToEye(ID3D11DeviceContext* context, ID3D11Texture2D* source, int eye,
                                  bool sideBySideSource, int sourceEye,
                                  ID3D11Texture2D* hudOverlay,
                                  bool flatSource) {
    auto& xr = OpenXRContext::Instance();
    EyeData& eyeData = (eye == 0) ? xr.GetLeftEye() : xr.GetRightEye();

    // Acquire swapchain image
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrResult r = xrAcquireSwapchainImage(eyeData.swapchain, &acquireInfo, &imageIndex);
    if (r != XR_SUCCESS) {
        Log("[FrameLoop] xrAcquireSwapchainImage(eye %d) = %d", eye, (int)r);
        return false;
    }

    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    r = xrWaitSwapchainImage(eyeData.swapchain, &waitInfo);
    if (r != XR_SUCCESS) {
        Log("[FrameLoop] xrWaitSwapchainImage(eye %d) = %d", eye, (int)r);
        XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(eyeData.swapchain, &releaseInfo);
        return false;
    }

    // Get destination texture from swapchain image
    ID3D11Texture2D* destTex = eyeData.images[imageIndex].texture;

    // Render into an application-owned texture with the concrete OpenXR
    // format, then copy into the runtime-owned image. VDXR/Meta exposes
    // TYPELESS swapchain resources; BFVR uses this same typed-local approach
    // instead of creating application RTVs on runtime resources.
    D3D11_TEXTURE2D_DESC srcDesc, dstDesc;
    source->GetDesc(&srcDesc);
    destTex->GetDesc(&dstDesc);

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    bool copied = device && EnsureSwapchainUploadTexture(device, dstDesc, eye);
    if (device) device->Release();
    ID3D11Texture2D* uploadTexture = copied ? m_swapchainUploadTextures[eye] : nullptr;
    D3D11_TEXTURE2D_DESC uploadDesc = {};
    if (uploadTexture) uploadTexture->GetDesc(&uploadDesc);
    if (copied) {
        if (!BlitTexture(context, source, uploadTexture, eye, sideBySideSource,
                         sourceEye, flatSource)) {
            static bool loggedBlitFailure = false;
            if (!loggedBlitFailure) {
                Log("[FrameLoop] ERROR: Failed to blit %ux%u fmt=%u bind=0x%X -> "
                    "%ux%u localFmt=%u runtimeFmt=%u",
                    srcDesc.Width, srcDesc.Height, srcDesc.Format, srcDesc.BindFlags,
                    uploadDesc.Width, uploadDesc.Height, uploadDesc.Format, dstDesc.Format);
                loggedBlitFailure = true;
            }
            copied = false;
        } else if (hudOverlay && !CompositeHudIntoProjection(
                       context, hudOverlay, uploadTexture,
                       sourceEye >= 0 ? sourceEye : eye)) {
            Log("[HUD] Projection HUD composition failed for eye %d; reverting pair", eye);
            copied = false;
        } else {
            context->CopyResource(destTex, uploadTexture);
            static uint64_t blitOkCount = 0;
            if (++blitOkCount <= 3 || blitOkCount % 300 == 0) {
                Log("[FrameLoop] Blit eye=%d OK: %ux%u fmt=%u -> local fmt=%u -> "
                    "%ux%u runtime fmt=%u (count=%llu)",
                    eye, srcDesc.Width, srcDesc.Height, srcDesc.Format,
                    uploadDesc.Format, dstDesc.Width, dstDesc.Height, dstDesc.Format,
                    blitOkCount);
            }
        }
    }

    // Release
    if (copied) context->Flush();
    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    const XrResult releaseResult = xrReleaseSwapchainImage(eyeData.swapchain, &releaseInfo);
    if (releaseResult != XR_SUCCESS) {
        Log("[FrameLoop] xrReleaseSwapchainImage(eye %d) = %d", eye, (int)releaseResult);
        copied = false;
    }
    return copied;
}

int FrameLoop::GetRenderEye() const {
    const int sequentialEye = m_sequentialRenderEye.load();
    if (sequentialEye >= 0) return sequentialEye;
    if (m_desktopTestMode.load()) return static_cast<int>(m_frameCount & 1);
    AcquireSRWLockShared(&m_stereoPairLock);
    const int eye = m_nextRenderEye;
    ReleaseSRWLockShared(&m_stereoPairLock);
    return eye;
}

bool FrameLoop::AcquireRenderTicket(StereoRenderTicket& ticket) {
    ticket = {};
    if (!m_initialized || !m_vrActive || m_desktopTestMode.load()) return false;

    auto& openXR = OpenXRContext::Instance();
    float currentHeadPosition[3] = {};
    float currentHeadRotation[4] = {};
    XrView currentViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    const bool currentPoseValid = openXR.GetPoseSnapshot(
        currentHeadPosition, currentHeadRotation, currentViews);
    input::ControllerState controllers[2] = {};
    input::XRInput::Instance().GetControllerSnapshot(controllers);

    AcquireSRWLockExclusive(&m_stereoPairLock);
    if (m_nextRenderEye == 0) {
        if (!currentPoseValid) {
            ReleaseSRWLockExclusive(&m_stereoPairLock);
            return false;
        }
        m_activePair = {};
        m_activePair.valid = true;
        m_activePair.pairSerial = m_nextPairSerial++;
        m_activePair.projectionCorrection = m_projectionCorrection;
        m_activePair.renderAspect = m_renderAspect.load(std::memory_order_relaxed);
        memcpy(m_activePair.headPosition, currentHeadPosition,
               sizeof(m_activePair.headPosition));
        memcpy(m_activePair.headRotation, currentHeadRotation,
               sizeof(m_activePair.headRotation));
        m_activePair.views[0] = currentViews[0];
        m_activePair.views[1] = currentViews[1];
        m_activePair.aimPitchDegrees = config::Get().aim_pitch_degrees;
        m_activePair.aimYawDegrees = config::Get().aim_yaw_degrees;
        m_activePair.rightAimValid = controllers[1].aimValid;
        if (m_activePair.rightAimValid) {
            memcpy(m_activePair.rightAimPosition, controllers[1].aimPosition,
                   sizeof(m_activePair.rightAimPosition));
            memcpy(m_activePair.rightAimRotation, controllers[1].aimRotation,
                   sizeof(m_activePair.rightAimRotation));
        }
    }
    if (!m_activePair.valid) {
        ReleaseSRWLockExclusive(&m_stereoPairLock);
        return false;
    }
    ticket = m_activePair;
    ticket.eye = m_nextRenderEye;
    ReleaseSRWLockExclusive(&m_stereoPairLock);
    return true;
}

void FrameLoop::CommitRenderedEye(const StereoRenderTicket& ticket) {
    if (!ticket.valid || ticket.eye < 0 || ticket.eye > 1) return;
    AcquireSRWLockExclusive(&m_stereoPairLock);
    if (!m_activePair.valid || ticket.pairSerial != m_activePair.pairSerial ||
        ticket.eye != m_nextRenderEye) {
        ReleaseSRWLockExclusive(&m_stereoPairLock);
        return;
    }
    if (m_renderedTicketCount == _countof(m_renderedTicketQueue)) {
        for (auto& queued : m_renderedTicketQueue) queued = {};
        m_renderedTicketRead = m_renderedTicketWrite = m_renderedTicketCount = 0;
        m_nextRenderEye = 0;
        m_activePair = {};
        ReleaseSRWLockExclusive(&m_stereoPairLock);
        Log("[FrameLoop] Stereo pair dropped: rendered-ticket queue overflow");
        return;
    }
    m_renderedTicketQueue[m_renderedTicketWrite] = ticket;
    m_renderedTicketWrite = (m_renderedTicketWrite + 1) % _countof(m_renderedTicketQueue);
    ++m_renderedTicketCount;
    m_activePair = ticket;
    m_nextRenderEye = ticket.eye ^ 1;
    if (ticket.eye == 1) m_activePair.valid = false;
    ReleaseSRWLockExclusive(&m_stereoPairLock);
}

void FrameLoop::AbortStereoPair() {
    ResetStereoPair();
}

void FrameLoop::ResetHudCaptureMetadata() {
    m_worldCapturePairSerial = 0;
    m_worldCaptureSerial[0] = m_worldCaptureSerial[1] = 0;
    m_worldCaptureMask = 0;
}

void FrameLoop::CaptureWorldBeforeHud(uint64_t pairSerial, int eye) {
    if (!pairSerial || eye < 0 || eye > 1 ||
        !OpenXRContext::Instance().WantsHudCapture() ||
        m_desktopTestMode.load(std::memory_order_acquire)) return;

    ID3D11Device* device = d3d11::GetGameDevice();
    ID3D11DeviceContext* context = d3d11::GetGameContext();
    IDXGISwapChain* swapChain = d3d11::GetGameSwapChain();
    if (!device || !context || !swapChain) return;

    ID3D11RenderTargetView* boundViews[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* boundDepth = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                boundViews, &boundDepth);
    auto releaseBindings = [&]() {
        for (auto*& view : boundViews) {
            if (view) {
                view->Release();
                view = nullptr;
            }
        }
        if (boundDepth) {
            boundDepth->Release();
            boundDepth = nullptr;
        }
    };
    if (!boundViews[0]) {
        releaseBindings();
        return;
    }
    ID3D11Resource* boundResource = nullptr;
    boundViews[0]->GetResource(&boundResource);
    ID3D11Texture2D* boundTexture = nullptr;
    if (boundResource) boundResource->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&boundTexture));

    ID3D11Texture2D* backbuffer = d3d11::AcquireCurrentBackbuffer(swapChain);
    IUnknown* boundIdentity = nullptr;
    IUnknown* backbufferIdentity = nullptr;
    if (boundResource) boundResource->QueryInterface(
        __uuidof(IUnknown), reinterpret_cast<void**>(&boundIdentity));
    if (backbuffer) backbuffer->QueryInterface(
        __uuidof(IUnknown), reinterpret_cast<void**>(&backbufferIdentity));
    const bool sameIdentity = boundIdentity && backbufferIdentity &&
        boundIdentity == backbufferIdentity;
    if (boundIdentity) boundIdentity->Release();
    if (backbufferIdentity) backbufferIdentity->Release();
    if (boundResource) boundResource->Release();

    D3D11_TEXTURE2D_DESC boundDesc = {}, backbufferDesc = {};
    if (boundTexture) boundTexture->GetDesc(&boundDesc);
    if (backbuffer) backbuffer->GetDesc(&backbufferDesc);
    DXGI_SWAP_CHAIN_DESC swapDesc = {};
    const bool swapDescValid = SUCCEEDED(swapChain->GetDesc(&swapDesc));
    const bool resourceDescMatches = boundTexture && backbuffer &&
        boundDesc.Width == backbufferDesc.Width &&
        boundDesc.Height == backbufferDesc.Height &&
        boundDesc.MipLevels == backbufferDesc.MipLevels &&
        boundDesc.ArraySize == backbufferDesc.ArraySize &&
        boundDesc.Format == backbufferDesc.Format &&
        boundDesc.SampleDesc.Count == backbufferDesc.SampleDesc.Count &&
        boundDesc.SampleDesc.Quality == backbufferDesc.SampleDesc.Quality;
    const bool swapDescMatches = swapDescValid &&
        (!swapDesc.BufferDesc.Width || swapDesc.BufferDesc.Width == backbufferDesc.Width) &&
        (!swapDesc.BufferDesc.Height || swapDesc.BufferDesc.Height == backbufferDesc.Height) &&
        (swapDesc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN ||
         swapDesc.BufferDesc.Format == backbufferDesc.Format);
    const bool rgbaSdr = backbufferDesc.SampleDesc.Count == 1 &&
        (backbufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         backbufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
         backbufferDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
         backbufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         backbufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
         backbufferDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
    if (boundTexture) boundTexture->Release();
    if (!sameIdentity || !resourceDescMatches || !swapDescMatches || !rgbaSdr) {
        if (backbuffer) backbuffer->Release();
        releaseBindings();
        return;
    }

    AcquireSRWLockExclusive(&m_captureLock);
    if (m_worldCapturePairSerial != pairSerial) {
        ResetHudCaptureMetadata();
        m_worldCapturePairSerial = pairSerial;
    }
    if (m_worldCaptureSerial[eye] == pairSerial) {
        ReleaseSRWLockExclusive(&m_captureLock);
        backbuffer->Release();
        releaseBindings();
        return;
    }
    bool recreate = !m_worldBeforeHudTextures[eye];
    if (!recreate) {
        D3D11_TEXTURE2D_DESC current = {};
        m_worldBeforeHudTextures[eye]->GetDesc(&current);
        recreate = current.Width != backbufferDesc.Width ||
            current.Height != backbufferDesc.Height ||
            current.Format != backbufferDesc.Format ||
            current.SampleDesc.Count != 1;
    }
    if (recreate) {
        if (m_worldBeforeHudTextures[eye]) {
            m_worldBeforeHudTextures[eye]->Release();
            m_worldBeforeHudTextures[eye] = nullptr;
        }
        D3D11_TEXTURE2D_DESC snapshotDesc = backbufferDesc;
        snapshotDesc.Usage = D3D11_USAGE_DEFAULT;
        snapshotDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        snapshotDesc.CPUAccessFlags = 0;
        snapshotDesc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(
                &snapshotDesc, nullptr, &m_worldBeforeHudTextures[eye]))) {
            m_worldCaptureSerial[eye] = 0;
            m_worldCaptureMask &= static_cast<uint8_t>(~(1u << eye));
        }
    }
    if (m_worldBeforeHudTextures[eye]) {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(m_worldBeforeHudTextures[eye], backbuffer);
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                    boundViews, boundDepth);
        m_worldCaptureSerial[eye] = pairSerial;
        m_worldCaptureMask |= static_cast<uint8_t>(1u << eye);
        static std::atomic<uint64_t> captureCount{0};
        const uint64_t count = captureCount.fetch_add(1) + 1;
        if (count <= 4 || count % 300 == 0) {
            Log("[HUD] World-before-HUD snapshot: pair=%llu eye=%d %ux%u count=%llu",
                static_cast<unsigned long long>(pairSerial), eye,
                backbufferDesc.Width, backbufferDesc.Height,
                static_cast<unsigned long long>(count));
        }
    }
    ReleaseSRWLockExclusive(&m_captureLock);
    backbuffer->Release();
    releaseBindings();
}

bool FrameLoop::EnsureHudExtractionTexture(ID3D11Device* device,
                                           ID3D11Texture2D* source) {
    if (!device || !source) return false;
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    source->GetDesc(&sourceDesc);
    if (sourceDesc.SampleDesc.Count != 1) return false;
    bool recreate = !m_hudExtractionTexture;
    if (!recreate) {
        D3D11_TEXTURE2D_DESC current = {};
        m_hudExtractionTexture->GetDesc(&current);
        recreate = current.Width != sourceDesc.Width ||
            current.Height != sourceDesc.Height || current.Format != sourceDesc.Format;
    }
    if (!recreate) return true;
    if (m_hudExtractionTexture) {
        m_hudExtractionTexture->Release();
        m_hudExtractionTexture = nullptr;
    }
    sourceDesc.Usage = D3D11_USAGE_DEFAULT;
    sourceDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    sourceDesc.CPUAccessFlags = 0;
    sourceDesc.MiscFlags = 0;
    return SUCCEEDED(device->CreateTexture2D(
        &sourceDesc, nullptr, &m_hudExtractionTexture));
}

bool FrameLoop::ValidateHudPair(ID3D11Device* device,
                                ID3D11DeviceContext* context,
                                ID3D11Texture2D* finalFrame,
                                ID3D11Texture2D* worldFrame,
                                uint64_t pairSerial) {
    if (m_hudWorldValidated) return true;
    if (!device || !context || !finalFrame || !worldFrame ||
        pairSerial < m_nextHudValidationPair) return false;

    D3D11_TEXTURE2D_DESC finalDesc = {}, worldDesc = {};
    finalFrame->GetDesc(&finalDesc);
    worldFrame->GetDesc(&worldDesc);
    const bool supportedFormat = finalDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        finalDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        finalDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        finalDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        finalDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        finalDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (!supportedFormat || finalDesc.Width != worldDesc.Width ||
        finalDesc.Height != worldDesc.Height || finalDesc.Format != worldDesc.Format ||
        finalDesc.SampleDesc.Count != 1 || worldDesc.SampleDesc.Count != 1) {
        m_nextHudValidationPair = pairSerial + 120;
        return false;
    }

    bool recreate = !m_hudValidationTexture || !m_hudValidationStaging;
    if (!recreate) {
        D3D11_TEXTURE2D_DESC current = {};
        m_hudValidationTexture->GetDesc(&current);
        recreate = current.Format != finalDesc.Format;
    }
    if (recreate) {
        if (m_hudValidationTexture) {
            m_hudValidationTexture->Release();
            m_hudValidationTexture = nullptr;
        }
        if (m_hudValidationStaging) {
            m_hudValidationStaging->Release();
            m_hudValidationStaging = nullptr;
        }
        D3D11_TEXTURE2D_DESC sampleDesc = {};
        sampleDesc.Width = 8;
        sampleDesc.Height = 4;
        sampleDesc.MipLevels = 1;
        sampleDesc.ArraySize = 1;
        sampleDesc.Format = finalDesc.Format;
        sampleDesc.SampleDesc.Count = 1;
        sampleDesc.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(device->CreateTexture2D(
                &sampleDesc, nullptr, &m_hudValidationTexture))) {
            m_nextHudValidationPair = pairSerial + 120;
            return false;
        }
        sampleDesc.Usage = D3D11_USAGE_STAGING;
        sampleDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(
                &sampleDesc, nullptr, &m_hudValidationStaging))) {
            m_hudValidationTexture->Release();
            m_hudValidationTexture = nullptr;
            m_nextHudValidationPair = pairSerial + 120;
            return false;
        }
    }

    for (UINT y = 0; y < 4; ++y) {
        for (UINT x = 0; x < 4; ++x) {
            constexpr UINT sampleNumerators[4] = {1, 3, 4, 7};
            const UINT sourceX = (std::min)(finalDesc.Width - 1,
                (sampleNumerators[x] * finalDesc.Width) / 8);
            const UINT sourceY = (std::min)(finalDesc.Height - 1,
                (sampleNumerators[y] * finalDesc.Height) / 8);
            const D3D11_BOX sample = {
                sourceX, sourceY, 0, sourceX + 1, sourceY + 1, 1};
            context->CopySubresourceRegion(
                m_hudValidationTexture, 0, x, y, 0, worldFrame, 0, &sample);
            context->CopySubresourceRegion(
                m_hudValidationTexture, 0, x + 4, y, 0, finalFrame, 0, &sample);
        }
    }
    context->CopyResource(m_hudValidationStaging, m_hudValidationTexture);
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(
            m_hudValidationStaging, 0, D3D11_MAP_READ, 0, &mapped))) {
        m_nextHudValidationPair = pairSerial + 120;
        return false;
    }

    float worldAverage = 0.0f;
    float finalAverage = 0.0f;
    float differenceAverage = 0.0f;
    unsigned changedSamples = 0;
    for (UINT y = 0; y < 4; ++y) {
        const auto* row = static_cast<const unsigned char*>(mapped.pData) +
            static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < 4; ++x) {
            const unsigned char* world = row + static_cast<size_t>(x) * 4;
            const unsigned char* final = row + static_cast<size_t>(x + 4) * 4;
            float sampleDifference = 0.0f;
            for (int channel = 0; channel < 3; ++channel) {
                worldAverage += world[channel] / 255.0f;
                finalAverage += final[channel] / 255.0f;
                sampleDifference += std::abs(
                    static_cast<int>(world[channel]) - static_cast<int>(final[channel])) /
                    255.0f;
            }
            differenceAverage += sampleDifference / 3.0f;
            if (sampleDifference / 3.0f > 0.08f) ++changedSamples;
        }
    }
    context->Unmap(m_hudValidationStaging, 0);
    worldAverage /= 48.0f;
    finalAverage /= 48.0f;
    differenceAverage /= 16.0f;
    const bool valid = worldAverage >= 0.015f && finalAverage >= 0.015f &&
        differenceAverage >= 0.0002f && differenceAverage <= 0.30f &&
        changedSamples >= 1 && changedSamples <= 12;
    Log("[HUD] Pre-HUD validation: pair=%llu world=%.4f final=%.4f "
        "difference=%.4f changed=%u/16 valid=%d",
        static_cast<unsigned long long>(pairSerial), worldAverage, finalAverage,
        differenceAverage, changedSamples, valid ? 1 : 0);
    if (valid) {
        m_hudWorldValidated = true;
    } else {
        m_nextHudValidationPair = pairSerial + 120;
    }
    return valid;
}

bool FrameLoop::CompositeHudIntoProjection(ID3D11DeviceContext* context,
                                           ID3D11Texture2D* hudTexture,
                                           ID3D11Texture2D* target, int eye) {
    if (!context || !hudTexture || !target || eye < 0 || eye > 1 ||
        !m_submissionViewsValid) return false;
    D3D11_TEXTURE2D_DESC sourceDesc = {}, targetDesc = {};
    hudTexture->GetDesc(&sourceDesc);
    target->GetDesc(&targetDesc);
    if (!sourceDesc.Width || !sourceDesc.Height ||
        !targetDesc.Width || !targetDesc.Height) return false;

    const XrView& eyeView = m_submissionViews[eye];
    const float tanLeft = tanf(eyeView.fov.angleLeft);
    const float tanRight = tanf(eyeView.fov.angleRight);
    const float runtimeTanUp = tanf(eyeView.fov.angleUp);
    const float runtimeTanDown = tanf(eyeView.fov.angleDown);
    const float verticalHalfSpan = (runtimeTanUp - runtimeTanDown) * 0.5f;
    const float horizontalSpan = tanRight - tanLeft;
    const float verticalSpan = verticalHalfSpan * 2.0f;
    if (horizontalSpan <= 0.0f || verticalSpan <= 0.0f) return false;

    const auto& hud = config::Get();
    constexpr float kDegreesToRadians = 0.01745329251994329577f;
    const float angularWidth = (std::min)(150.0f, hud.hud_width_degrees) *
        kDegreesToRadians;
    float viewportWidth = static_cast<float>(targetDesc.Width) *
        (2.0f * tanf(angularWidth * 0.5f) * hud.hud_scale) / horizontalSpan;
    float viewportHeight = viewportWidth * static_cast<float>(sourceDesc.Height) /
        static_cast<float>(sourceDesc.Width);

    const XrVector3f centerPosition = {
        (m_submissionViews[0].pose.position.x +
         m_submissionViews[1].pose.position.x) * 0.5f,
        (m_submissionViews[0].pose.position.y +
         m_submissionViews[1].pose.position.y) * 0.5f,
        (m_submissionViews[0].pose.position.z +
         m_submissionViews[1].pose.position.z) * 0.5f};
    const float eyeDelta[3] = {
        eyeView.pose.position.x - centerPosition.x,
        eyeView.pose.position.y - centerPosition.y,
        eyeView.pose.position.z - centerPosition.z};
    const XrQuaternionf& orientation = eyeView.pose.orientation;
    const float inverse[4] = {
        -orientation.x, -orientation.y, -orientation.z, orientation.w};
    auto rotate = [](const float quaternion[4], const float vector[3], float output[3]) {
        const float tx = 2.0f * (quaternion[1] * vector[2] - quaternion[2] * vector[1]);
        const float ty = 2.0f * (quaternion[2] * vector[0] - quaternion[0] * vector[2]);
        const float tz = 2.0f * (quaternion[0] * vector[1] - quaternion[1] * vector[0]);
        output[0] = vector[0] + quaternion[3] * tx +
            (quaternion[1] * tz - quaternion[2] * ty);
        output[1] = vector[1] + quaternion[3] * ty +
            (quaternion[2] * tx - quaternion[0] * tz);
        output[2] = vector[2] + quaternion[3] * tz +
            (quaternion[0] * ty - quaternion[1] * tx);
    };
    float localEye[3] = {};
    rotate(inverse, eyeDelta, localEye);
    const float distance = (std::max)(0.5f, hud.hud_distance);
    const float projectedX = (hud.hud_horizontal_offset - localEye[0]) / distance;
    const float projectedY = (hud.hud_vertical_offset - localEye[1]) / distance;
    const float centerU = (projectedX - tanLeft) / horizontalSpan;
    const float centerV = (verticalHalfSpan - projectedY) / verticalSpan;
    const float centerX = centerU * targetDesc.Width;
    const float centerY = centerV * targetDesc.Height;
    if (centerX <= 0.0f || centerX >= targetDesc.Width ||
        centerY <= 0.0f || centerY >= targetDesc.Height) return false;

    const float availableWidth = 2.0f * (std::min)(
        centerX, static_cast<float>(targetDesc.Width) - centerX);
    const float availableHeight = 2.0f * (std::min)(
        centerY, static_cast<float>(targetDesc.Height) - centerY);
    const float fitScale = (std::min)(1.0f, (std::min)(
        availableWidth / viewportWidth, availableHeight / viewportHeight));
    viewportWidth *= fitScale;
    viewportHeight *= fitScale;
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = centerX - viewportWidth * 0.5f;
    viewport.TopLeftY = centerY - viewportHeight * 0.5f;
    viewport.Width = viewportWidth;
    viewport.Height = viewportHeight;
    viewport.MaxDepth = 1.0f;

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    const bool composed = device && render::HudBlitter::Instance().Composite(
        device, context, hudTexture, target, viewport, hud.hud_opacity,
        m_lastDotSettings[eye],
        static_cast<float>(targetDesc.Width) / targetDesc.Height);
    if (device) device->Release();
    if (composed) {
        static std::atomic<uint64_t> composeCount{0};
        const uint64_t count = composeCount.fetch_add(1) + 1;
        if (count <= 2 || count % 600 == 0) {
            Log("[HUD] Projection-baked HUD: eye=%d viewport=(%.0f,%.0f %.0fx%.0f) "
                "distance=%.2f count=%llu", eye, viewport.TopLeftX, viewport.TopLeftY,
                viewport.Width, viewport.Height, distance,
                static_cast<unsigned long long>(count));
        }
    }
    return composed;
}

bool FrameLoop::TrySubmitTheaterFrame(ID3D11Device* device,
                                      ID3D11DeviceContext* context,
                                      IDXGISwapChain* swapChain) {
    auto& xr = OpenXRContext::Instance();
    if (!device || !context || !swapChain || !xr.IsInitialized() || !m_poseSeeded)
        return false;

    StartWaitWorker();
    bool waitReady = m_waitReadyEvent &&
        WaitForSingleObject(m_waitReadyEvent, 250) == WAIT_OBJECT_0;
    if (!waitReady) return false;

    bool submitted = false;
    if (!xr.BeginFrame()) {
        if (m_waitRequestEvent) SetEvent(m_waitRequestEvent);
        return false;
    }
    if (!xr.ShouldRender() || !xr.LocateViews()) {
        xr.EndFrame(false);
        if (m_waitRequestEvent) SetEvent(m_waitRequestEvent);
        return false;
    }

    ID3D11Texture2D* backbuffer = d3d11::AcquireCurrentBackbuffer(swapChain);
    if (backbuffer) {
        D3D11_TEXTURE2D_DESC desc = {};
        backbuffer->GetDesc(&desc);
        if (desc.Width && desc.Height && desc.SampleDesc.Count == 1) {
            const bool copied = CopyTextureToEye(
                context, backbuffer, 0, false, -1, nullptr, true);
            if (copied) {
                context->Flush();
                submitted = xr.EndFrameTheater(
                    static_cast<float>(desc.Width) / desc.Height);
            }
        }
        backbuffer->Release();
    }
    if (xr.IsFrameActive()) xr.EndFrame(false);
    if (m_waitRequestEvent) SetEvent(m_waitRequestEvent);
    return submitted;
}

bool FrameLoop::ConsumeRenderedTicket(StereoRenderTicket& ticket) {
    ticket = {};
    AcquireSRWLockExclusive(&m_stereoPairLock);
    if (m_renderedTicketCount != 0) {
        ticket = m_renderedTicketQueue[m_renderedTicketRead];
        m_renderedTicketQueue[m_renderedTicketRead] = {};
        m_renderedTicketRead = (m_renderedTicketRead + 1) % _countof(m_renderedTicketQueue);
        --m_renderedTicketCount;
    }
    ReleaseSRWLockExclusive(&m_stereoPairLock);
    return ticket.valid;
}

void FrameLoop::ResetStereoPair() {
    AcquireSRWLockExclusive(&m_stereoPairLock);
    m_nextRenderEye = 0;
    m_activePair = {};
    m_renderedTicketRead = 0;
    m_renderedTicketWrite = 0;
    m_renderedTicketCount = 0;
    for (auto& ticket : m_renderedTicketQueue) ticket = {};
    ReleaseSRWLockExclusive(&m_stereoPairLock);
    AcquireSRWLockExclusive(&m_captureLock);
    m_capturePairSerial = 0;
    m_eyeCaptureSerial[0] = m_eyeCaptureSerial[1] = 0;
    m_eyeCaptureWasBackbuffer[0] = m_eyeCaptureWasBackbuffer[1] = false;
    m_eyeCaptureMask = 0;
    m_captureWidth = m_captureHeight = m_captureSamples = 0;
    m_captureFormat = DXGI_FORMAT_UNKNOWN;
    m_submissionRightAimValid = false;
    m_capturePairViews[0] = {XR_TYPE_VIEW};
    m_capturePairViews[1] = {XR_TYPE_VIEW};
    ResetHudCaptureMetadata();
    ReleaseSRWLockExclusive(&m_captureLock);
}

bool FrameLoop::BeginSequentialRender() {
    auto& xr = OpenXRContext::Instance();
    if (!config::Get().same_frame_stereo || !m_initialized || !m_vrActive ||
        !xr.IsInitialized() || !xr.IsFrameActive() || !xr.ShouldRender() ||
        !d3d11::GetGameDevice() || !d3d11::GetGameContext() ||
        !d3d11::GetGameSwapChain() || m_sequentialFramePending.load()) {
        return false;
    }
    m_sequentialCaptureMask = 0;
    m_sequentialCaptureFailed = false;
    return true;
}

void FrameLoop::SetSequentialRenderEye(int eye) {
    m_sequentialRenderEye = eye;
    g_currentEye = eye;
}

bool FrameLoop::CaptureSequentialEye(int eye) {
    if (eye < 0 || eye > 1) return false;
    auto* device = d3d11::GetGameDevice();
    auto* context = d3d11::GetGameContext();
    auto* swapChain = d3d11::GetGameSwapChain();
    if (!device || !context || !swapChain) return false;

    ID3D11Texture2D* backbuffer = nullptr;
    backbuffer = d3d11::AcquireCurrentBackbuffer(swapChain);
    if (!backbuffer) {
        m_sequentialCaptureFailed = true;
        return false;
    }
    EnsureEyeTextures(device, backbuffer);
    if (m_eyeTextures[eye]) {
        context->CopyResource(m_eyeTextures[eye], backbuffer);
        m_sequentialCaptureMask.fetch_or(static_cast<uint8_t>(1u << eye));
    } else {
        m_sequentialCaptureFailed = true;
    }
    backbuffer->Release();
    return m_eyeTextures[eye] != nullptr;
}

void FrameLoop::FinishSequentialRender() {
    m_sequentialRenderEye = -1;
    g_currentEye = -1;
    m_sequentialFramePending = true;
}

void FrameLoop::ToggleDesktopTestMode() {
    const bool enabled = !m_desktopTestMode.load();
    m_desktopTestMode = enabled;
    if (enabled) {
        g_desktopTestMode = true;
        AcquireSRWLockExclusive(&m_desktopPoseLock);
        m_desktopYaw = 0;
        m_desktopPitch = 0;
        m_desktopRoll = 0;
        memset(m_desktopPosition, 0, sizeof(m_desktopPosition));
        ReleaseSRWLockExclusive(&m_desktopPoseLock);
        Log("[FrameLoop] Desktop pose simulation ENABLED (arrows=yaw/pitch, "
            "PgUp/PgDn=roll, numpad=move, Home=reset, F6=toggle)");
    } else {
        g_desktopTestMode = false;
        Log("[FrameLoop] Desktop test mode DISABLED");
    }
}

void FrameLoop::UpdateDesktopControls() {
    if (!m_desktopTestMode.load()) return;

    AcquireSRWLockExclusive(&m_desktopPoseLock);
    const float rotSpeed = 0.02f;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) m_desktopYaw -= rotSpeed;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) m_desktopYaw += rotSpeed;
    if (GetAsyncKeyState(VK_UP) & 0x8000) m_desktopPitch -= rotSpeed;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) m_desktopPitch += rotSpeed;
    if (GetAsyncKeyState(VK_PRIOR) & 0x8000) m_desktopRoll -= rotSpeed;
    if (GetAsyncKeyState(VK_NEXT) & 0x8000) m_desktopRoll += rotSpeed;

    // Position is expressed in meters, matching OpenXR.
    const float moveSpeed = 0.01f;
    if (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) m_desktopPosition[0] -= moveSpeed;
    if (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) m_desktopPosition[0] += moveSpeed;
    if (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) m_desktopPosition[2] -= moveSpeed;
    if (GetAsyncKeyState(VK_NUMPAD2) & 0x8000) m_desktopPosition[2] += moveSpeed;
    if (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) m_desktopPosition[1] += moveSpeed;
    if (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) m_desktopPosition[1] -= moveSpeed;

    // Home to reset
    if (GetAsyncKeyState(VK_HOME) & 0x8000) {
        m_desktopYaw = 0;
        m_desktopPitch = 0;
        m_desktopRoll = 0;
        memset(m_desktopPosition, 0, sizeof(m_desktopPosition));
    }
    ReleaseSRWLockExclusive(&m_desktopPoseLock);

    // F7 to capture
    static bool f7WasDown = false;
    bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7Down && !f7WasDown) {
        SaveStereoCapture();
    }
    f7WasDown = f7Down;

}

void FrameLoop::GetDesktopHeadPose(float position[3], float rotation[4]) const {
    AcquireSRWLockShared(&m_desktopPoseLock);
    memcpy(position, m_desktopPosition, sizeof(m_desktopPosition));

    const float halfYaw = m_desktopYaw * 0.5f;
    const float halfPitch = m_desktopPitch * 0.5f;
    const float halfRoll = m_desktopRoll * 0.5f;
    const float cy = cosf(halfYaw), sy = sinf(halfYaw);
    const float cp = cosf(halfPitch), sp = sinf(halfPitch);
    const float cr = cosf(halfRoll), sr = sinf(halfRoll);

    // OpenXR basis: +X right, +Y up, -Z forward. Compose yaw, pitch,
    // then roll to emulate an HMD orientation in the same representation.
    rotation[0] = cy * sp * cr + sy * cp * sr;
    rotation[1] = sy * cp * cr - cy * sp * sr;
    rotation[2] = cy * cp * sr - sy * sp * cr;
    rotation[3] = cy * cp * cr + sy * sp * sr;
    ReleaseSRWLockShared(&m_desktopPoseLock);
}

void FrameLoop::SaveStereoCapture() {
    ID3D11Device* device = d3d11::GetGameDevice();
    ID3D11DeviceContext* context = d3d11::GetGameContext();
    if (!device || !context || !m_eyeTextures[0] || !m_eyeTextures[1]) {
        Log("[DesktopStereo] BMP capture unavailable: eye textures are not ready");
        return;
    }

    D3D11_TEXTURE2D_DESC desc[2] = {};
    m_eyeTextures[0]->GetDesc(&desc[0]);
    m_eyeTextures[1]->GetDesc(&desc[1]);
    const bool matching = desc[0].Width == desc[1].Width &&
        desc[0].Height == desc[1].Height && desc[0].Format == desc[1].Format &&
        desc[0].SampleDesc.Count == 1 && desc[1].SampleDesc.Count == 1;
    const bool rgba = desc[0].Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc[0].Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        desc[0].Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool bgra = desc[0].Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        desc[0].Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        desc[0].Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (!matching || (!rgba && !bgra)) {
        Log("[DesktopStereo] BMP capture unsupported: left=%ux%u fmt=%u right=%ux%u fmt=%u",
            desc[0].Width, desc[0].Height, desc[0].Format,
            desc[1].Width, desc[1].Height, desc[1].Format);
        return;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc[0];
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ID3D11Texture2D* staging[2] = {};
    HRESULT result = device->CreateTexture2D(&stagingDesc, nullptr, &staging[0]);
    if (SUCCEEDED(result))
        result = device->CreateTexture2D(&stagingDesc, nullptr, &staging[1]);
    if (FAILED(result)) {
        if (staging[0]) staging[0]->Release();
        if (staging[1]) staging[1]->Release();
        Log("[DesktopStereo] BMP staging texture creation failed: 0x%08X", result);
        return;
    }

    context->CopyResource(staging[0], m_eyeTextures[0]);
    context->CopyResource(staging[1], m_eyeTextures[1]);
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped[2] = {};
    const HRESULT leftMap = context->Map(staging[0], 0, D3D11_MAP_READ, 0, &mapped[0]);
    const HRESULT rightMap = context->Map(staging[1], 0, D3D11_MAP_READ, 0, &mapped[1]);
    if (FAILED(leftMap) || FAILED(rightMap)) {
        if (SUCCEEDED(rightMap)) context->Unmap(staging[1], 0);
        if (SUCCEEDED(leftMap)) context->Unmap(staging[0], 0);
        staging[1]->Release();
        staging[0]->Release();
        Log("[DesktopStereo] BMP staging map failed: left=0x%08X right=0x%08X",
            leftMap, rightMap);
        return;
    }

    const uint32_t outputWidth = desc[0].Width * 2u;
    const uint32_t rowBytes = (outputWidth * 3u + 3u) & ~3u;
    const uint64_t pixelBytes64 = static_cast<uint64_t>(rowBytes) * desc[0].Height;
    if (pixelBytes64 > MAXDWORD - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER)) {
        context->Unmap(staging[1], 0);
        context->Unmap(staging[0], 0);
        staging[1]->Release();
        staging[0]->Release();
        Log("[DesktopStereo] BMP capture too large");
        return;
    }
    std::vector<unsigned char> pixels(static_cast<size_t>(pixelBytes64), 0);
    for (uint32_t y = 0; y < desc[0].Height; ++y) {
        unsigned char* output = pixels.data() + static_cast<size_t>(y) * rowBytes;
        for (int eye = 0; eye < 2; ++eye) {
            const auto* input = static_cast<const unsigned char*>(mapped[eye].pData) +
                static_cast<size_t>(y) * mapped[eye].RowPitch;
            unsigned char* eyeOutput = output + static_cast<size_t>(eye) * desc[0].Width * 3u;
            for (uint32_t x = 0; x < desc[0].Width; ++x) {
                const unsigned char* source = input + static_cast<size_t>(x) * 4u;
                eyeOutput[x * 3u + 0u] = rgba ? source[2] : source[0];
                eyeOutput[x * 3u + 1u] = source[1];
                eyeOutput[x * 3u + 2u] = rgba ? source[0] : source[2];
            }
        }
    }
    context->Unmap(staging[1], 0);
    context->Unmap(staging[0], 0);
    staging[1]->Release();
    staging[0]->Release();

    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    strcpy_s(slash ? slash + 1 : path, slash ? MAX_PATH - static_cast<size_t>(slash + 1 - path) : MAX_PATH,
             "BL1GOTYVR_stereo.bmp");

    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};
    const DWORD pixelBytes = static_cast<DWORD>(pixelBytes64);
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = static_cast<LONG>(outputWidth);
    infoHeader.biHeight = -static_cast<LONG>(desc[0].Height);
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 24;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = pixelBytes;

    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool saved = file != INVALID_HANDLE_VALUE &&
        WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr) &&
        written == sizeof(fileHeader) &&
        WriteFile(file, &infoHeader, sizeof(infoHeader), &written, nullptr) &&
        written == sizeof(infoHeader) &&
        WriteFile(file, pixels.data(), pixelBytes, &written, nullptr) &&
        written == pixelBytes;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Log(saved ? "[DesktopStereo] SBS BMP saved: %s (%ux%u)" :
        "[DesktopStereo] SBS BMP save failed: %s", path, outputWidth, desc[0].Height);
}

void FrameLoop::ValidateDesktopStereoPair(ID3D11Device* device,
                                           ID3D11DeviceContext* context) {
    if (!device || !context || !m_eyeTextures[0] || !m_eyeTextures[1]) return;
    if (m_desktopPairSerial > 5 && m_desktopPairSerial % 60 != 0) return;

    D3D11_TEXTURE2D_DESC leftDesc = {};
    D3D11_TEXTURE2D_DESC rightDesc = {};
    m_eyeTextures[0]->GetDesc(&leftDesc);
    m_eyeTextures[1]->GetDesc(&rightDesc);
    if (leftDesc.Width != rightDesc.Width || leftDesc.Height != rightDesc.Height ||
        leftDesc.Format != rightDesc.Format || leftDesc.SampleDesc.Count != 1 ||
        rightDesc.SampleDesc.Count != 1) return;
    const bool fourByteColor =
        leftDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        leftDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        leftDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        leftDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!fourByteColor) {
        Log("[DesktopStereo] Pair validation unsupported for format=%u", leftDesc.Format);
        return;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = leftDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ID3D11Texture2D* staging[2] = {};
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging[0])) ||
        FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging[1]))) {
        if (staging[0]) staging[0]->Release();
        if (staging[1]) staging[1]->Release();
        return;
    }
    context->CopyResource(staging[0], m_eyeTextures[0]);
    context->CopyResource(staging[1], m_eyeTextures[1]);
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped[2] = {};
    const HRESULT leftMap = context->Map(staging[0], 0, D3D11_MAP_READ, 0, &mapped[0]);
    const HRESULT rightMap = context->Map(staging[1], 0, D3D11_MAP_READ, 0, &mapped[1]);
    if (SUCCEEDED(leftMap) && SUCCEEDED(rightMap)) {
        const UINT stepX = (std::max)(1u, leftDesc.Width / 96u);
        const UINT stepY = (std::max)(1u, leftDesc.Height / 96u);
        uint64_t samples = 0;
        uint64_t changed = 0;
        uint64_t totalRgbDelta = 0;
        for (UINT y = stepY / 2; y < leftDesc.Height; y += stepY) {
            const auto* leftRow = static_cast<const unsigned char*>(mapped[0].pData) +
                static_cast<size_t>(y) * mapped[0].RowPitch;
            const auto* rightRow = static_cast<const unsigned char*>(mapped[1].pData) +
                static_cast<size_t>(y) * mapped[1].RowPitch;
            for (UINT x = stepX / 2; x < leftDesc.Width; x += stepX) {
                const unsigned char* leftPixel = leftRow + static_cast<size_t>(x) * 4;
                const unsigned char* rightPixel = rightRow + static_cast<size_t>(x) * 4;
                const unsigned delta =
                    static_cast<unsigned>(abs(static_cast<int>(leftPixel[0]) - rightPixel[0])) +
                    static_cast<unsigned>(abs(static_cast<int>(leftPixel[1]) - rightPixel[1])) +
                    static_cast<unsigned>(abs(static_cast<int>(leftPixel[2]) - rightPixel[2]));
                totalRgbDelta += delta;
                if (delta > 6) ++changed;
                ++samples;
            }
        }
        const double changedPercent = samples
            ? static_cast<double>(changed) * 100.0 / static_cast<double>(samples) : 0.0;
        const double meanRgbDelta = samples
            ? static_cast<double>(totalRgbDelta) / (static_cast<double>(samples) * 3.0) : 0.0;
        Log("[DesktopStereo] pair=%llu samples=%llu changed=%.2f%% meanRgbDelta=%.3f %s",
            static_cast<unsigned long long>(m_desktopPairSerial),
            static_cast<unsigned long long>(samples), changedPercent, meanRgbDelta,
            changedPercent > 0.5 ? "STEREO_CONTENT" : "POSSIBLE_DUPLICATE");
    }
    if (SUCCEEDED(rightMap)) context->Unmap(staging[1], 0);
    if (SUCCEEDED(leftMap)) context->Unmap(staging[0], 0);
    staging[1]->Release();
    staging[0]->Release();
}

void FrameLoop::OnDesktopPresent(ID3D11Device* device, ID3D11DeviceContext* context,
                                 IDXGISwapChain* swapChain) {
    if (!camera::IsCameraFound()) {
        m_desktopCaptureMask = 0;
        ++m_frameCount;
        return;
    }
    ID3D11Texture2D* backbuffer = d3d11::AcquireCurrentBackbuffer(swapChain);
    if (!backbuffer) return;
    if (EnsureEyeTextures(device, backbuffer, false)) m_desktopCaptureMask = 0;

    const int eye = GetRenderEye();
    ID3D11RenderTargetView* boundTarget = nullptr;
    ID3D11DepthStencilView* boundDepth = nullptr;
    context->OMGetRenderTargets(1, &boundTarget, &boundDepth);
    bool backbufferBound = false;
    if (boundTarget) {
        ID3D11Resource* resource = nullptr;
        boundTarget->GetResource(&resource);
        backbufferBound = resource == backbuffer;
        if (resource) resource->Release();
    }
    if (backbufferBound) context->OMSetRenderTargets(0, nullptr, nullptr);

    D3D11_TEXTURE2D_DESC desc = {};
    backbuffer->GetDesc(&desc);
    bool captured = m_eyeTextures[eye] != nullptr;
    if (captured && desc.SampleDesc.Count > 1) {
        DXGI_FORMAT resolveFormat = desc.Format;
        if (resolveFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS)
            resolveFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (resolveFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS)
            resolveFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        context->ResolveSubresource(m_eyeTextures[eye], 0, backbuffer, 0, resolveFormat);
    } else if (captured) {
        context->CopyResource(m_eyeTextures[eye], backbuffer);
    }
    if (backbufferBound) context->OMSetRenderTargets(1, &boundTarget, boundDepth);
    if (boundTarget) boundTarget->Release();
    if (boundDepth) boundDepth->Release();
    backbuffer->Release();

    if (eye == 0) m_desktopCaptureMask = 0;
    if (captured) m_desktopCaptureMask |= static_cast<uint8_t>(1u << eye);
    if (m_desktopCaptureMask == 0x3u) {
        ++m_desktopPairSerial;
        ValidateDesktopStereoPair(device, context);
        m_desktopCaptureMask = 0;
    }
    ++m_frameCount;
    g_currentEye = eye;
}

void FrameLoop::OnPresent(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) {
    if (!m_initialized) return;

    // Poll for command file changes (runtime debugging)
    CommandSystem::Instance().PollCommandFile();

    if (m_desktopTestMode.load()) {
        OnDesktopPresent(device, context, swapChain);
        return;
    }

    auto& xr = OpenXRContext::Instance();
    if (m_poseSeeded && xr.GetPredictedDisplayTime() != 0)
        input::InputHook::Instance().UpdateState(xr.GetPredictedDisplayTime());
    StereoRenderTicket renderedTicket = {};
    const bool hasRenderedTicket = ConsumeRenderedTicket(renderedTicket);
    if (hasRenderedTicket) m_missingTicketPresents = 0;

    if (hasRenderedTicket && !camera::ConsumeRenderPoseAcknowledgement(
            renderedTicket.pairSerial, renderedTicket.eye)) {
        static uint64_t lateAcknowledgements = 0;
        ++lateAcknowledgements;
        if (lateAcknowledgements <= 10 || lateAcknowledgements % 300 == 0) {
            Log("[FrameLoop] Late FSceneView acknowledgement: serial=%llu eye=%d "
                "continuing causal capture (count=%llu)",
                static_cast<unsigned long long>(renderedTicket.pairSerial), renderedTicket.eye,
                static_cast<unsigned long long>(lateAcknowledgements));
        }
    }

    // Seed one pose before the first pair. After that, a missing camera tag
    // leaves the last complete pair with the compositor instead of submitting
    // a zero-layer frame that flashes the environment or black.
    if (!hasRenderedTicket) {
        ++m_missingTicketPresents;
        xr.PollSessionEvents();
        if (!m_poseSeeded && xr.WaitForFrame() && xr.BeginFrame()) {
            input::InputHook::Instance().UpdateState(xr.GetPredictedDisplayTime());
            m_poseSeeded = xr.ShouldRender() && xr.LocateViews();
            xr.EndFrame(false);
            StartWaitWorker();
        }
        constexpr uint32_t kTheaterEntryDelay = 30;
        if (m_poseSeeded && m_hasSubmittedStereoProjection &&
            m_missingTicketPresents >= kTheaterEntryDelay) {
            const bool submitted = TrySubmitTheaterFrame(device, context, swapChain);
            if (submitted && (m_missingTicketPresents == kTheaterEntryDelay ||
                              m_missingTicketPresents % 300 == 0)) {
                Log("[FrameLoop] Loading/cinematic theater submitted "
                    "(missing tickets=%u)", m_missingTicketPresents);
            }
        }
        ++m_frameCount;
        g_currentEye = -1;
        return;
    }

    // Get the swapchain backbuffer and inspect the render target left bound by
    // UE3. Gameplay can remain in an intermediate target while menus are
    // composed directly into the backbuffer.
    ID3D11Texture2D* backbuffer = nullptr;
    backbuffer = d3d11::AcquireCurrentBackbuffer(swapChain);
    if (!backbuffer) {
        ResetStereoPair();
        return;
    }

    ID3D11Texture2D* captureSource = backbuffer;
    D3D11_TEXTURE2D_DESC backDesc = {};
    backbuffer->GetDesc(&backDesc);
    // RenderDoc confirms that UE3 finishes world and UI composition in the
    // SDR swapchain backbuffer. Capture it directly before Present.
    static int captureMode = 1;
    static int trackedTargetIndex = 0;
    static bool f8WasDown = false;
    const bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8Down && !f8WasDown) {
        captureMode = (captureMode + 1) % 4;
        Log("[FrameLoop] Capture mode changed: %d - %s", captureMode,
            captureMode == 0 ? "composed source" :
            captureMode == 1 ? "internal SDR backbuffer" :
            captureMode == 2 ? "tracked SDR target" : "GDI window");
    }
    f8WasDown = f8Down;
    static bool f10WasDown = false;
    const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10Down && !f10WasDown) {
        const int count = d3d11::GetTrackedRenderTargetCount();
        if (count > 0) trackedTargetIndex = (trackedTargetIndex + 1) % count;
        Log("[FrameLoop] Tracked render target selected: %d/%d", trackedTargetIndex, count);
    }
    f10WasDown = f10Down;
    static bool f9WasDown = false;
    const bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9Down && !f9WasDown) {
        Log("[FrameLoop] Projection correction remains enabled; coherent stereo requires "
            "matching render and submission FOVs");
    }
    f9WasDown = f9Down;
    static bool f11WasDown = false;
    const bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f11Down && !f11WasDown) {
        m_gdiLatencyCorrection = !m_gdiLatencyCorrection;
        OpenXRContext::Instance().SetUseRenderedViewPoses(m_gdiLatencyCorrection);
        Log("[FrameLoop] GDI pose latency correction %s",
            m_gdiLatencyCorrection ? "ENABLED" : "disabled");
    }
    f11WasDown = f11Down;
    static uint64_t composedSeen = 0;
    static uint64_t composedAccepted = 0;
    static uint64_t composedRejectedDevice = 0;
    static uint64_t composedRejectedDesc = 0;
    static uint64_t tonemapSeen = 0;
    static uint64_t tonemapAccepted = 0;
    static uint64_t hookFiredCount = 0;
    ID3D11Texture2D* composedTexture = d3d11::GetLatestComposedTexture();
    // Log hook activity every 300 frames to diagnose capture pipeline
    if (m_frameCount > 0 && m_frameCount % 300 == 0) {
        hookFiredCount = d3d11::GetHookFiredCount();
        Log("[FrameLoop] frame=%llu captureMode=%d composed=%llu/%llu hookFired=%llu "
            "pair=%llu eye=%d", m_frameCount, captureMode, composedAccepted, composedSeen,
            hookFiredCount, static_cast<unsigned long long>(renderedTicket.pairSerial),
            renderedTicket.eye);
    }
    if (composedTexture) {
        ++composedSeen;
        D3D11_TEXTURE2D_DESC composedDesc = {};
        composedTexture->GetDesc(&composedDesc);
        ID3D11Device* composedDevice = nullptr;
        composedTexture->GetDevice(&composedDevice);
        const bool sameDevice = composedDevice == device;
        if (composedDevice) composedDevice->Release();
        const bool knownColorFormat =
            composedDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            composedDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            composedDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
            composedDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            composedDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
            composedDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
            composedDesc.Format == DXGI_FORMAT_B8G8R8X8_UNORM ||
            composedDesc.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
            composedDesc.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS ||
            composedDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
            composedDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
            composedDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const bool validDesc = composedDesc.Width == backDesc.Width &&
                               composedDesc.Height == backDesc.Height &&
                               composedDesc.SampleDesc.Count >= 1 && knownColorFormat;
        if (captureMode == 0 && sameDevice && validDesc) {
            captureSource = composedTexture;
            ++composedAccepted;
        } else {
            if (!sameDevice) ++composedRejectedDevice;
            else ++composedRejectedDesc;
            composedTexture->Release();
            composedTexture = nullptr;
        }
    }
    ID3D11Texture2D* desktopTexture = nullptr;
    ID3D11Texture2D* latestSdrTexture = captureMode == 2
        ? d3d11::GetLatestSdrRenderTarget() : nullptr;
    if (latestSdrTexture) {
        D3D11_TEXTURE2D_DESC sdrDesc = {};
        latestSdrTexture->GetDesc(&sdrDesc);
        ID3D11Device* sdrDevice = nullptr;
        latestSdrTexture->GetDevice(&sdrDevice);
        const bool knownSdrFormat =
            sdrDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            sdrDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            sdrDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
            sdrDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            sdrDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
            sdrDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        const bool validSdr = sdrDevice == device && sdrDesc.Width == backDesc.Width &&
                              sdrDesc.Height == backDesc.Height &&
                              sdrDesc.SampleDesc.Count == 1 && knownSdrFormat;
        if (sdrDevice) sdrDevice->Release();
        if (validSdr) captureSource = latestSdrTexture;
        else {
            latestSdrTexture->Release();
            latestSdrTexture = nullptr;
        }
    }
    ID3D11Texture2D* sdrTexture = captureMode == 2 && captureSource == backbuffer
        ? d3d11::GetTrackedRenderTarget(trackedTargetIndex) : nullptr;
    if (sdrTexture) {
        D3D11_TEXTURE2D_DESC sdrDesc = {};
        sdrTexture->GetDesc(&sdrDesc);
        ID3D11Device* sdrDevice = nullptr;
        sdrTexture->GetDevice(&sdrDevice);
        const bool validSdr = captureMode == 2 && captureSource == backbuffer &&
                              sdrDevice == device && sdrDesc.Width == backDesc.Width &&
                              sdrDesc.Height == backDesc.Height && sdrDesc.SampleDesc.Count == 1;
        if (sdrDevice) sdrDevice->Release();
        if (validSdr) captureSource = sdrTexture;
        else {
            sdrTexture->Release();
            sdrTexture = nullptr;
        }
    }
    ID3D11Texture2D* tonemapTexture = nullptr;
    ID3D11Texture2D* sceneTexture = d3d11::GetLatestSceneRenderTarget();
    if (sceneTexture && captureMode == 0 && captureSource == backbuffer) {
        D3D11_TEXTURE2D_DESC sceneDesc = {};
        sceneTexture->GetDesc(&sceneDesc);
        if (sceneDesc.Width >= backDesc.Width && sceneDesc.Height >= backDesc.Height &&
            sceneDesc.Format != DXGI_FORMAT_UNKNOWN) {
            captureSource = sceneTexture;
        } else {
            sceneTexture->Release();
            sceneTexture = nullptr;
        }
    } else if (sceneTexture) {
        sceneTexture->Release();
        sceneTexture = nullptr;
    }
    ID3D11RenderTargetView* activeRtv = nullptr;
    if (captureMode == 0 && captureSource == backbuffer)
        context->OMGetRenderTargets(1, &activeRtv, nullptr);
    if (activeRtv) {
        ID3D11Resource* resource = nullptr;
        activeRtv->GetResource(&resource);
        ID3D11Texture2D* activeTexture = nullptr;
        if (resource) {
            resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void**>(&activeTexture));
            resource->Release();
        }
        if (activeTexture && activeTexture != backbuffer) {
            D3D11_TEXTURE2D_DESC backDesc = {};
            D3D11_TEXTURE2D_DESC activeDesc = {};
            backbuffer->GetDesc(&backDesc);
            activeTexture->GetDesc(&activeDesc);
            if ((activeDesc.BindFlags & D3D11_BIND_RENDER_TARGET) &&
                activeDesc.Width >= backDesc.Width && activeDesc.Height >= backDesc.Height) {
                captureSource = activeTexture;
            } else {
                activeTexture->Release();
            }
        } else if (activeTexture) {
            activeTexture->Release();
        }
        activeRtv->Release();
    }
    if (captureMode == 3 && captureSource == backbuffer) {
        desktopTexture = CaptureDesktopFrame(device, context, swapChain);
        if (desktopTexture) captureSource = desktopTexture;
    }

    D3D11_TEXTURE2D_DESC captureDesc = {};
    captureSource->GetDesc(&captureDesc);
    if (captureDesc.Height) {
        uint32_t principalWidth = 0, principalHeight = 0;
        const bool principalExtentValid = camera::GetPrincipalRenderExtent(
            principalWidth, principalHeight) && principalWidth <= captureDesc.Width &&
            principalHeight <= captureDesc.Height;
        const float actualAspect = principalExtentValid
            ? static_cast<float>(principalWidth) / principalHeight
            : static_cast<float>(captureDesc.Width) / captureDesc.Height;
        const float previousAspect = m_renderAspect.exchange(
            actualAspect, std::memory_order_relaxed);
        if (std::fabs(previousAspect - actualAspect) > 0.001f)
            Log("[FrameLoop] Render aspect corrected from %.4f to actual %.4f (%ux%u)",
                previousAspect, actualAspect, captureDesc.Width, captureDesc.Height);
    }
    static ID3D11Texture2D* loggedSource = nullptr;
    if (captureSource != loggedSource) {
        Log("[FrameLoop] Capture source=%p (%s) %ux%u fmt=%u samples=%u bind=0x%X",
            captureSource, captureSource == backbuffer ? "backbuffer" :
                (captureSource == desktopTexture ? "desktop output" :
                    (captureSource == latestSdrTexture ? "latest SDR target" :
                        (captureSource == sdrTexture ? "tracked SDR target" :
                            (captureSource == tonemapTexture ? "tonemap SceneColor" :
                                (captureSource == sceneTexture ? "tracked scene RTV" :
                                    (captureSource == composedTexture ? "composed source" : "active RTV")))))),
            captureDesc.Width, captureDesc.Height, captureDesc.Format,
            captureDesc.SampleDesc.Count, captureDesc.BindFlags);
        loggedSource = captureSource;
    }
    if (m_frameCount > 0 && m_frameCount % 300 == 0) {
        Log("[FrameLoop] Internal composition: composed=%llu/%llu rejectDevice=%llu "
            "rejectDesc=%llu tonemap=%llu/%llu active=%s", composedAccepted, composedSeen,
            composedRejectedDevice, composedRejectedDesc, tonemapAccepted, tonemapSeen,
            captureSource == composedTexture ? "composed" :
                (captureSource == tonemapTexture ? "tonemap" : "fallback"));
    }

    const bool finalIsBackbuffer = captureSource == backbuffer;
    const bool eyeTexturesRecreated = EnsureEyeTextures(device, captureSource, false);

    const int renderEye = renderedTicket.eye;
    if (eyeTexturesRecreated) {
        AcquireSRWLockExclusive(&m_captureLock);
        m_capturePairSerial = 0;
        m_eyeCaptureMask = 0;
        m_eyeCaptureSerial[0] = m_eyeCaptureSerial[1] = 0;
        m_eyeCaptureWasBackbuffer[0] = m_eyeCaptureWasBackbuffer[1] = false;
        m_captureWidth = m_captureHeight = m_captureSamples = 0;
        m_captureFormat = DXGI_FORMAT_UNKNOWN;
        if (m_hudExtractionTexture) {
            m_hudExtractionTexture->Release();
            m_hudExtractionTexture = nullptr;
        }
        ReleaseSRWLockExclusive(&m_captureLock);
        xr.InvalidateHudResources();
    }
    ID3D11RenderTargetView* boundTarget = nullptr;
    ID3D11DepthStencilView* boundDepth = nullptr;
    context->OMGetRenderTargets(1, &boundTarget, &boundDepth);
    bool sourceBound = false;
    if (boundTarget) {
        ID3D11Resource* resource = nullptr;
        boundTarget->GetResource(&resource);
        ID3D11Texture2D* texture = nullptr;
        if (resource) {
            resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void**>(&texture));
            resource->Release();
        }
        sourceBound = texture == captureSource;
        if (texture) texture->Release();
    }
    if (sourceBound) context->OMSetRenderTargets(0, nullptr, nullptr);
    bool capturedEye = false;
    if (m_eyeTextures[renderEye]) {
        if (captureDesc.SampleDesc.Count > 1) {
            DXGI_FORMAT resolveFormat = captureDesc.Format;
            if (resolveFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS)
                resolveFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (resolveFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS)
                resolveFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            context->ResolveSubresource(m_eyeTextures[renderEye], 0, captureSource, 0, resolveFormat);
        } else {
            context->CopyResource(m_eyeTextures[renderEye], captureSource);
        }
        capturedEye = true;
    }
    if (sourceBound) context->OMSetRenderTargets(1, &boundTarget, boundDepth);
    if (boundTarget) boundTarget->Release();
    if (boundDepth) boundDepth->Release();
    if (captureSource != backbuffer) captureSource->Release();
    backbuffer->Release();

    XrView capturedPairViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    AcquireSRWLockExclusive(&m_captureLock);
    if (renderEye == 0 || m_capturePairSerial != renderedTicket.pairSerial) {
        m_capturePairSerial = renderedTicket.pairSerial;
        m_eyeCaptureMask = 0;
        m_eyeCaptureSerial[0] = m_eyeCaptureSerial[1] = 0;
        m_eyeCaptureWasBackbuffer[0] = m_eyeCaptureWasBackbuffer[1] = false;
        m_capturePairViews[0] = renderedTicket.views[0];
        m_capturePairViews[1] = renderedTicket.views[1];
        m_captureWidth = captureDesc.Width;
        m_captureHeight = captureDesc.Height;
        m_captureFormat = captureDesc.Format;
        m_captureSamples = captureDesc.SampleDesc.Count;
    }
    const bool captureMetadataMatches = captureDesc.Width == m_captureWidth &&
        captureDesc.Height == m_captureHeight && captureDesc.Format == m_captureFormat &&
        captureDesc.SampleDesc.Count == m_captureSamples;
    if (!captureMetadataMatches) {
        Log("[FrameLoop] Stereo pair dropped: serial=%llu eye=%d capture source changed",
            static_cast<unsigned long long>(renderedTicket.pairSerial), renderEye);
        capturedEye = false;
    }
    if (capturedEye && renderedTicket.pairSerial == m_capturePairSerial) {
        m_eyeCaptureSerial[renderEye] = renderedTicket.pairSerial;
        m_eyeCaptureWasBackbuffer[renderEye] = finalIsBackbuffer;
        m_eyeCaptureMask |= static_cast<uint8_t>(1u << renderEye);
    }

    bool completePair = capturedEye && m_eyeCaptureMask == 0x3u &&
        m_eyeCaptureSerial[0] == renderedTicket.pairSerial &&
        m_eyeCaptureSerial[1] == renderedTicket.pairSerial;
    if (completePair) {
        capturedPairViews[0] = m_capturePairViews[0];
        capturedPairViews[1] = m_capturePairViews[1];
    }
    ReleaseSRWLockExclusive(&m_captureLock);
    bool leftOk = false;
    bool rightOk = false;
    bool hudSeparated = false;
    bool hudQuadPrepared = false;
    XrView submittedViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool xrFrameBegun = false;
    bool preparedWaitConsumed = false;
    if (completePair) {
        StartWaitWorker();
        preparedWaitConsumed = m_waitReadyEvent &&
            WaitForSingleObject(m_waitReadyEvent, 1) == WAIT_OBJECT_0;
        if (!preparedWaitConsumed) {
            static uint64_t waitWorkerMisses = 0;
            ++waitWorkerMisses;
            if (waitWorkerMisses <= 10 || waitWorkerMisses % 300 == 0) {
                Log("[FrameLoop] Prepared xrWaitFrame not ready; retaining compositor image "
                    "(count=%llu)", static_cast<unsigned long long>(waitWorkerMisses));
            }
        }
    }
    if (completePair && preparedWaitConsumed && xr.BeginFrame()) {
        xrFrameBegun = true;
        if (!xr.ShouldRender()) {
            static uint64_t noRenderCount = 0;
            if (++noRenderCount <= 5 || noRenderCount % 300 == 0)
                Log("[FrameLoop] ShouldRender=false (count=%llu sessionState=%d)",
                    noRenderCount, (int)xr.GetSessionState());
            completePair = false;
        } else if (!xr.LocateViews()) {
            static bool loggedLocateFailure = false;
            if (!loggedLocateFailure) {
                Log("[FrameLoop] xrLocateViews failed; skipping projection layer");
                loggedLocateFailure = true;
            }
            completePair = false;
        } else {
            m_poseSeeded = true;
        }
    } else if (completePair) {
        completePair = false;
    }
    if (completePair) {
        const bool reverseEyes = config::Get().reverse_eyes;
        const int leftSource = reverseEyes ? 1 : 0;
        const int rightSource = reverseEyes ? 0 : 1;
        m_submissionViews[0] = capturedPairViews[0];
        m_submissionViews[1] = capturedPairViews[1];
        m_submissionViewsValid = true;
        m_submissionRightAimValid = renderedTicket.rightAimValid;
        if (m_submissionRightAimValid) {
            memcpy(m_submissionRightAimPosition, renderedTicket.rightAimPosition,
                   sizeof(m_submissionRightAimPosition));
            memcpy(m_submissionRightAimRotation, renderedTicket.rightAimRotation,
                   sizeof(m_submissionRightAimRotation));
        }
        m_submissionAimPitchDegrees = renderedTicket.aimPitchDegrees;
        m_submissionAimYawDegrees = renderedTicket.aimYawDegrees;
        m_submissionProjectionCorrection = renderedTicket.projectionCorrection;
        m_submissionRenderAspect = renderedTicket.renderAspect;
        AcquireSRWLockExclusive(&m_captureLock);
        D3D11_TEXTURE2D_DESC finalDesc[2] = {}, worldDesc[2] = {};
        if (m_eyeTextures[0]) m_eyeTextures[0]->GetDesc(&finalDesc[0]);
        if (m_eyeTextures[1]) m_eyeTextures[1]->GetDesc(&finalDesc[1]);
        if (m_worldBeforeHudTextures[0]) m_worldBeforeHudTextures[0]->GetDesc(&worldDesc[0]);
        if (m_worldBeforeHudTextures[1]) m_worldBeforeHudTextures[1]->GetDesc(&worldDesc[1]);
        const bool bakeHud = xr.ShouldBakeHud();
        const bool separateHud = xr.ShouldSeparateHud();
        const bool worldPairMatches = m_eyeCaptureWasBackbuffer[0] &&
            m_eyeCaptureWasBackbuffer[1] && (bakeHud || separateHud) &&
            m_worldCapturePairSerial == renderedTicket.pairSerial &&
            m_worldCaptureMask == 0x3u &&
            m_worldCaptureSerial[0] == renderedTicket.pairSerial &&
            m_worldCaptureSerial[1] == renderedTicket.pairSerial &&
            m_worldBeforeHudTextures[0] && m_worldBeforeHudTextures[1] &&
            m_eyeTextures[0] && m_eyeTextures[1] &&
            finalDesc[0].Width == finalDesc[1].Width &&
            finalDesc[0].Height == finalDesc[1].Height &&
            finalDesc[0].Format == finalDesc[1].Format &&
            finalDesc[0].SampleDesc.Count == 1 && finalDesc[1].SampleDesc.Count == 1 &&
            worldDesc[0].Width == finalDesc[0].Width &&
            worldDesc[0].Height == finalDesc[0].Height &&
            worldDesc[0].Format == finalDesc[0].Format &&
            worldDesc[0].SampleDesc.Count == 1 &&
            worldDesc[1].Width == finalDesc[1].Width &&
            worldDesc[1].Height == finalDesc[1].Height &&
            worldDesc[1].Format == finalDesc[1].Format &&
            worldDesc[1].SampleDesc.Count == 1;
        // Eye 1 is the final AER pass and therefore owns the freshest mono HUD.
        if (worldPairMatches && ValidateHudPair(
                device, context, m_eyeTextures[1], m_worldBeforeHudTextures[1],
                renderedTicket.pairSerial) &&
            EnsureHudExtractionTexture(device, m_eyeTextures[1])) {
            const bool extracted = render::HudBlitter::Instance().ExtractDifference(
                device, context, m_eyeTextures[1], m_worldBeforeHudTextures[1],
                m_hudExtractionTexture);
            hudQuadPrepared = extracted && separateHud && xr.PrepareHudTexture(
                m_hudExtractionTexture, renderedTicket.pairSerial);
            hudSeparated = extracted && (bakeHud || hudQuadPrepared);
        }

        ID3D11Texture2D* bakedHud = hudSeparated && bakeHud
            ? m_hudExtractionTexture : nullptr;
        const bool vehicleWorldSnapshot = worldPairMatches &&
            camera::IsVehicleCameraActive();
        ID3D11Texture2D* leftProjection = (hudSeparated || vehicleWorldSnapshot)
            ? m_worldBeforeHudTextures[leftSource] : m_eyeTextures[leftSource];
        ID3D11Texture2D* rightProjection = (hudSeparated || vehicleWorldSnapshot)
            ? m_worldBeforeHudTextures[rightSource] : m_eyeTextures[rightSource];
        leftOk = leftProjection &&
            CopyTextureToEye(context, leftProjection, 0, false, leftSource, bakedHud);
        rightOk = rightProjection &&
            CopyTextureToEye(context, rightProjection, 1, false, rightSource, bakedHud);
        if (hudSeparated && (!leftOk || !rightOk)) {
            hudSeparated = false;
            hudQuadPrepared = false;
            leftOk = m_eyeTextures[leftSource] &&
                CopyTextureToEye(context, m_eyeTextures[leftSource], 0, false, leftSource);
            rightOk = m_eyeTextures[rightSource] &&
                CopyTextureToEye(context, m_eyeTextures[rightSource], 1, false, rightSource);
        }
        ReleaseSRWLockExclusive(&m_captureLock);
        m_submissionViewsValid = false;
        m_submissionRightAimValid = false;
        submittedViews[0] = capturedPairViews[leftSource];
        submittedViews[1] = capturedPairViews[rightSource];
        completePair = leftOk && rightOk;
        if (completePair) context->Flush();
    }

    // A left-eye engine frame does not own an OpenXR frame. The completed
    // right eye waits once, submits both frozen views, and seeds the pose for
    // the next pair, matching ME2VR's synchronized AER cadence.
    const bool submitted = xrFrameBegun && xr.EndFrame(
        completePair, completePair ? submittedViews : nullptr,
        completePair && hudQuadPrepared,
        completePair && hudQuadPrepared ? renderedTicket.pairSerial : 0);
    if (preparedWaitConsumed && m_waitRequestEvent) SetEvent(m_waitRequestEvent);
    static bool loggedFirstSubmission = false;
    if (submitted && completePair && !loggedFirstSubmission) {
        Log("[FrameLoop] First coherent stereo pair submitted: serial=%llu frame=%llu",
            static_cast<unsigned long long>(renderedTicket.pairSerial), m_frameCount);
        loggedFirstSubmission = true;
    }
    if (submitted && completePair) m_hasSubmittedStereoProjection = true;
    if (!submitted && completePair) {
        Log("[FrameLoop] Stereo pair submission failed: serial=%llu left=%d right=%d",
            static_cast<unsigned long long>(renderedTicket.pairSerial), leftOk, rightOk);
    }
    if (completePair || renderEye == 1) {
        AcquireSRWLockExclusive(&m_captureLock);
        m_capturePairSerial = 0;
        m_eyeCaptureMask = 0;
        m_eyeCaptureSerial[0] = m_eyeCaptureSerial[1] = 0;
        m_eyeCaptureWasBackbuffer[0] = m_eyeCaptureWasBackbuffer[1] = false;
        m_captureWidth = m_captureHeight = m_captureSamples = 0;
        m_captureFormat = DXGI_FORMAT_UNKNOWN;
        ResetHudCaptureMetadata();
        ReleaseSRWLockExclusive(&m_captureLock);
    }

    m_frameCount++;
    g_currentEye = renderEye;
}

}} // namespace bl1gotyvr::xr
