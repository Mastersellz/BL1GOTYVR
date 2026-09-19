#pragma once

#include <d3d11.h>

#include <cstdint>

namespace bl1gotyvr::xr {

// Opens the filtered frame published by the ReShade add-on (see
// tools/reshade_capture). Returns an AddRef'd texture with the last
// post-effect image or nullptr when the add-on is not loaded or the frame does
// not match the requested extent.
class ReShadeFrameBridge {
public:
    static ReShadeFrameBridge& Instance();

    ID3D11Texture2D* Acquire(uint32_t width, uint32_t height);
    bool IsActive() const { return m_active; }

private:
    ReShadeFrameBridge() = default;

    void EnsureOpen();
    void CloseSharedTexture();

    void* m_mapping = nullptr;
    void* m_shared = nullptr;
    ID3D11Texture2D* m_openTexture = nullptr;
    uint64_t m_openHandle = 0;
    uint64_t m_lastSerial = 0;
    bool m_active = false;
    bool m_loggedAcquire = false;
};

} // namespace bl1gotyvr::xr
