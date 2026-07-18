#include "D3D11Hooks.hpp"
#include "../core/VRMod.hpp"
#include "../core/globals.hpp"
#include "../hook/MinHookWrapper.hpp"
#include "../xr/OpenXRContext.hpp"
#include "../xr/FrameLoop.hpp"
#include "../config/Config.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <Windows.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace bl1gotyvr { namespace d3d11 {

// Original function pointers
using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using ResolveSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                                       ID3D11Resource*, UINT, DXGI_FORMAT);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                       ID3D11RenderTargetView* const*,
                                                       ID3D11DepthStencilView*);
using PSSetShaderResourcesFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                         ID3D11ShaderResourceView* const*);

static PresentFn       oPresent = nullptr;
static ResizeBuffersFn oResizeBuffers = nullptr;
static CopyResourceFn oCopyResource = nullptr;
static ResolveSubresourceFn oResolveSubresource = nullptr;
static OMSetRenderTargetsFn oOMSetRenderTargets = nullptr;
static PSSetShaderResourcesFn oPSSetShaderResources = nullptr;

static ID3D11Device*        s_gameDevice = nullptr;
static ID3D11DeviceContext*  s_gameContext = nullptr;
static IDXGISwapChain*       s_gameSwapChain = nullptr;
static ID3D11Texture2D*      s_observedBackbuffer = nullptr;
static ID3D11Texture2D*      s_latestComposedTexture = nullptr;
static ID3D11Texture2D*      s_latestSceneRenderTarget = nullptr;
static ID3D11Texture2D*      s_latestSdrRenderTarget = nullptr;
static ID3D11Texture2D*      s_trackedRenderTargets[16] = {};
static int                   s_trackedRenderTargetCount = 0;
static int                   s_sceneRenderTargetScore = -1;
static ID3D11Texture2D*      s_latestTonemapSource = nullptr;
static int                   s_tonemapSourceScore = -1;
static bool                  s_backbufferBound = false;
static ID3D11Texture2D*      s_postTonemapTexture = nullptr;
static uint64_t              s_postTonemapGeneration = 0;
static uint64_t              s_consumedPostTonemapGeneration = 0;
static SRWLOCK               s_captureLock = SRWLOCK_INIT;
static thread_local bool     s_insidePresent = false;

ID3D11Device* GetGameDevice() { return s_gameDevice; }
ID3D11DeviceContext* GetGameContext() { return s_gameContext; }
IDXGISwapChain* GetGameSwapChain() { return s_gameSwapChain; }

ID3D11Texture2D* GetLatestComposedTexture() {
    AcquireSRWLockExclusive(&s_captureLock);
    ID3D11Texture2D* texture = s_latestComposedTexture;
    s_latestComposedTexture = nullptr;
    ReleaseSRWLockExclusive(&s_captureLock);
    return texture;
}

ID3D11Texture2D* GetLatestSceneRenderTarget() {
    AcquireSRWLockExclusive(&s_captureLock);
    ID3D11Texture2D* texture = s_latestSceneRenderTarget;
    s_latestSceneRenderTarget = nullptr;
    s_sceneRenderTargetScore = -1;
    ReleaseSRWLockExclusive(&s_captureLock);
    return texture;
}

ID3D11Texture2D* GetLatestSdrRenderTarget() {
    AcquireSRWLockExclusive(&s_captureLock);
    ID3D11Texture2D* texture = s_latestSdrRenderTarget;
    s_latestSdrRenderTarget = nullptr;
    ReleaseSRWLockExclusive(&s_captureLock);
    return texture;
}

int GetTrackedRenderTargetCount() {
    AcquireSRWLockShared(&s_captureLock);
    const int count = s_trackedRenderTargetCount;
    ReleaseSRWLockShared(&s_captureLock);
    return count;
}

ID3D11Texture2D* GetTrackedRenderTarget(int index) {
    AcquireSRWLockShared(&s_captureLock);
    ID3D11Texture2D* texture = index >= 0 && index < s_trackedRenderTargetCount
        ? s_trackedRenderTargets[index] : nullptr;
    if (texture) texture->AddRef();
    ReleaseSRWLockShared(&s_captureLock);
    return texture;
}

