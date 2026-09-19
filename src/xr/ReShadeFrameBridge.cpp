#include "xr/ReShadeFrameBridge.hpp"

#include <d3d11_1.h>

#include "core/VRMod.hpp"
#include "d3d11/D3D11Hooks.hpp"
#include "reshade/ReShadeSharedFrame.hpp"

#include <Windows.h>

namespace bl1gotyvr::xr {

ReShadeFrameBridge& ReShadeFrameBridge::Instance() {
    static ReShadeFrameBridge instance;
    return instance;
}

void ReShadeFrameBridge::EnsureOpen() {
    if (m_mapping) return;
    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(reshade::SharedFrame),
                                   reshade::kSharedFrameName);
    if (!m_mapping) return;
    m_shared = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                             sizeof(reshade::SharedFrame));
    if (!m_shared) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return;
    }
    Log("[ReShade] Filtered-frame bridge mapped");
}

void ReShadeFrameBridge::CloseSharedTexture() {
    if (m_openTexture) {
        m_openTexture->Release();
        m_openTexture = nullptr;
    }
    m_openHandle = 0;
}

ID3D11Texture2D* ReShadeFrameBridge::Acquire(uint32_t width, uint32_t height) {
    EnsureOpen();
    auto* shared = static_cast<reshade::SharedFrame*>(m_shared);
    if (!shared) return nullptr;

    // Seqlock read: retry while the add-on is writing.
    reshade::SharedFrame snapshot = {};
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int32_t before = shared->sequence;
        if (before & 1) continue;
        snapshot = *shared;
        if (shared->sequence == before) break;
        snapshot = {};
    }
    if (snapshot.magic != static_cast<int32_t>(reshade::kSharedFrameMagic) ||
        !snapshot.sharedHandle || snapshot.frameSerial == m_lastSerial ||
        snapshot.width != width || snapshot.height != height) {
        m_active = snapshot.magic == static_cast<int32_t>(reshade::kSharedFrameMagic);
        return nullptr;
    }

    if (m_openHandle != snapshot.sharedHandle) {
        CloseSharedTexture();
        ID3D11Device* device = d3d11::GetGameDevice();
        if (device) {
            ID3D11Device1* device1 = nullptr;
            if (SUCCEEDED(device->QueryInterface(
                    __uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1))) &&
                device1) {
                device1->OpenSharedResource1(
                    reinterpret_cast<HANDLE>(snapshot.sharedHandle),
                    __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&m_openTexture));
                device1->Release();
            }
            if (!m_openTexture) {
                device->OpenSharedResource(
                    reinterpret_cast<HANDLE>(snapshot.sharedHandle),
                    __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&m_openTexture));
            }
        }
        if (!m_openTexture) return nullptr;
        m_openHandle = snapshot.sharedHandle;
    }

    m_lastSerial = snapshot.frameSerial;
    m_active = true;
    m_openTexture->AddRef();
    if (!m_loggedAcquire) {
        m_loggedAcquire = true;
        Log("[ReShade] Using the filtered frame bridge: %ux%u fmt=%u serial=%llu",
            snapshot.width, snapshot.height, snapshot.format,
            static_cast<unsigned long long>(snapshot.frameSerial));
    }
    return m_openTexture;
}

} // namespace bl1gotyvr::xr
