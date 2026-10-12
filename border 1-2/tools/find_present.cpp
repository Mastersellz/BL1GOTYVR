#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

// Minimal DLL that logs the real Present address
extern "C" __declspec(dllexport) void __cdecl FindPresent() {
    // Create a temp device to get the real vtable
    WNDCLASSEXA wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "FindPresent_Temp";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPED, 0,0,1,1, 0,0,wc.hInstance, 0);

    // Get system d3d11.dll
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    char d3d11Path[MAX_PATH];
    sprintf(d3d11Path, "%s\\d3d11.dll", sysDir);
    HMODULE hD3D11 = LoadLibraryA(d3d11Path);

    typedef HRESULT (WINAPI *PFN_D3D11CreateDeviceAndSwapChain)(
        IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
        IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    auto pCreate = (PFN_D3D11CreateDeviceAndSwapChain)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");
    if (!pCreate) { FreeLibrary(hD3D11); DestroyWindow(hwnd); return; }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = 2; sd.BufferDesc.Height = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2; sd.OutputWindow = hwnd; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    ID3D11Device* dev; ID3D11DeviceContext* ctx; IDXGISwapChain* sc;
    HRESULT hr = pCreate(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);

    if (SUCCEEDED(hr) && sc) {
        void** vtable = *(void***)sc;
        void* presentAddr = vtable[8];
        // Log to a file
        FILE* f = fopen("F:\\SteamLibrary\\steamapps\\common\\BorderlandsGOTYEnhanced\\Binaries\\Win64\\present_hook.txt", "w");
        if (f) {
            fprintf(f, "Real Present: %p\n", presentAddr);
            for (int i = 0; i < 20; i++) fprintf(f, "vtable[%d] = %p\n", i, vtable[i]);
            fclose(f);
        }
        sc->Release(); dev->Release(); ctx->Release();
    }
    FreeLibrary(hD3D11);
    DestroyWindow(hwnd);
    FreeLibrary(hD3D11);
}
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
