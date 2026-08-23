#include "Overlay.hpp"

#include "../camera/CameraHook.hpp"
#include "../config/Config.hpp"
#include "../core/VRMod.hpp"
#include "../display/DisplayHooks.hpp"
#include "../input/InputHook.hpp"
#include "../player/ArmIKSystem.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <algorithm>
#include <atomic>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND, UINT, WPARAM, LPARAM);

namespace bl1gotyvr { namespace ui {
namespace {

bool s_initialized = false;
std::atomic<bool> s_visible{false};
ID3D11Device* s_device = nullptr;
ID3D11DeviceContext* s_context = nullptr;
ID3D11RenderTargetView* s_renderTarget = nullptr;
ID3D11Texture2D* s_backbufferIdentity = nullptr;
HWND s_window = nullptr;
WNDPROC s_originalWndProc = nullptr;
int s_selectedHand = 1;
float s_uiScale = 1.0f;
float s_statusUntil = 0.0f;
const char* s_status = nullptr;

LRESULT CALLBACK OverlayWndProc(HWND window, UINT message,
                                WPARAM wParam, LPARAM lParam) {
    if (s_visible.load(std::memory_order_acquire)) {
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
        const ImGuiIO& io = ImGui::GetIO();
        const bool mouseMessage = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
        const bool keyMessage = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        if ((mouseMessage && io.WantCaptureMouse) ||
            (keyMessage && io.WantCaptureKeyboard)) return TRUE;
    }
    return s_originalWndProc
        ? CallWindowProcW(s_originalWndProc, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

bool CreateRenderTarget(IDXGISwapChain* swapChain) {
    ID3D11Texture2D* backbuffer = nullptr;
    if (!swapChain || !s_device ||
        FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return false;
    const HRESULT result = s_device->CreateRenderTargetView(
        backbuffer, nullptr, &s_renderTarget);
    s_backbufferIdentity = SUCCEEDED(result) ? backbuffer : nullptr;
    backbuffer->Release();
    return SUCCEEDED(result);
}

bool Initialize(IDXGISwapChain* swapChain) {
    if (!swapChain || FAILED(swapChain->GetDevice(IID_PPV_ARGS(&s_device))))
        return false;
    s_device->GetImmediateContext(&s_context);
    DXGI_SWAP_CHAIN_DESC description = {};
    if (FAILED(swapChain->GetDesc(&description)) || !description.OutputWindow ||
        !CreateRenderTarget(swapChain)) return false;
    s_window = description.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigInputTrickleEventQueue = false;
    ImGui::StyleColorsDark();
    ID3D11Texture2D* backbuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        D3D11_TEXTURE2D_DESC backbufferDescription = {};
        backbuffer->GetDesc(&backbufferDescription);
        backbuffer->Release();
        s_uiScale = (std::max)(1.0f, (std::min)(
            static_cast<float>(backbufferDescription.Height) / 1440.0f, 2.75f));
    }
    ImGui::GetStyle().ScaleAllSizes(s_uiScale);
    io.FontGlobalScale = s_uiScale;
    ImGui_ImplWin32_Init(s_window);
    ImGui_ImplDX11_Init(s_device, s_context);
    s_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        s_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(OverlayWndProc)));
    Log("[Overlay] In-game tuning initialized; press F10 to toggle");
    return true;
}

void DrawPositionSliders(float& forward, float& right, float& up,
                         const char* id) {
    ImGui::PushID(id);
    ImGui::SliderFloat("Forward", &forward, -50.0f, 50.0f, "%.1f uu");
    ImGui::SliderFloat("Right", &right, -50.0f, 50.0f, "%.1f uu");
    ImGui::SliderFloat("Up", &up, -50.0f, 50.0f, "%.1f uu");
    ImGui::PopID();
}

void ResetTuning(config::Settings& settings) {
    settings.weapon_offset_forward = 0.0f;
    settings.weapon_offset_right = 0.0f;
    settings.weapon_offset_up = 0.0f;
    settings.weapon_rotation_pitch = 0.0f;
    settings.weapon_rotation_yaw = 0.0f;
    settings.weapon_rotation_roll = 0.0f;
    settings.left_hand_offset_forward = 0.0f;
    settings.left_hand_offset_right = 0.0f;
    settings.left_hand_offset_up = 0.0f;
    settings.left_hand_rotation_pitch = 0.0f;
    settings.left_hand_rotation_yaw = 0.0f;
    settings.left_hand_rotation_roll = 0.0f;
    settings.right_hand_offset_forward = 0.0f;
    settings.right_hand_offset_right = 0.0f;
    settings.right_hand_offset_up = 0.0f;
    settings.right_hand_rotation_pitch = 0.0f;
    settings.right_hand_rotation_yaw = 0.0f;
    settings.right_hand_rotation_roll = 0.0f;
    settings.dot_horizontal_offset = 0.0f;
    settings.dot_vertical_offset = 0.0f;
}

void DrawUi() {
    auto& settings = config::Get();
    ImGui::SetNextWindowSize(ImVec2(500.0f * s_uiScale, 620.0f * s_uiScale),
                                 ImGuiCond_FirstUseEver);
    ImGui::Begin("BL1 GOTY VR Tuning");
    ImGui::Text("F10: show/hide | Changes apply live");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Weapon model", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Position and rotation follow the right controller. Bullets and dot are unchanged.");
        DrawPositionSliders(settings.weapon_offset_forward,
                            settings.weapon_offset_right,
                            settings.weapon_offset_up, "weapon");
        ImGui::PushID("weapon_rotation");
        ImGui::SliderFloat("Pitch", &settings.weapon_rotation_pitch,
                           -180.0f, 180.0f, "%+.1f deg");
        ImGui::SliderFloat("Yaw", &settings.weapon_rotation_yaw,
                           -180.0f, 180.0f, "%+.1f deg");
        ImGui::SliderFloat("Roll", &settings.weapon_rotation_roll,
                           -180.0f, 180.0f, "%+.1f deg");
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Hand position", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::RadioButton("Left", &s_selectedHand, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Right", &s_selectedHand, 1);
        if (s_selectedHand == 0) {
            DrawPositionSliders(settings.left_hand_offset_forward,
                                settings.left_hand_offset_right,
                                settings.left_hand_offset_up, "left_hand");
            ImGui::PushID("left_hand_rotation");
            ImGui::SliderFloat("Pitch", &settings.left_hand_rotation_pitch,
                               -180.0f, 180.0f, "%+.1f deg");
            ImGui::SliderFloat("Yaw", &settings.left_hand_rotation_yaw,
                               -180.0f, 180.0f, "%+.1f deg");
            ImGui::SliderFloat("Roll", &settings.left_hand_rotation_roll,
                               -180.0f, 180.0f, "%+.1f deg");
            ImGui::PopID();
        } else {
            DrawPositionSliders(settings.right_hand_offset_forward,
                                settings.right_hand_offset_right,
                                settings.right_hand_offset_up, "right_hand");
            ImGui::PushID("right_hand_rotation");
            ImGui::SliderFloat("Pitch", &settings.right_hand_rotation_pitch,
                               -180.0f, 180.0f, "%+.1f deg");
            ImGui::SliderFloat("Yaw", &settings.right_hand_rotation_yaw,
                               -180.0f, 180.0f, "%+.1f deg");
            ImGui::SliderFloat("Roll", &settings.right_hand_rotation_roll,
                               -180.0f, 180.0f, "%+.1f deg");
            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader("Aim dot", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Target distance (m)", &settings.aim_convergence_m,
                           1.0f, 100.0f, "%.1f m");
        ImGui::TextWrapped(
            "The active weapon profile stores aim pitch/yaw automatically. "
            "Use Ctrl+numpad tuning to move bullets and dot together.");
    }