ID3D11Texture2D* GetLatestTonemapSource() {
    AcquireSRWLockExclusive(&s_captureLock);
    ID3D11Texture2D* texture = s_latestTonemapSource;
    s_latestTonemapSource = nullptr;
    s_tonemapSourceScore = -1;
    ReleaseSRWLockExclusive(&s_captureLock);
    return texture;
}

ID3D11Texture2D* GetPostTonemapTexture() {
    AcquireSRWLockExclusive(&s_captureLock);
    ID3D11Texture2D* texture = nullptr;
    if (s_postTonemapGeneration != s_consumedPostTonemapGeneration) {
        texture = s_postTonemapTexture;
        s_consumedPostTonemapGeneration = s_postTonemapGeneration;
    }
    if (texture) texture->AddRef();
    ReleaseSRWLockExclusive(&s_captureLock);
    return texture;
}

ID3D11Texture2D* AcquireCurrentBackbuffer(IDXGISwapChain* swapChain, UINT* bufferIndex) {
    if (!swapChain) return nullptr;
    UINT index = 0;
    IDXGISwapChain3* swapChain3 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3),
                                             reinterpret_cast<void**>(&swapChain3)))) {
        index = swapChain3->GetCurrentBackBufferIndex();
        swapChain3->Release();
    }
    if (bufferIndex) *bufferIndex = index;
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(index, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backbuffer)))) return nullptr;
    return backbuffer;
}

static void ObserveBackbuffer(IDXGISwapChain* swapChain) {
    ID3D11Texture2D* backbuffer = AcquireCurrentBackbuffer(swapChain);
    if (!backbuffer) return;
    AcquireSRWLockExclusive(&s_captureLock);
    if (s_observedBackbuffer) s_observedBackbuffer->Release();
    s_observedBackbuffer = backbuffer;
    ReleaseSRWLockExclusive(&s_captureLock);
}

static void RememberComposedSource(ID3D11Resource* destination, ID3D11Resource* source) {
    AcquireSRWLockShared(&s_captureLock);
    const bool writesBackbuffer = destination == s_observedBackbuffer;
    ReleaseSRWLockShared(&s_captureLock);
    if (!writesBackbuffer || !source) return;

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(source->QueryInterface(__uuidof(ID3D11Texture2D),
                                      reinterpret_cast<void**>(&texture)))) return;
    AcquireSRWLockExclusive(&s_captureLock);
    if (s_latestComposedTexture) s_latestComposedTexture->Release();
    s_latestComposedTexture = texture;
    ReleaseSRWLockExclusive(&s_captureLock);
}

static void STDMETHODCALLTYPE HookedCopyResource(ID3D11DeviceContext* context,
                                                  ID3D11Resource* destination,
                                                  ID3D11Resource* source) {
    oCopyResource(context, destination, source);
    if (s_insidePresent) return;
    RememberComposedSource(destination, source);
}

static void STDMETHODCALLTYPE HookedResolveSubresource(ID3D11DeviceContext* context,
                                                        ID3D11Resource* destination,
                                                        UINT destinationSubresource,
                                                        ID3D11Resource* source,
                                                        UINT sourceSubresource,
                                                        DXGI_FORMAT format) {
    oResolveSubresource(context, destination, destinationSubresource,
                        source, sourceSubresource, format);
    if (s_insidePresent) return;
    RememberComposedSource(destination, source);
}

