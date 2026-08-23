#include "XInputBridge.hpp"

#include "../core/VRMod.hpp"

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bl1gotyvr { namespace input {
namespace {

bool IsXInputModule(const char* name) {
    return name != nullptr && _strnicmp(name, "xinput", 6) == 0;
}

SHORT ToThumb(float value) {
    value = (std::max)(-1.0f, (std::min)(value, 1.0f));
    return static_cast<SHORT>(std::lround(
        value < 0.0f ? value * 32768.0f : value * 32767.0f));
}

BYTE ToTrigger(float value) {
    value = (std::max)(0.0f, (std::min)(value, 1.0f));
    return static_cast<BYTE>(std::lround(value * 255.0f));
}

SHORT MergeThumb(SHORT physical, SHORT vr) {
    return std::abs(static_cast<int>(vr)) >
        std::abs(static_cast<int>(physical)) ? vr : physical;
}

} // namespace

XInputBridge& XInputBridge::Instance() {
    static XInputBridge instance;
    return instance;
}

bool XInputBridge::Initialize() {
    if (IsInstalled()) return true;
    PatchHostImports();
    HookLoadedXInputExports();
    if (!IsInstalled()) {
        Log("[XInput] No hookable XInput path; keyboard/mouse fallback active");
        return false;
    }
    Log("[XInput] Quest gamepad bridge active: IAT=%zu exports=%zu",
        m_slots.size(), m_inlineTargets.size());
    return true;
}

bool XInputBridge::HookLoadedXInputExports() {
    constexpr const wchar_t* moduleNames[] = {
        L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll"};
    HMODULE module = nullptr;
    for (const wchar_t* name : moduleNames) {
        module = GetModuleHandleW(name);
        if (module != nullptr) break;
    }
    if (module == nullptr) return false;

    const MH_STATUS initializeResult = MH_Initialize();
    if (initializeResult != MH_OK &&
        initializeResult != MH_ERROR_ALREADY_INITIALIZED) {
        return false;
    }

    auto createHook = [&](void* target, void* replacement,
                          XInputGetStateFn& original) {
        if (target == nullptr ||
            std::find(m_inlineTargets.begin(), m_inlineTargets.end(), target) !=
                m_inlineTargets.end()) {
            return;
        }
        const MH_STATUS createResult = MH_CreateHook(
            target, replacement, reinterpret_cast<void**>(&original));
        if (createResult == MH_OK && MH_EnableHook(target) == MH_OK) {
            m_inlineTargets.push_back(target);
        }
    };
    createHook(
        reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")),
        reinterpret_cast<void*>(&HookedXInputGetState), m_originalGetState);
    createHook(
        reinterpret_cast<void*>(GetProcAddress(module, MAKEINTRESOURCEA(100))),
        reinterpret_cast<void*>(&HookedXInputGetStateEx), m_originalGetStateEx);
    return !m_inlineTargets.empty();
}

bool XInputBridge::PatchHostImports() {
    auto* module = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (module == nullptr) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0) return false;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        module + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const char* moduleName = reinterpret_cast<const char*>(
            module + descriptor->Name);
        if (!IsXInputModule(moduleName)) continue;
        auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            module + descriptor->FirstThunk);
        auto* originalThunk = descriptor->OriginalFirstThunk != 0
            ? reinterpret_cast<IMAGE_THUNK_DATA64*>(
                module + descriptor->OriginalFirstThunk)
            : nullptr;
        if (originalThunk == nullptr) continue;

        for (std::size_t index = 0;
             originalThunk[index].u1.AddressOfData != 0; ++index) {
            bool getState = false;
            bool extended = false;
            if (IMAGE_SNAP_BY_ORDINAL64(originalThunk[index].u1.Ordinal)) {
                const WORD ordinal = static_cast<WORD>(IMAGE_ORDINAL64(
                    originalThunk[index].u1.Ordinal));
                // XInput 1.3 exports the standard GetState entry as ordinal 2.
                // UE3 imports it by ordinal rather than by its public name.
                extended = ordinal == 100;
                getState = ordinal == 2 || extended;
            } else {
                const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    module + originalThunk[index].u1.AddressOfData);
                const char* importName =
                    reinterpret_cast<const char*>(import->Name);
                getState = std::strcmp(importName, "XInputGetState") == 0;
                extended = std::strcmp(importName, "XInputGetStateEx") == 0;
                getState = getState || extended;
            }
            if (!getState) continue;

            void** slot = reinterpret_cast<void**>(&firstThunk[index].u1.Function);
            void* original = *slot;
            void* replacement = extended
                ? reinterpret_cast<void*>(&HookedXInputGetStateEx)
                : reinterpret_cast<void*>(&HookedXInputGetState);
            if (original == nullptr ||
                original == replacement) {
                continue;
            }
            DWORD oldProtection = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE,
                                &oldProtection)) {
                continue;
            }
            *slot = replacement;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            m_slots.push_back({slot, original, replacement});
            if (extended && m_originalGetStateEx == nullptr) {
                m_originalGetStateEx = reinterpret_cast<XInputGetStateFn>(original);
            } else if (!extended && m_originalGetState == nullptr) {
                m_originalGetState = reinterpret_cast<XInputGetStateFn>(original);
            }
        }
    }
    return !m_slots.empty() &&
        (m_originalGetState != nullptr || m_originalGetStateEx != nullptr);
}

