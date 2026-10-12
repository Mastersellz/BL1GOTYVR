#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

int main() {
    WNDCLASSEXA windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcA;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "BL1GOTYVR_CleanPresent";
    if (!RegisterClassExA(&windowClass)) return 1;

    HWND window = CreateWindowExA(0, windowClass.lpszClassName, "", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) return 2;

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
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
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &desc, &swapChain, &device, &featureLevel, &context);
    if (FAILED(result) || !swapChain) {
        std::printf("D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", result);
        return 3;
    }

    void* present = (*reinterpret_cast<void***>(swapChain))[8];
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    const auto* bytes = reinterpret_cast<const unsigned char*>(present);
    std::printf("dxgi=%p Present=%p RVA=0x%llX\n", dxgi, present,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(present) -
                                        reinterpret_cast<uintptr_t>(dxgi)));
    std::printf("bytes:");
    for (int i = 0; i < 32; ++i) std::printf(" %02X", bytes[i]);
    std::printf("\n");

    context->Release();
    device->Release();
    swapChain->Release();
    DestroyWindow(window);
    UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);
    return 0;
}
