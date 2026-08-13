#include "HudBlitter.hpp"

#include "../core/VRMod.hpp"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace bl1gotyvr { namespace render {
namespace {

DXGI_FORMAT TypedColorFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

template<typename T>
void Release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

} // namespace

HudBlitter& HudBlitter::Instance() {
    static HudBlitter instance;
    return instance;
}

bool HudBlitter::Initialize(ID3D11Device* device) {
    if (!device) return false;
    if (m_vertexShader && m_pixelShader && m_differencePixelShader &&
        m_constantBuffer && m_sampler && m_blend && m_depthDisabled && m_rasterizer) {
        return m_device == device;
    }
    if (m_initializationAttempted) return false;
    m_initializationAttempted = true;
    m_device = device;

    static constexpr char vertexSource[] = R"(
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Output main(uint vertexId : SV_VertexID) {
    Output output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv.x * 2.0 - 1.0,
                             1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}
)";
    static constexpr char pixelSource[] = R"(
Texture2D hudTexture : register(t0);
SamplerState hudSampler : register(s0);
cbuffer HudConstants : register(b0) { float opacity; float3 padding; };
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return hudTexture.Sample(hudSampler, uv) * opacity;
}
)";
    // Estimate the source alpha for either additive or subtractive UI blending,
    // then reconstruct premultiplied HUD color from final minus world.
    static constexpr char differencePixelSource[] = R"(
Texture2D finalTexture : register(t0);
Texture2D worldTexture : register(t1);
SamplerState hudSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float3 finalColor = finalTexture.Sample(hudSampler, uv).rgb;
    float3 worldColor = worldTexture.Sample(hudSampler, uv).rgb;
    float3 increase = saturate(
        (finalColor - worldColor) / max(1.0 - worldColor, 0.001));
    float3 decrease = saturate(
        (worldColor - finalColor) / max(worldColor, 0.001));
    float alpha = max(max(increase.r, increase.g), increase.b);
    alpha = max(alpha, max(max(decrease.r, decrease.g), decrease.b));
    if (alpha < 0.004) return 0.0;
    float3 premultiplied = saturate(
        finalColor - worldColor * (1.0 - alpha));
    return float4(premultiplied, alpha);
}
)";

    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    ID3DBlob* differenceBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT result = D3DCompile(vertexSource, sizeof(vertexSource), "BL1HudVS", nullptr,
                                nullptr, "main", "vs_5_0", 0, 0, &vertexBlob, &errors);
    if (errors) {
        if (FAILED(result)) Log("[HUD] Vertex shader compile failed: %.*s",
            static_cast<int>(errors->GetBufferSize()),
            static_cast<const char*>(errors->GetBufferPointer()));
        Release(errors);
    }
    if (FAILED(result)) return false;
    result = D3DCompile(pixelSource, sizeof(pixelSource), "BL1HudPS", nullptr,
                        nullptr, "main", "ps_5_0", 0, 0, &pixelBlob, &errors);
    if (errors) {
        if (FAILED(result)) Log("[HUD] Pixel shader compile failed: %.*s",
            static_cast<int>(errors->GetBufferSize()),
            static_cast<const char*>(errors->GetBufferPointer()));
        Release(errors);
    }
    if (FAILED(result)) {
        Release(vertexBlob);
        return false;
    }
    result = D3DCompile(differencePixelSource, sizeof(differencePixelSource),
                        "BL1HudDifferencePS", nullptr, nullptr, "main", "ps_5_0",
                        0, 0, &differenceBlob, &errors);
    if (errors) {
        if (FAILED(result)) Log("[HUD] Difference shader compile failed: %.*s",
            static_cast<int>(errors->GetBufferSize()),
            static_cast<const char*>(errors->GetBufferPointer()));
        Release(errors);
    }
    if (FAILED(result)) {
        Release(vertexBlob);
        Release(pixelBlob);
        return false;
    }

    result = device->CreateVertexShader(vertexBlob->GetBufferPointer(),
        vertexBlob->GetBufferSize(), nullptr, &m_vertexShader);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &m_pixelShader);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        differenceBlob->GetBufferPointer(), differenceBlob->GetBufferSize(), nullptr,
        &m_differencePixelShader);
    Release(vertexBlob);
    Release(pixelBlob);
    Release(differenceBlob);

    D3D11_BUFFER_DESC constantDesc = {};
    constantDesc.ByteWidth = 16;
    constantDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(result)) result = device->CreateBuffer(
        &constantDesc, nullptr, &m_constantBuffer);

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(result)) result = device->CreateSamplerState(&samplerDesc, &m_sampler);

    D3D11_BLEND_DESC blendDesc = {};
    auto& blendTarget = blendDesc.RenderTarget[0];
    blendTarget.BlendEnable = TRUE;
    blendTarget.SrcBlend = D3D11_BLEND_ONE;
    blendTarget.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendTarget.BlendOp = D3D11_BLEND_OP_ADD;
    blendTarget.SrcBlendAlpha = D3D11_BLEND_ONE;
    blendTarget.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendTarget.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendTarget.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(result)) result = device->CreateBlendState(&blendDesc, &m_blend);

    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;
    if (SUCCEEDED(result)) result = device->CreateDepthStencilState(
        &depthDesc, &m_depthDisabled);

    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (SUCCEEDED(result)) result = device->CreateRasterizerState(
        &rasterizerDesc, &m_rasterizer);
    if (FAILED(result)) {
        Log("[HUD] D3D11 resource creation failed: 0x%08X", result);
        Shutdown();
        m_initializationAttempted = true;
        return false;
    }
    Log("[HUD] Alpha-estimation blitter ready");
    return true;
}