    ImGui::Separator();
    if (ImGui::Button("Save settings")) {
        s_status = config::SaveLoaded() ? "Saved to BL1GOTYVR.ini" : "Save failed";
        s_statusUntil = static_cast<float>(ImGui::GetTime() + 3.0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset offsets")) {
        ResetTuning(settings);
        s_status = "Offsets reset; press Save settings to persist";
        s_statusUntil = static_cast<float>(ImGui::GetTime() + 3.0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Recenter")) {
        camera::RequestRecenter();
        player::ArmIKSystem::Instance().RequestCalibrationReset();
        input::InputHook::Instance().RequestMotionCalibrationReset();
        s_status = "Recenter requested";
        s_statusUntil = static_cast<float>(ImGui::GetTime() + 3.0);
    }
    if (s_status && ImGui::GetTime() < s_statusUntil)
        ImGui::TextWrapped("%s", s_status);
    ImGui::End();
}

} // namespace

void OnPresent(IDXGISwapChain* swapChain) {
    if (!s_initialized) {
        s_initialized = Initialize(swapChain);
        if (!s_initialized) return;
    }

    ID3D11Texture2D* backbuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        if (s_renderTarget && backbuffer != s_backbufferIdentity) {
            s_renderTarget->Release();
            s_renderTarget = nullptr;
            s_backbufferIdentity = nullptr;
        }
        backbuffer->Release();
    }
    if (!s_renderTarget && !CreateRenderTarget(swapChain)) return;

    static bool f10WasDown = false;
    const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10Down && !f10WasDown) {
        const bool visible = !s_visible.load(std::memory_order_relaxed);
        s_visible.store(visible, std::memory_order_release);
        ImGui::GetIO().MouseDrawCursor = visible;
    }
    f10WasDown = f10Down;
    if (!s_visible.load(std::memory_order_acquire)) return;

    // The game sees a spoofed 4096x4096 client rect while the physical cursor
    // is constrained by the real monitor. Release the game clip and map the
    // physical monitor coordinates into ImGui's logical display space.
    ClipCursor(nullptr);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    POINT cursor = {};
    RECT monitor = {};
    ImGuiIO& io = ImGui::GetIO();
    if (GetCursorPos(&cursor) && display::GetPhysicalMonitorRect(s_window, monitor)) {
        const float width = static_cast<float>(monitor.right - monitor.left);
        const float height = static_cast<float>(monitor.bottom - monitor.top);
        const float normalizedX = (cursor.x - monitor.left) / width;
        const float normalizedY = (cursor.y - monitor.top) / height;
        io.AddMousePosEvent(normalizedX * io.DisplaySize.x,
                            normalizedY * io.DisplaySize.y);
        static bool loggedMapping = false;
        if (!loggedMapping) {
            Log("[Overlay] Cursor mapping: physical=%dx%d logical=%.0fx%.0f",
                monitor.right - monitor.left, monitor.bottom - monitor.top,
                io.DisplaySize.x, io.DisplaySize.y);
            loggedMapping = true;
        }
    }
    ImGui::NewFrame();
    DrawUi();
    ImGui::Render();
    s_context->OMSetRenderTargets(1, &s_renderTarget, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

bool IsVisible() {
    return s_visible.load(std::memory_order_acquire);
}

}} // namespace bl1gotyvr::ui
