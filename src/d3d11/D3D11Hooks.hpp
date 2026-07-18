#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>

namespace bl1gotyvr { namespace d3d11 {

bool InstallHooks();
ID3D11Device* GetGameDevice();
ID3D11DeviceContext* GetGameContext();
IDXGISwapChain* GetGameSwapChain();
ID3D11Texture2D* GetLatestComposedTexture();
ID3D11Texture2D* GetLatestSceneRenderTarget();
ID3D11Texture2D* GetLatestSdrRenderTarget();
int GetTrackedRenderTargetCount();
ID3D11Texture2D* GetTrackedRenderTarget(int index);
ID3D11Texture2D* GetLatestTonemapSource();
ID3D11Texture2D* GetPostTonemapTexture();
ID3D11Texture2D* AcquireCurrentBackbuffer(IDXGISwapChain* swapChain, UINT* bufferIndex = nullptr);

}} // namespace bl1gotyvr::d3d11