bool HudBlitter::Blit(ID3D11Device* device, ID3D11DeviceContext* context,
                      ID3D11Texture2D* source, ID3D11Texture2D* target,
                      float opacity) {
    return Initialize(device) && Render(device, context, source, nullptr, target,
                                        opacity, m_pixelShader);
}

bool HudBlitter::ExtractDifference(ID3D11Device* device,
                                   ID3D11DeviceContext* context,
                                   ID3D11Texture2D* finalFrame,
                                   ID3D11Texture2D* worldFrame,
                                   ID3D11Texture2D* target) {
    return Initialize(device) && Render(device, context, finalFrame, worldFrame,
                                        target, 1.0f, m_differencePixelShader);
}

bool HudBlitter::Render(ID3D11Device* device, ID3D11DeviceContext* context,
                        ID3D11Texture2D* source, ID3D11Texture2D* secondarySource,
                        ID3D11Texture2D* target, float opacity,
                        ID3D11PixelShader* pixelShader) {
    if (!device || !context || !source || !target || !pixelShader) return false;
    opacity = (std::max)(0.0f, (std::min)(opacity, 1.0f));

    D3D11_TEXTURE2D_DESC sourceDesc = {}, secondaryDesc = {}, targetDesc = {};
    source->GetDesc(&sourceDesc);
    target->GetDesc(&targetDesc);
    if (secondarySource) secondarySource->GetDesc(&secondaryDesc);
    if (sourceDesc.SampleDesc.Count != 1 || targetDesc.SampleDesc.Count != 1 ||
        (secondarySource && (secondaryDesc.SampleDesc.Count != 1 ||
         secondaryDesc.Width != sourceDesc.Width ||
         secondaryDesc.Height != sourceDesc.Height ||
         secondaryDesc.Format != sourceDesc.Format))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sourceViewDesc = {};
    sourceViewDesc.Format = TypedColorFormat(sourceDesc.Format);
    sourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sourceViewDesc.Texture2D.MipLevels = 1;
    D3D11_SHADER_RESOURCE_VIEW_DESC secondaryViewDesc = sourceViewDesc;
    if (secondarySource) secondaryViewDesc.Format = TypedColorFormat(secondaryDesc.Format);
    D3D11_RENDER_TARGET_VIEW_DESC targetViewDesc = {};
    targetViewDesc.Format = TypedColorFormat(targetDesc.Format);
    targetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    ID3D11ShaderResourceView* sourceView = nullptr;
    ID3D11ShaderResourceView* secondaryView = nullptr;
    ID3D11RenderTargetView* targetView = nullptr;
    if (FAILED(device->CreateShaderResourceView(source, &sourceViewDesc, &sourceView)) ||
        (secondarySource && FAILED(device->CreateShaderResourceView(
            secondarySource, &secondaryViewDesc, &secondaryView))) ||
        FAILED(device->CreateRenderTargetView(target, &targetViewDesc, &targetView))) {
        Release(sourceView);
        Release(secondaryView);
        Release(targetView);
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        Release(sourceView);
        Release(secondaryView);
        Release(targetView);
        return false;
    }
    const float constants[4] = {opacity, 0.0f, 0.0f, 0.0f};
    memcpy(mapped.pData, constants, sizeof(constants));
    context->Unmap(m_constantBuffer, 0);

    ID3D11RenderTargetView* oldTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDepth = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldTargets, &oldDepth);
    ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldBlendFactor[4] = {};
    UINT oldSampleMask = 0;
    context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    ID3D11DepthStencilState* oldDepthState = nullptr;
    UINT oldStencilRef = 0;
    context->OMGetDepthStencilState(&oldDepthState, &oldStencilRef);
    ID3D11RasterizerState* oldRasterizer = nullptr;
    context->RSGetState(&oldRasterizer);
    UINT oldViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    context->RSGetViewports(&oldViewportCount, oldViewports);
    ID3D11InputLayout* oldLayout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetInputLayout(&oldLayout);
    context->IAGetPrimitiveTopology(&oldTopology);
    ID3D11VertexShader* oldVs = nullptr;
    ID3D11GeometryShader* oldGs = nullptr;
    ID3D11PixelShader* oldPs = nullptr;
    context->VSGetShader(&oldVs, nullptr, nullptr);
    context->GSGetShader(&oldGs, nullptr, nullptr);
    context->PSGetShader(&oldPs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldResources[2] = {};
    ID3D11SamplerState* oldSampler = nullptr;
    ID3D11Buffer* oldConstantBuffer = nullptr;
    context->PSGetShaderResources(0, 2, oldResources);
    context->PSGetSamplers(0, 1, &oldSampler);
    context->PSGetConstantBuffers(0, 1, &oldConstantBuffer);

    const float transparent[4] = {};
    context->ClearRenderTargetView(targetView, transparent);
    context->OMSetRenderTargets(1, &targetView, nullptr);
    context->OMSetBlendState(m_blend, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depthDisabled, 0);
    context->RSSetState(m_rasterizer);
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(targetDesc.Width);
    viewport.Height = static_cast<float>(targetDesc.Height);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(pixelShader, nullptr, 0);
    ID3D11ShaderResourceView* resources[2] = {sourceView, secondaryView};
    context->PSSetShaderResources(0, 2, resources);
    context->PSSetSamplers(0, 1, &m_sampler);
    context->PSSetConstantBuffers(0, 1, &m_constantBuffer);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* nullResources[2] = {};
    context->PSSetShaderResources(0, 2, nullResources);
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                oldTargets, oldDepth);
    context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    context->OMSetDepthStencilState(oldDepthState, oldStencilRef);
    context->RSSetState(oldRasterizer);
    context->RSSetViewports(oldViewportCount, oldViewports);
    context->IASetInputLayout(oldLayout);
    context->IASetPrimitiveTopology(oldTopology);
    context->VSSetShader(oldVs, nullptr, 0);
    context->GSSetShader(oldGs, nullptr, 0);
    context->PSSetShader(oldPs, nullptr, 0);
    context->PSSetShaderResources(0, 2, oldResources);
    context->PSSetSamplers(0, 1, &oldSampler);
    context->PSSetConstantBuffers(0, 1, &oldConstantBuffer);

    for (auto*& value : oldTargets) Release(value);
    Release(oldDepth);
    Release(oldBlend);
    Release(oldDepthState);
    Release(oldRasterizer);
    Release(oldLayout);
    Release(oldVs);
    Release(oldGs);
    Release(oldPs);
    for (auto*& value : oldResources) Release(value);
    Release(oldSampler);
    Release(oldConstantBuffer);
    Release(sourceView);
    Release(secondaryView);
    Release(targetView);
    return true;
}

void HudBlitter::Shutdown() {
    Release(m_rasterizer);
    Release(m_depthDisabled);
    Release(m_blend);
    Release(m_sampler);
    Release(m_constantBuffer);
    Release(m_differencePixelShader);
    Release(m_pixelShader);
    Release(m_vertexShader);
    m_device = nullptr;
    m_initializationAttempted = false;
}

}} // namespace bl1gotyvr::render