static void CapturePostTonemap(ID3D11DeviceContext* context, UINT marker) {
    AcquireSRWLockShared(&s_captureLock);
    ID3D11Texture2D* backbuffer = s_observedBackbuffer;
    if (backbuffer) backbuffer->AddRef();
    ReleaseSRWLockShared(&s_captureLock);
    if (!backbuffer) return;

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    backbuffer->GetDesc(&sourceDesc);
    ID3D11Texture2D* destination = nullptr;
    AcquireSRWLockShared(&s_captureLock);
    destination = s_postTonemapTexture;
    if (destination) destination->AddRef();
    ReleaseSRWLockShared(&s_captureLock);

    bool recreate = !destination;
    if (destination) {
        D3D11_TEXTURE2D_DESC destinationDesc = {};
        destination->GetDesc(&destinationDesc);
        recreate = destinationDesc.Width != sourceDesc.Width ||
                   destinationDesc.Height != sourceDesc.Height ||
                   destinationDesc.Format != sourceDesc.Format;
    }
    if (recreate) {
        if (destination) { destination->Release(); destination = nullptr; }
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        if (device) {
            D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copyDesc.CPUAccessFlags = 0;
            copyDesc.MiscFlags = 0;
            device->CreateTexture2D(&copyDesc, nullptr, &destination);
            device->Release();
        }
        if (destination) {
            AcquireSRWLockExclusive(&s_captureLock);
            if (s_postTonemapTexture) s_postTonemapTexture->Release();
            s_postTonemapTexture = destination;
            s_postTonemapTexture->AddRef();
            ReleaseSRWLockExclusive(&s_captureLock);
            static bool loggedCapture = false;
            if (!loggedCapture) {
                Log("[BL1GOTYVR] Capturing post-tonemap target: marker=%u %ux%u fmt=%u",
                    marker, sourceDesc.Width, sourceDesc.Height, sourceDesc.Format);
                loggedCapture = true;
            }
        }
    }
    if (destination) {
        oCopyResource(context, destination, backbuffer);
        AcquireSRWLockExclusive(&s_captureLock);
        ++s_postTonemapGeneration;
        ReleaseSRWLockExclusive(&s_captureLock);
        destination->Release();
        if (marker == UINT_MAX) {
            static uint64_t postPresentCaptures = 0;
            if (++postPresentCaptures == 1 || postPresentCaptures % 300 == 0) {
                Log("[BL1GOTYVR] Post-Present backbuffer refreshed: count=%llu",
                    postPresentCaptures);
            }
        }
    }
    backbuffer->Release();
}

