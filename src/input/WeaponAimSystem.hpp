#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace bl1gotyvr { namespace input {

struct PlayerIdentitySnapshot {
    uintptr_t controller = 0;
    uintptr_t pawn = 0;
    uintptr_t weapon = 0;
    uint64_t generation = 0;
    bool pawnValid = false;
    bool weaponValid = false;
};

void BuildCalibratedLocalForward(float pitchDegrees, float yawDegrees, float output[3]);

class WeaponAimSystem {
public:
    static WeaponAimSystem& Instance();

    void UpdateDirection(const float worldOrigin[3], const float worldDirection[3],
                         float convergenceMeters);
    void InvalidateDirection();
    void SetFireActive(bool active) { m_fireActive.store(active, std::memory_order_release); }
    void SetVehicleSecondaryFireActive(bool active) {
        m_vehicleSecondaryFireActive.store(active, std::memory_order_release);
    }
    bool IsFireActive() const { return m_fireActive.load(std::memory_order_acquire); }
    void SetBallisticOverrideEnabled(bool enabled);
    bool IsBallisticOverrideEnabled() const {
        return m_ballisticOverrideEnabled.load(std::memory_order_acquire);
    }

    void Discover(const void* globals, uint64_t controllerAddress,
                  uint64_t moduleBase, uint32_t moduleSize);
    void Shutdown();
    bool IsHookActive() const { return m_hookInstalled.load(std::memory_order_acquire); }
    uint64_t GetOverrideCount() const { return m_overrideCount.load(std::memory_order_relaxed); }
    PlayerIdentitySnapshot GetPlayerIdentity();
    bool RefreshIdentityFromLivePawn(uintptr_t controller, uintptr_t pawn);

private:
    WeaponAimSystem() = default;

    using ProcessEventFn = int32_t(__fastcall*)(void*, uint64_t, void*, void*);
    using GetAimRotationFn = void(__fastcall*)(void*, void*, void*);
    using ScriptInvokeFn = void(__fastcall*)(void*, void*, void*);

    static int32_t __fastcall HookedProcessEvent(void* object, uint64_t functionName,
                                                  void* params, void* result);
    bool Install(uintptr_t target);
    bool InstallNativeAimProbe(uint64_t moduleBase, uint32_t moduleSize);
    bool InstallScriptInvokeProbe(uintptr_t function, uint64_t moduleBase,
                                  uint32_t moduleSize);
    static void __fastcall HookedGetAimRotation(void* object, void* frame, void* result);
    static void __fastcall HookedScriptInvoke(void* object, void* frame, void* result);
    uintptr_t FindProcessEvent(uint64_t controllerAddress, uint64_t moduleBase,
                               uint32_t moduleSize);
    bool FindPawnAimRotation(uint64_t controllerAddress);

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_hookInstalled{false};
    std::atomic<uint64_t> m_getAdjustedAimName{0};
    std::atomic<uintptr_t> m_getAdjustedAimFunction{0};
    std::atomic<uintptr_t> m_getAdjustedAimOwnerClass{0};
    std::atomic<uintptr_t> m_localController{0};
    std::atomic<uintptr_t> m_localPawn{0};
    std::atomic<uintptr_t> m_localWeapon{0};
    std::atomic<uint64_t> m_identityGeneration{0};
    std::atomic<bool> m_pawnIdentityValid{false};
    std::atomic<bool> m_weaponIdentityValid{false};
    mutable SRWLOCK m_identityLock = SRWLOCK_INIT;
    int32_t m_pawnPropertyOffset = -1;
    int32_t m_controllerPropertyOffset = -1;
    int32_t m_weaponPropertyOffset = -1;
    int32_t m_ownerPropertyOffset = -1;
    std::atomic<int32_t> m_aimPitch{0};
    std::atomic<int32_t> m_aimYaw{0};
    std::atomic<int32_t> m_aimRoll{0};
    std::atomic<bool> m_aimValid{false};
    std::atomic<uint64_t> m_aimUpdatedMs{0};
    std::atomic<float> m_aimOriginX{0.0f};
    std::atomic<float> m_aimOriginY{0.0f};
    std::atomic<float> m_aimOriginZ{0.0f};
    std::atomic<float> m_aimTargetX{0.0f};
    std::atomic<float> m_aimTargetY{0.0f};
    std::atomic<float> m_aimTargetZ{0.0f};
    std::atomic<bool> m_aimTargetValid{false};
    std::atomic<bool> m_ballisticOverrideEnabled{true};
    mutable SRWLOCK m_ballisticOverrideLock = SRWLOCK_INIT;
    std::atomic<uint64_t> m_overrideCount{0};
    std::atomic<uintptr_t> m_pawnAimRotationAddr{0};
    std::atomic<bool> m_fireActive{false};
    std::atomic<bool> m_vehicleSecondaryFireActive{false};
    std::atomic<uint64_t> m_nativeAimCalls{0};
    std::atomic<uint64_t> m_processEventAimCalls{0};
    std::atomic<uint64_t> m_scriptInvokeAimCalls{0};
    uintptr_t m_nativeAimTarget = 0;
    GetAimRotationFn m_originalGetAimRotation = nullptr;
    uintptr_t m_processEventTarget = 0;
    ProcessEventFn m_originalProcessEvent = nullptr;
    uintptr_t m_scriptInvokeTarget = 0;
    ScriptInvokeFn m_originalScriptInvoke = nullptr;
    std::atomic<bool> m_scriptInvokeInstalled{false};
};

}} // namespace bl1gotyvr::input