void XInputBridge::Publish(const VrGamepadState& state) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_vrState = state;
    m_vrStateUpdatedMs = state.active ? GetTickCount64() : 0;
    if (state.active) m_virtualConnected = true;
}

void XInputBridge::ReleaseVrState() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_vrState = {};
    m_vrStateUpdatedMs = 0;
}

DWORD WINAPI XInputBridge::HookedXInputGetState(
    DWORD userIndex, XINPUT_STATE* state) {
    auto& bridge = Instance();
    if (state == nullptr) return ERROR_BAD_ARGUMENTS;
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    if (bridge.m_originalGetState != nullptr) {
        result = bridge.m_originalGetState(userIndex, state);
    }
    return MergeVrState(userIndex, state, result);
}

DWORD WINAPI XInputBridge::HookedXInputGetStateEx(
    DWORD userIndex, XINPUT_STATE* state) {
    auto& bridge = Instance();
    if (state == nullptr) return ERROR_BAD_ARGUMENTS;
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    if (bridge.m_originalGetStateEx != nullptr) {
        result = bridge.m_originalGetStateEx(userIndex, state);
    }
    return MergeVrState(userIndex, state, result);
}

DWORD XInputBridge::MergeVrState(
    DWORD userIndex, XINPUT_STATE* state, DWORD originalResult) {
    auto& bridge = Instance();
    if (userIndex != 0) return originalResult;

    static std::atomic<std::uint64_t> pollCount{0};
    const std::uint64_t currentPoll = pollCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (currentPoll == 1)
        Log("[XInput] Game XInputGetState polling intercepted");

    std::lock_guard<std::mutex> lock(bridge.m_stateMutex);
    const std::uint64_t now = GetTickCount64();
    if (!bridge.m_vrState.active || bridge.m_vrStateUpdatedMs == 0 ||
        now < bridge.m_vrStateUpdatedMs ||
        now - bridge.m_vrStateUpdatedMs > 250) {
        bridge.m_vrState = {};
        bridge.m_vrStateUpdatedMs = 0;
        if (originalResult == ERROR_SUCCESS || !bridge.m_virtualConnected)
            return originalResult;
        std::memset(state, 0, sizeof(*state));
        state->dwPacketNumber = bridge.m_packetNumber;
        return ERROR_SUCCESS;
    }
    if (originalResult != ERROR_SUCCESS) std::memset(state, 0, sizeof(*state));

    XINPUT_GAMEPAD& gamepad = state->Gamepad;
    const VrGamepadState& vr = bridge.m_vrState;
    const float moveMagnitudeSquared = vr.moveX * vr.moveX + vr.moveY * vr.moveY;
    if (moveMagnitudeSquared > 0.0001f) {
        gamepad.sThumbLX = ToThumb(vr.moveX);
        gamepad.sThumbLY = ToThumb(vr.moveY);
    }
    gamepad.sThumbRX = MergeThumb(gamepad.sThumbRX, ToThumb(vr.turnX));
    gamepad.sThumbRY = MergeThumb(gamepad.sThumbRY, ToThumb(vr.turnY));
    gamepad.bLeftTrigger = (std::max)(
        gamepad.bLeftTrigger, ToTrigger(vr.leftTrigger));
    gamepad.bRightTrigger = (std::max)(
        gamepad.bRightTrigger, ToTrigger(vr.rightTrigger));
    gamepad.wButtons |= vr.buttons;

    bridge.m_packetNumber = (std::max)(
        bridge.m_packetNumber, state->dwPacketNumber);
    if (std::memcmp(&bridge.m_lastPublished, &gamepad, sizeof(gamepad)) != 0) {
        bridge.m_lastPublished = gamepad;
        ++bridge.m_packetNumber;
    }
    state->dwPacketNumber = bridge.m_packetNumber;
    return ERROR_SUCCESS;
}

void XInputBridge::Shutdown() {
    ReleaseVrState();
    for (const PatchedSlot& patch : m_slots) {
        if (patch.slot == nullptr || patch.original == nullptr ||
            patch.replacement == nullptr || *patch.slot != patch.replacement) {
            continue;
        }
        DWORD oldProtection = 0;
        if (!VirtualProtect(patch.slot, sizeof(void*), PAGE_READWRITE,
                            &oldProtection)) {
            continue;
        }
        *patch.slot = patch.original;
        DWORD ignored = 0;
        VirtualProtect(patch.slot, sizeof(void*), oldProtection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), patch.slot, sizeof(void*));
    }
    for (void* target : m_inlineTargets) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    m_inlineTargets.clear();
    m_slots.clear();
    m_originalGetState = nullptr;
    m_originalGetStateEx = nullptr;
    m_virtualConnected = false;
    m_lastPublished = {};
    m_packetNumber = 1;
    Log("[XInput] Bridge shutdown");
}

}} // namespace bl1gotyvr::input