static void STDMETHODCALLTYPE HookedOMSetRenderTargets(ID3D11DeviceContext* context,
                                                        UINT viewCount,
                                                        ID3D11RenderTargetView* const* views,
                                                        ID3D11DepthStencilView* depthView) {
    if (s_insidePresent) {
        oOMSetRenderTargets(context, viewCount, views, depthView);
        return;
    }
    bool backbufferBound = false;
    for (UINT i = 0; i < viewCount && views; ++i) {
        if (!views[i]) continue;
        ID3D11Resource* boundResource = nullptr;
        views[i]->GetResource(&boundResource);
        AcquireSRWLockShared(&s_captureLock);
        const bool matches = boundResource == s_observedBackbuffer;
        ReleaseSRWLockShared(&s_captureLock);
        if (boundResource) boundResource->Release();
        if (matches) { backbufferBound = true; break; }
    }
    oOMSetRenderTargets(context, viewCount, views, depthView);
    AcquireSRWLockExclusive(&s_captureLock);
    s_backbufferBound = backbufferBound;
    ReleaseSRWLockExclusive(&s_captureLock);
    if (!viewCount || !views || !views[0]) return;

    ID3D11Resource* resource = nullptr;
    views[0]->GetResource(&resource);
    if (!resource) return;
    ID3D11Texture2D* texture = nullptr;
    resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    resource->Release();
    if (!texture) return;

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    static LONG capturedGameplayStack = 0;
    if (g_frameCount > 600 && desc.Width == 1920 && desc.Height == 1080 &&
        desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        InterlockedCompareExchange(&capturedGameplayStack, 1, 0) == 0) {
        void* frames[32] = {};
        const USHORT frameCount = CaptureStackBackTrace(1, _countof(frames), frames, nullptr);
        Log("[BL1GOTYVR] Gameplay HDR RTV call stack: frames=%u", frameCount);
        for (USHORT frame = 0; frame < frameCount; ++frame) {
            HMODULE owner = nullptr;
            char ownerPath[MAX_PATH] = "<private>";
            uintptr_t offset = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(frames[frame]), &owner) && owner) {
                GetModuleFileNameA(owner, ownerPath, MAX_PATH);
                offset = reinterpret_cast<uintptr_t>(frames[frame]) -
                         reinterpret_cast<uintptr_t>(owner);
            }
            Log("[BL1GOTYVR]   stack[%u]=%p module=%s+0x%llX", frame, frames[frame],
                ownerPath, static_cast<unsigned long long>(offset));
        }
        static constexpr uintptr_t candidateRvas[] = {
            0x01237E90, 0x012369B0, 0x005905F0, 0x005911CA,
            0x00435E78, 0x00436A10, 0x001F7C70
        };
        const uintptr_t gameBase = reinterpret_cast<uintptr_t>(
            GetModuleHandleA("BorderlandsGOTY.exe"));
        for (uintptr_t rva : candidateRvas) {
            const auto* code = reinterpret_cast<const unsigned char*>(gameBase + rva);
            char bytes[32 * 3 + 1] = {};
            for (int index = 0; index < 32; ++index) {
                sprintf_s(bytes + index * 3, sizeof(bytes) - index * 3,
                          index == 31 ? "%02X" : "%02X ", code[index]);
            }
            Log("[BL1GOTYVR] Render candidate RVA 0x%08llX bytes=%s",
                static_cast<unsigned long long>(rva), bytes);
        }
    }
    const float aspect = desc.Height ? static_cast<float>(desc.Width) / desc.Height : 0.0f;
    static bool loggedFormats[192] = {};
    if (static_cast<unsigned>(desc.Format) < _countof(loggedFormats) && !loggedFormats[desc.Format]) {
        loggedFormats[desc.Format] = true;
        Log("[BL1GOTYVR] Scene RTV format candidate: %ux%u fmt=%u samples=%u bind=0x%X",
            desc.Width, desc.Height, desc.Format, desc.SampleDesc.Count, desc.BindFlags);
    }
    AcquireSRWLockShared(&s_captureLock);
    const bool isBackbuffer = texture == s_observedBackbuffer;
    ReleaseSRWLockShared(&s_captureLock);
    if (isBackbuffer || desc.Width < 640 || desc.Height < 360 || aspect < 1.4f || aspect > 2.2f) {
        texture->Release();
        return;
    }

    const bool sdrColor = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                          desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                          desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                          desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                          desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                          desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (desc.Width == 1920 && desc.Height == 1080 && sdrColor) {
        AcquireSRWLockExclusive(&s_captureLock);
        int existing = -1;
        for (int index = 0; index < s_trackedRenderTargetCount; ++index) {
            if (s_trackedRenderTargets[index] == texture) { existing = index; break; }
        }
        if (existing < 0 && s_trackedRenderTargetCount < _countof(s_trackedRenderTargets)) {
            const int index = s_trackedRenderTargetCount++;
            s_trackedRenderTargets[index] = texture;
            texture->AddRef();
            Log("[BL1GOTYVR] Tracked render target[%d]: texture=%p format=%u bind=0x%X",
                index, texture, desc.Format, desc.BindFlags);
        }
        ReleaseSRWLockExclusive(&s_captureLock);
    }
    if (sdrColor) {
        AcquireSRWLockExclusive(&s_captureLock);
        if (s_latestSdrRenderTarget) s_latestSdrRenderTarget->Release();
        s_latestSdrRenderTarget = texture;
        texture->AddRef();
        ReleaseSRWLockExclusive(&s_captureLock);
    }

    int score = 10;
    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        score = 100;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        score = 80;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        // UE3's actual scene color is HDR and is tonemapped by our VR blit.
        score = 200;
        break;
    default:
        break;
    }

    AcquireSRWLockExclusive(&s_captureLock);
    if (score >= s_sceneRenderTargetScore) {
        if (s_latestSceneRenderTarget) s_latestSceneRenderTarget->Release();
        s_latestSceneRenderTarget = texture;
        s_sceneRenderTargetScore = score;
        texture = nullptr;
    }
    ReleaseSRWLockExclusive(&s_captureLock);
    if (texture) texture->Release();
}

