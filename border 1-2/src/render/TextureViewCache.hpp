#pragma once
#include <d3d11.h>
#include <cstdint>

namespace bl1gotyvr::render {

// Render-thread-only, bounded caches. Each view owns its resource reference,
// preventing pointer reuse while cached. Clear before resize/device recovery.
class TextureViewCache {
public:
    TextureViewCache() = default;
    TextureViewCache(const TextureViewCache&) = delete;
    TextureViewCache& operator=(const TextureViewCache&) = delete;
    ~TextureViewCache() = default; // owner clears outside DllMain/loader lock

    // Returned views are borrowed until eviction/Clear (not caller-owned).
    HRESULT ShaderResource(ID3D11Device* device, ID3D11Texture2D* texture,
                           DXGI_FORMAT format, ID3D11ShaderResourceView** output) {
        *output = nullptr;
        auto& slot = Find(m_sources, texture, format);
        if (!slot.view) {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
            desc.Format = format;
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = 1;
            const HRESULT hr = device->CreateShaderResourceView(texture, &desc, &slot.view);
            if (FAILED(hr)) return hr;
            slot.texture = texture;
            slot.format = format;
        }
        *output = slot.view;
        return S_OK;
    }

    HRESULT RenderTarget(ID3D11Device* device, ID3D11Texture2D* texture,
                         DXGI_FORMAT format, ID3D11RenderTargetView** output) {
        *output = nullptr;
        auto& slot = Find(m_targets, texture, format);
        if (!slot.view) {
            D3D11_RENDER_TARGET_VIEW_DESC desc = {};
            desc.Format = format;
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            const HRESULT hr = device->CreateRenderTargetView(texture, &desc, &slot.view);
            if (FAILED(hr)) return hr;
            slot.texture = texture;
            slot.format = format;
        }
        *output = slot.view;
        return S_OK;
    }

    void Clear() {
        for (auto& slot : m_sources) Release(slot);
        for (auto& slot : m_targets) Release(slot);
        m_clock = 0;
    }

private:
    template<class View> struct Slot {
        ID3D11Texture2D* texture = nullptr; // owned by view
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        View* view = nullptr;
        uint64_t used = 0;
    };
    template<class View> static void Release(Slot<View>& slot) {
        if (slot.view) slot.view->Release();
        slot = {};
    }
    template<class View> Slot<View>& Find(Slot<View> (&slots)[8],
                                         ID3D11Texture2D* texture, DXGI_FORMAT format) {
        Slot<View>* oldest = &slots[0];
        for (auto& slot : slots) {
            if (slot.view && slot.texture == texture && slot.format == format) {
                slot.used = ++m_clock;
                return slot;
            }
            if (slot.used < oldest->used) oldest = &slot;
        }
        Release(*oldest);
        oldest->used = ++m_clock;
        return *oldest;
    }
    Slot<ID3D11ShaderResourceView> m_sources[8] = {};
    Slot<ID3D11RenderTargetView> m_targets[8] = {};
    uint64_t m_clock = 0;
};

} // namespace bl1gotyvr::render
