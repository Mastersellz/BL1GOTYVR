#include "FrameLoop.hpp"
#include "OpenXRContext.hpp"
#include "../core/VRMod.hpp"
#include "../core/globals.hpp"
#include "../input/InputHook.hpp"
#include "../d3d11/D3D11Hooks.hpp"
#include "../camera/CameraHook.hpp"
#include "../config/Config.hpp"
#include <cstring>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <algorithm>
#include <cmath>

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
    m_initialized = true;
    Log("[FrameLoop] Initialized");
}

void FrameLoop::Shutdown() {
    Log("[FrameLoop] Shutting down...");
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
}

void FrameLoop::InvalidateBackbufferResources() {
    for (int i = 0; i < 2; i++) {
        if (m_eyeTextures[i]) { m_eyeTextures[i]->Release(); m_eyeTextures[i] = nullptr; }
    }
    m_sequentialRenderEye = -1;
    m_sequentialCaptureMask = 0;
    m_sequentialFramePending = false;
    m_sequentialCaptureFailed = false;
}

void FrameLoop::EnsureEyeTextures(ID3D11Device* device, ID3D11Texture2D* source,
                                  bool sideBySideSource) {
    D3D11_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);
    if (sideBySideSource) desc.Width /= 2;

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
        if (FAILED(hr)) {
            Log("[FrameLoop] ERROR: CreateTexture2D(eye %d) = 0x%08X", i, hr);
        }
    }
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
    };
    float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
        uv = saturate(uv * uvScale + uvOffset + float2(eyeShift, 0.0));
    float4 color = sourceTexture.Sample(sourceSampler, uv);
    if (hdrSource > 0.5) {
        float3 x = max(color.rgb, 0.0);
        color.rgb = saturate((x * (2.51 * x + 0.03)) /
                             (x * (2.43 * x + 0.59) + 0.14));
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
    constantsDesc.ByteWidth = 32;
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
                            bool sideBySideSource, int sourceEye) {
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
    srvDesc.Format = typedFormat(sourceDesc.Format);
    if (sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (sourceDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (sourceDesc.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS)
        srvDesc.Format = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = destinationDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS
        ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : typedFormat(destinationDesc.Format);
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
    float projectionFov = 0.0f;
    const auto& vrSettings = config::Get();
    const int sampledEye = sourceEye >= 0 ? sourceEye : eye;
    const float sourceAspect = sideBySideSource
        ? static_cast<float>(sourceDesc.Width / 2) / sourceDesc.Height
        : static_cast<float>(sourceDesc.Width) / sourceDesc.Height;
    const bool projectionCrop = m_projectionCorrection &&
        OpenXRContext::Instance().GetProjectionCrop(eye,
            sourceAspect,
            uvScaleX, uvScaleY, uvOffsetX, uvOffsetY, projectionFov);
    if (sideBySideSource) {
        uvScaleX *= 0.5f;
        uvOffsetX = (sampledEye == 0 ? 0.0f : 0.5f) + uvOffsetX * 0.5f;
    } else if (!m_submittingNativeEyes && !projectionCrop &&
               vrSettings.convergence_m > 0.0f && eye >= 0 && eye < 2) {
        const float magnitude = (std::min)(0.20f, vrSettings.convergence_m * 0.01f);
        convergenceShift = eye == 0 ? -magnitude : magnitude;
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
            Log("[FrameLoop] Projection crop: FOV=%.2f scale=(%.4f,%.4f) offset=(%.4f,%.4f)",
                projectionFov, uvScaleX, uvScaleY, uvOffsetX, uvOffsetY);
            loggedProjectionCrop = true;
        }
    }
    const float settings[8] = {
        sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1.0f : 0.0f,
        convergenceShift, uvScaleX, uvScaleY, uvOffsetX, uvOffsetY, 0.0f, 0.0f
    };
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
                                 bool sideBySideSource, int sourceEye) {
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

    // Copy source to destination
    D3D11_TEXTURE2D_DESC srcDesc, dstDesc;
    source->GetDesc(&srcDesc);
    destTex->GetDesc(&dstDesc);

    bool copied = true;
    if (!sideBySideSource && srcDesc.Width == dstDesc.Width && srcDesc.Height == dstDesc.Height &&
        srcDesc.Format == dstDesc.Format) {
        context->CopyResource(destTex, source);
    } else {
        if (!BlitTexture(context, source, destTex, eye, sideBySideSource, sourceEye)) {
            static bool loggedBlitFailure = false;
            if (!loggedBlitFailure) {
                Log("[FrameLoop] ERROR: Failed to scale %ux%u source to %ux%u eye texture",
                    srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height);
                loggedBlitFailure = true;
            }
            copied = false;
        }
    }

    // Release
    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(eyeData.swapchain, &releaseInfo);
    return copied;
}

int FrameLoop::GetRenderEye() const {
    const int sequentialEye = m_sequentialRenderEye.load();
    if (sequentialEye >= 0) return sequentialEye;
    return (int)(m_frameCount & 1);
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
    // TODO: Phase 7 — BMP capture for offline stereo verification
    Log("[FrameLoop] Stereo capture requested (not yet implemented)");
}

void FrameLoop::OnPresent(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) {
    if (!m_initialized) return;

    auto& xr = OpenXRContext::Instance();

    if (config::Get().same_frame_stereo) {
        auto prepareFrame = [&]() {
            if (xr.IsFrameActive()) return true;
            if (!xr.WaitForFrame() || !xr.BeginFrame()) return false;
            input::InputHook::Instance().UpdateState();
            if (!xr.ShouldRender() || !xr.LocateViews()) {
                xr.EndFrame(false);
                return false;
            }
            return true;
        };

        if (!prepareFrame()) return;
        if (!m_sequentialFramePending.load()) return;
        if (m_sequentialRenderEye.load() >= 0) return;

        const uint8_t mask = m_sequentialCaptureMask.load();
        bool complete = !m_sequentialCaptureFailed.load() && mask == 0x3u;
        if (complete) {
            const int leftSource = config::Get().reverse_eyes ? 1 : 0;
            const int rightSource = config::Get().reverse_eyes ? 0 : 1;
            const bool leftCopied = CopyTextureToEye(context, m_eyeTextures[leftSource], 0);
            const bool rightCopied = CopyTextureToEye(context, m_eyeTextures[rightSource], 1);
            if (!leftCopied || !rightCopied) complete = false;
        }
        xr.EndFrame(complete);
        m_sequentialFramePending = false;
        m_sequentialCaptureMask = 0;
        m_sequentialCaptureFailed = false;
        m_frameCount++;
        prepareFrame();
        return;
    }

    // Desktop test mode
    if (m_desktopTestMode) {
        // For desktop mode, still submit to compositor if available
        if (!xr.IsInitialized()) return;
    }

    // OpenXR frame lifecycle
    if (!xr.WaitForFrame()) return;
    if (!xr.BeginFrame()) return;

    // Update input
    input::InputHook::Instance().UpdateState();

    if (!xr.ShouldRender()) {
        xr.EndFrame(false);
        return;
    }
    if (!xr.LocateViews()) {
        static bool loggedLocateFailure = false;
        if (!loggedLocateFailure) {
            Log("[FrameLoop] xrLocateViews failed; skipping projection layer");
            loggedLocateFailure = true;
        }
        xr.EndFrame(false);
        return;
    }

    // Get the swapchain backbuffer and inspect the render target left bound by
    // UE3. Gameplay can remain in an intermediate target while menus are
    // composed directly into the backbuffer.
    ID3D11Texture2D* backbuffer = nullptr;
    backbuffer = d3d11::AcquireCurrentBackbuffer(swapChain);
    if (!backbuffer) {
        xr.EndFrame(false);
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
        captureMode = captureMode == 3 ? 1 : (captureMode == 1 ? 2 : 3);
        Log("[FrameLoop] Capture mode changed: %s",
            captureMode == 3 ? "GDI window" :
                (captureMode == 1 ? "internal SDR backbuffer" : "tracked SDR target"));
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
        m_projectionCorrection = !m_projectionCorrection;
        Log("[FrameLoop] Projection correction %s", m_projectionCorrection ? "ENABLED" : "disabled");
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
    ID3D11Texture2D* composedTexture = d3d11::GetLatestComposedTexture();
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

    const bool nativeMultiview = camera::IsNativeMultiviewActive();
    const uint64_t nativeGeneration = camera::GetNativeMultiviewGeneration();
    const bool freshNativePair = nativeMultiview &&
        nativeGeneration != m_lastNativeMultiviewGeneration;
    EnsureEyeTextures(device, captureSource, nativeMultiview);

    int renderEye = GetRenderEye();
    if (m_gdiLatencyCorrection && captureSource == desktopTexture) renderEye ^= 1;
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
    bool leftOk = false;
    bool rightOk = false;
    if (nativeMultiview) {
        bool capturedPair = m_lastNativeMultiviewGeneration != 0;
        if (freshNativePair) {
            capturedPair = m_eyeTextures[0] && m_eyeTextures[1] &&
                BlitTexture(context, captureSource, m_eyeTextures[0], 0, true, 0) &&
                BlitTexture(context, captureSource, m_eyeTextures[1], 1, true, 1);
            if (capturedPair)
                m_lastNativeMultiviewGeneration = nativeGeneration;
        } else {
            static uint64_t reusedNativePairs = 0;
            if (++reusedNativePairs == 1 || reusedNativePairs % 300 == 0) {
                Log("[FrameLoop] Reusing synchronized native pair: count=%llu generation=%llu",
                    reusedNativePairs, m_lastNativeMultiviewGeneration);
            }
        }
        const bool reverseEyes = config::Get().reverse_eyes;
        const int leftSource = reverseEyes ? 1 : 0;
        const int rightSource = reverseEyes ? 0 : 1;
        m_submittingNativeEyes = true;
        leftOk = capturedPair && m_eyeTextures[leftSource] &&
            CopyTextureToEye(context, m_eyeTextures[leftSource], 0);
        rightOk = capturedPair && m_eyeTextures[rightSource] &&
            CopyTextureToEye(context, m_eyeTextures[rightSource], 1);
        m_submittingNativeEyes = false;
    } else if (m_eyeTextures[renderEye]) {
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
    }
    if (sourceBound) context->OMSetRenderTargets(1, &boundTarget, boundDepth);
    if (boundTarget) boundTarget->Release();
    if (boundDepth) boundDepth->Release();
    if (captureSource != backbuffer) captureSource->Release();
    backbuffer->Release();

    if (!nativeMultiview) {
        const bool reverseEyes = config::Get().reverse_eyes;
        const int leftSource = reverseEyes ? 1 : 0;
        const int rightSource = reverseEyes ? 0 : 1;
        leftOk = m_eyeTextures[leftSource] != nullptr;
        rightOk = m_eyeTextures[rightSource] != nullptr;
        if (leftOk) leftOk = CopyTextureToEye(context, m_eyeTextures[leftSource], 0);
        if (rightOk) rightOk = CopyTextureToEye(context, m_eyeTextures[rightSource], 1);
    }

    // End frame
    const bool submitted = xr.EndFrame(leftOk && rightOk);
    static bool loggedFirstSubmission[2] = {};
    const int submissionMode = nativeMultiview ? 1 : 0;
    if (submitted && !loggedFirstSubmission[submissionMode]) {
        Log("[FrameLoop] First %s OpenXR frame submitted",
            nativeMultiview ? "native multiview" : "AFR");
        loggedFirstSubmission[submissionMode] = true;
    }

    m_frameCount++;
    g_currentEye = nativeMultiview ? -1 : renderEye;
}

}} // namespace bl1gotyvr::xr
