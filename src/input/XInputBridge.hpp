#pragma once

#include <Windows.h>
#include <Xinput.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace bl1gotyvr { namespace input {

struct VrGamepadState {
    float moveX = 0.0f;
    float moveY = 0.0f;
    float turnX = 0.0f;
    float turnY = 0.0f;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
    WORD buttons = 0;
    bool active = false;
};

class XInputBridge {
public:
    static XInputBridge& Instance();

    bool Initialize();
    void Publish(const VrGamepadState& state);
    void ReleaseVrState();
    void Shutdown();
    bool IsInstalled() const {
        return !m_slots.empty() || !m_inlineTargets.empty();
    }

private:
    XInputBridge() = default;
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

    struct PatchedSlot {
        void** slot = nullptr;
        void* original = nullptr;
        void* replacement = nullptr;
    };

    static DWORD WINAPI HookedXInputGetState(
        DWORD userIndex, XINPUT_STATE* state);
    static DWORD WINAPI HookedXInputGetStateEx(
        DWORD userIndex, XINPUT_STATE* state);
    static DWORD MergeVrState(DWORD userIndex, XINPUT_STATE* state,
                              DWORD originalResult);
    bool PatchHostImports();
    bool HookLoadedXInputExports();

    mutable std::mutex m_stateMutex;
    VrGamepadState m_vrState;
    std::uint64_t m_vrStateUpdatedMs = 0;
    bool m_virtualConnected = false;
    XINPUT_GAMEPAD m_lastPublished = {};
    DWORD m_packetNumber = 1;
    XInputGetStateFn m_originalGetState = nullptr;
    XInputGetStateFn m_originalGetStateEx = nullptr;
    std::vector<PatchedSlot> m_slots;
    std::vector<void*> m_inlineTargets;
};

}} // namespace bl1gotyvr::input