static void STDMETHODCALLTYPE HookedPSSetShaderResources(ID3D11DeviceContext* context,
                                                          UINT startSlot, UINT viewCount,
                                                          ID3D11ShaderResourceView* const* views) {
    if (s_insidePresent) {
        oPSSetShaderResources(context, startSlot, viewCount, views);
        return;
    }
    oPSSetShaderResources(context, startSlot, viewCount, views);
    AcquireSRWLockShared(&s_captureLock);
    const bool inspect = s_backbufferBound;
    ReleaseSRWLockShared(&s_captureLock);
    if (!inspect || !views) return;

    for (UINT i = 0; i < viewCount; ++i) {
        if (!views[i]) continue;
        ID3D11Resource* resource = nullptr;
        views[i]->GetResource(&resource);
        if (!resource) continue;
        ID3D11Texture2D* texture = nullptr;
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        resource->Release();
        if (!texture) continue;

        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);
        const float aspect = desc.Height ? static_cast<float>(desc.Width) / desc.Height : 0.0f;
        int score = -1;
        if (desc.Width >= 640 && desc.Height >= 360 && aspect >= 1.4f && aspect <= 2.2f) {
            if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) score = 200;
            else if (desc.Format == DXGI_FORMAT_R11G11B10_FLOAT) score = 180;
            else if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                     desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) score = 100;
            if (startSlot + i == 0 && score >= 0) score += 25;
        }
        if (score < 0) { texture->Release(); continue; }

        static bool loggedSlots[16][192] = {};
        const UINT slot = startSlot + i;
        if (slot < 16 && static_cast<unsigned>(desc.Format) < 192 &&
            !loggedSlots[slot][desc.Format]) {
            loggedSlots[slot][desc.Format] = true;
            Log("[BL1GOTYVR] Backbuffer pass SRV: slot=%u %ux%u fmt=%u bind=0x%X",
                slot, desc.Width, desc.Height, desc.Format, desc.BindFlags);
        }

        AcquireSRWLockExclusive(&s_captureLock);
        if (score >= s_tonemapSourceScore) {
            if (s_latestTonemapSource) s_latestTonemapSource->Release();
            s_latestTonemapSource = texture;
            s_tonemapSourceScore = score;
            texture = nullptr;
        }
        ReleaseSRWLockExclusive(&s_captureLock);
        if (texture) texture->Release();
    }
}

