#pragma once

#include <d3d11.h>

namespace bl1gotyvr { namespace render {

class HudBlitter {
public:
    static HudBlitter& Instance();

    bool Blit(ID3D11Device* device, ID3D11DeviceContext* context,
              ID3D11Texture2D* source, ID3D11Texture2D* target,
              float opacity);
    bool Composite(ID3D11Device* device, ID3D11DeviceContext* context,
                   ID3D11Texture2D* source, ID3D11Texture2D* target,
                   const D3D11_VIEWPORT& viewport, float opacity,
                   const float protectedDot[4], float targetAspect);
    bool ExtractDifference(ID3D11Device* device, ID3D11DeviceContext* context,
                           ID3D11Texture2D* finalFrame,
                           ID3D11Texture2D* worldFrame,
                           ID3D11Texture2D* target);
    void Shutdown();

private:
    HudBlitter() = default;
    bool Initialize(ID3D11Device* device);
    bool Render(ID3D11Device* device, ID3D11DeviceContext* context,
                 ID3D11Texture2D* source, ID3D11Texture2D* secondarySource,
                 ID3D11Texture2D* target, float opacity,
                 ID3D11PixelShader* pixelShader,
                 const D3D11_VIEWPORT* viewport, bool clearTarget,
                 const float protectedDot[4], float targetAspect);

    ID3D11Device* m_device = nullptr;
    ID3D11VertexShader* m_vertexShader = nullptr;
    ID3D11PixelShader* m_pixelShader = nullptr;
    ID3D11PixelShader* m_differencePixelShader = nullptr;
    ID3D11Buffer* m_constantBuffer = nullptr;
    ID3D11SamplerState* m_sampler = nullptr;
    ID3D11BlendState* m_blend = nullptr;
    ID3D11DepthStencilState* m_depthDisabled = nullptr;
    ID3D11RasterizerState* m_rasterizer = nullptr;
    bool m_initializationAttempted = false;
};

}} // namespace bl1gotyvr::render