// Present hook — main frame entry point
static HRESULT WINAPI HookedPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    s_insidePresent = true;
    __try {
        config::ReloadIfChanged();
        struct SwapChainObservation {
            IDXGISwapChain* swapChain;
            uint64_t presents;
        };
        static SwapChainObservation observations[8] = {};
        SwapChainObservation* observation = nullptr;
        for (auto& current : observations) {
            if (current.swapChain == sc) {
                observation = &current;
                break;
            }
            if (!current.swapChain && !observation) observation = &current;
        }
        if (observation && !observation->swapChain) {
            observation->swapChain = sc;
            DXGI_SWAP_CHAIN_DESC swapDesc = {};
            ID3D11Device* swapDevice = nullptr;
            sc->GetDesc(&swapDesc);
            sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&swapDevice));
            char title[128] = {};
            char className[128] = {};
            if (swapDesc.OutputWindow) {
                GetWindowTextA(swapDesc.OutputWindow, title, sizeof(title));
                GetClassNameA(swapDesc.OutputWindow, className, sizeof(className));
            }
            Log("[BL1GOTYVR] Swapchain discovered: sc=%p device=%p hwnd=%p title='%s' class='%s' "
                "size=%ux%u buffers=%u format=%u swapEffect=%u windowed=%d",
                sc, swapDevice, swapDesc.OutputWindow, title, className,
                swapDesc.BufferDesc.Width, swapDesc.BufferDesc.Height, swapDesc.BufferCount,
                swapDesc.BufferDesc.Format, swapDesc.SwapEffect, swapDesc.Windowed);
            if (swapDevice) swapDevice->Release();
        }
        if (observation) {
            ++observation->presents;
            if (observation->presents % 600 == 0) {
                Log("[BL1GOTYVR] Swapchain heartbeat: sc=%p presents=%llu",
                    sc, observation->presents);
            }
        }
        s_gameSwapChain = sc;
        ObserveBackbuffer(sc);
        g_frameCount++;

        // Log first few frames for debugging
        if (g_frameCount <= 5) {
            UINT bufferIndex = 0;
            ID3D11Texture2D* current = AcquireCurrentBackbuffer(sc, &bufferIndex);
            Log("[BL1GOTYVR] Present hook called! Frame %llu, sc=%p currentBuffer=%u texture=%p",
                g_frameCount.load(), sc, bufferIndex, current);
            if (current) current->Release();
        }

        // Capture device on first call
        if (!s_gameDevice) {
            Log("[BL1GOTYVR] Attempting GetDevice...");
            HRESULT hr = sc->GetDevice(__uuidof(ID3D11Device), (void**)&s_gameDevice);
            Log("[BL1GOTYVR] GetDevice returned: 0x%08X, device=%p", hr, s_gameDevice);
            if (s_gameDevice) {
                s_gameDevice->GetImmediateContext(&s_gameContext);
                Log("[BL1GOTYVR] Captured D3D11 device: %p, context: %p", s_gameDevice, s_gameContext);

                // Initialize FrameLoop
                Log("[BL1GOTYVR] Initializing FrameLoop...");
                xr::FrameLoop::Instance().Initialize();
                Log("[BL1GOTYVR] FrameLoop initialized");
            }
        }

        // Initialize OpenXR on first frame with valid device
        if (s_gameDevice && !xr::OpenXRContext::Instance().IsInitialized()) {
            Log("[BL1GOTYVR] Attempting OpenXR init...");
            // Get backbuffer format
            ID3D11Texture2D* bb = nullptr;
            bb = AcquireCurrentBackbuffer(sc);
            HRESULT hr = bb ? S_OK : E_FAIL;
            Log("[BL1GOTYVR] GetBuffer returned: 0x%08X, bb=%p", hr, bb);
            if (SUCCEEDED(hr) && bb) {
                D3D11_TEXTURE2D_DESC desc;
                bb->GetDesc(&desc);
                bb->Release();

                if (xr::OpenXRContext::Instance().Initialize(s_gameDevice, desc.Format)) {
                    Log("[BL1GOTYVR] OpenXR initialized (fmt=%u)", (uint32_t)desc.Format);
                } else {
                    // Retry in 300 frames
                    if (g_frameCount % 300 == 0) {
                        Log("[BL1GOTYVR] OpenXR init failed, retrying in 300 frames...");
                    }
                }
            }
        }

        // Handle F6 toggle for desktop test mode
        static bool f6WasDown = false;
        bool f6Down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6Down && !f6WasDown) {
            xr::FrameLoop::Instance().ToggleDesktopTestMode();
        }
        f6WasDown = f6Down;

        // Run frame loop (OpenXR submission)
        if (xr::OpenXRContext::Instance().IsInitialized()) {
            xr::FrameLoop::Instance().OnPresent(s_gameDevice, s_gameContext, sc);
        }

        if (g_frameCount % 300 == 0) {
            Log("[BL1GOTYVR] Frame %llu (VR=%d)", g_frameCount.load(),
                xr::FrameLoop::Instance().IsVRActive());
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[BL1GOTYVR] Exception in Present hook: 0x%08X", GetExceptionCode());
    }

    const HRESULT result = oPresent(sc, syncInterval, flags);
    s_insidePresent = false;
    ObserveBackbuffer(sc);
    return result;
}

// ResizeBuffers hook — recreate VR resources on resolution change
static HRESULT WINAPI HookedResizeBuffers(IDXGISwapChain* sc, UINT bufferCount, UINT width, UINT height,
                                           DXGI_FORMAT newFormat, UINT swapChainFlags) {
    __try {
        const auto& settings = config::Get();
        width = static_cast<UINT>(settings.render_width);
        height = static_cast<UINT>(settings.render_height);
        Log("[BL1GOTYVR] ResizeBuffers: forcing %ux%u, fmt=%u", width, height, newFormat);

        // Invalidate VR textures before resize
        xr::FrameLoop::Instance().InvalidateBackbufferResources();

        // Release old device references before resize
        if (s_gameContext) { s_gameContext->Release(); s_gameContext = nullptr; }
        if (s_gameDevice) { s_gameDevice->Release(); s_gameDevice = nullptr; }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[BL1GOTYVR] Exception in ResizeBuffers hook: 0x%08X", GetExceptionCode());
    }
    return oResizeBuffers(sc, bufferCount, width, height, newFormat, swapChainFlags);
}

bool InstallHooks() {
    Log("[BL1GOTYVR] Installing D3D11 hooks...");

    HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Log("[BL1GOTYVR] CoInitializeEx: 0x%08X", comInit);

    WNDCLASSEXA windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcA;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "BL1GOTYVR_HookWindow";
    RegisterClassExA(&windowClass);
    HWND window = CreateWindowExA(0, windowClass.lpszClassName, "", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) {
        Log("[BL1GOTYVR] ERROR: Temporary hook window creation failed: %lu", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = 100;
    desc.BufferDesc.Height = 100;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel = {};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain, &device, &featureLevel, &context);
    if (FAILED(hr)) {
        Log("[BL1GOTYVR] Hardware temp swapchain failed: 0x%08X; trying WARP", hr);
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain, &device, &featureLevel, &context);
    }
    if (FAILED(hr) || !swapChain) {
        Log("[BL1GOTYVR] ERROR: Temporary D3D11 swapchain creation failed: 0x%08X", hr);
        if (context) context->Release();
        if (device) device->Release();
        DestroyWindow(window);
        UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swapChain);
    void** contextVtable = *reinterpret_cast<void***>(context);
    void* presentTarget = vtable[8];
    void* resizeTarget = vtable[13];
    void* copyResourceTarget = contextVtable[47];
    void* resolveTarget = contextVtable[57];
    void* omSetRenderTargetsTarget = contextVtable[33];
    void* psSetShaderResourcesTarget = contextVtable[8];
    Log("[BL1GOTYVR] DXGI targets: Present=%p ResizeBuffers=%p", presentTarget, resizeTarget);

    // Tear down the temporary swapchain before patching global DXGI methods.
    // Its destruction may call DXGI internally and must not enter our hooks.
    context->Release();
    swapChain->Release();
    device->Release();
    DestroyWindow(window);
    UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);

    unsigned char presentBytes[16] = {};
    memcpy(presentBytes, presentTarget, sizeof(presentBytes));
    Log("[BL1GOTYVR] Present prehook bytes: "
        "%02X %02X %02X %02X %02X %02X %02X %02X "
        "%02X %02X %02X %02X %02X %02X %02X %02X",
        presentBytes[0], presentBytes[1], presentBytes[2], presentBytes[3],
        presentBytes[4], presentBytes[5], presentBytes[6], presentBytes[7],
        presentBytes[8], presentBytes[9], presentBytes[10], presentBytes[11],
        presentBytes[12], presentBytes[13], presentBytes[14], presentBytes[15]);
    uintptr_t detourAddress = reinterpret_cast<uintptr_t>(presentTarget);
    bool steamOverlayDetour = false;
    for (int depth = 0; depth < 4; ++depth) {
        unsigned char jump[5] = {};
        memcpy(jump, reinterpret_cast<void*>(detourAddress), sizeof(jump));
        if (jump[0] != 0xE9) break;
        int32_t displacement = 0;
        memcpy(&displacement, jump + 1, sizeof(displacement));
        const uintptr_t destination = detourAddress + 5 + displacement;
        HMODULE owner = nullptr;
        char ownerPath[MAX_PATH] = "<private relay>";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(destination), &owner) && owner) {
            GetModuleFileNameA(owner, ownerPath, MAX_PATH);
            if (strstr(ownerPath, "gameoverlayrenderer64.dll")) steamOverlayDetour = true;
        }
        Log("[BL1GOTYVR] Present prehook jump[%d]: %p -> %p owner=%s",
            depth, reinterpret_cast<void*>(detourAddress),
            reinterpret_cast<void*>(destination), ownerPath);
        detourAddress = destination;
    }
    if (steamOverlayDetour) {
        char dxgiPath[MAX_PATH] = {};
        GetModuleFileNameA(GetModuleHandleA("dxgi.dll"), dxgiPath, MAX_PATH);
        HANDLE file = CreateFileA(dxgiPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE mapping = file != INVALID_HANDLE_VALUE
            ? CreateFileMappingA(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr)
            : nullptr;
        const auto* cleanImage = mapping
            ? static_cast<const unsigned char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0))
            : nullptr;
        if (cleanImage) {
            const uintptr_t rva = reinterpret_cast<uintptr_t>(presentTarget) -
                                  reinterpret_cast<uintptr_t>(GetModuleHandleA("dxgi.dll"));
            DWORD oldProtect = 0;
            if (VirtualProtect(presentTarget, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(presentTarget, cleanImage + rva, 5);
                FlushInstructionCache(GetCurrentProcess(), presentTarget, 5);
                DWORD ignored = 0;
                VirtualProtect(presentTarget, 5, oldProtect, &ignored);
                Log("[BL1GOTYVR] Steam Overlay Present detour bypassed with clean DXGI prologue: "
                    "%02X %02X %02X %02X %02X", cleanImage[rva], cleanImage[rva + 1],
                    cleanImage[rva + 2], cleanImage[rva + 3], cleanImage[rva + 4]);
            }
            UnmapViewOfFile(cleanImage);
        } else {
            Log("[BL1GOTYVR] WARNING: Could not map clean dxgi.dll; Steam Overlay remains in chain");
        }
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    MH_STATUS presentStatus = MH_CreateHook(
        presentTarget, &HookedPresent, reinterpret_cast<void**>(&oPresent));
    Log("[BL1GOTYVR] Present trampoline: target=%p original=%p", presentTarget, oPresent);
    if (presentStatus == MH_OK) presentStatus = MH_QueueEnableHook(presentTarget);
    MH_STATUS copyStatus = MH_CreateHook(
        copyResourceTarget, &HookedCopyResource, reinterpret_cast<void**>(&oCopyResource));
    if (copyStatus == MH_OK) copyStatus = MH_QueueEnableHook(copyResourceTarget);
    MH_STATUS resolveStatus = MH_CreateHook(
        resolveTarget, &HookedResolveSubresource, reinterpret_cast<void**>(&oResolveSubresource));
    if (resolveStatus == MH_OK) resolveStatus = MH_QueueEnableHook(resolveTarget);
    MH_STATUS omStatus = MH_CreateHook(
        omSetRenderTargetsTarget, &HookedOMSetRenderTargets,
        reinterpret_cast<void**>(&oOMSetRenderTargets));
    if (omStatus == MH_OK) omStatus = MH_QueueEnableHook(omSetRenderTargetsTarget);
    MH_STATUS psSrvStatus = MH_CreateHook(
        psSetShaderResourcesTarget, &HookedPSSetShaderResources,
        reinterpret_cast<void**>(&oPSSetShaderResources));
    if (psSrvStatus == MH_OK) psSrvStatus = MH_QueueEnableHook(psSetShaderResourcesTarget);
    const MH_STATUS applyStatus = MH_ApplyQueued();
    if (presentStatus == MH_OK && applyStatus != MH_OK) presentStatus = applyStatus;
    if (presentStatus != MH_OK) {
        Log("[BL1GOTYVR] ERROR: Present hook failed: %s", MH_StatusToString(presentStatus));
        return false;
    }
    Log("[BL1GOTYVR] Composition hooks queued: CopyResource=%s ResolveSubresource=%s OMSetRT=%s PSSetSRV=%s Apply=%s",
        MH_StatusToString(copyStatus), MH_StatusToString(resolveStatus), MH_StatusToString(omStatus),
        MH_StatusToString(psSrvStatus), MH_StatusToString(applyStatus));
    Log("[BL1GOTYVR] ResizeBuffers hook deferred until Present path is stable (target=%p)",
        resizeTarget);
    Log("[BL1GOTYVR] Real DXGI Present hook installed");
    return true;
}

}} // namespace bl1gotyvr::d3d11
