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
    void DumpInteractionCandidates();
    void ResolveAddress(uintptr_t address);
    void DumpInteractionState();
    void FindInteractionReferrers();
    void FindInteractionReferrersIndirect();
    void ScanTraceCalls();
    void DisasmAt(uintptr_t address, const char* tag);
    void DumpCode(uintptr_t address, uint32_t size, const char* tag);
    void DumpInteractionProperties();
    void Shutdown();
    bool IsHookActive() const { return m_hookInstalled.load(std::memory_order_acquire); }
    uint64_t GetOverrideCount() const { return m_overrideCount.load(std::memory_order_relaxed); }
    PlayerIdentitySnapshot GetPlayerIdentity();
    bool IsVehicleTerminalUiActive() const;
    bool IsPlayerInjured() const {
        return m_playerInjured.load(std::memory_order_acquire);
    }
    bool IsPhaseWalkActive();
    bool RefreshIdentityFromLivePawn(uintptr_t controller, uintptr_t pawn);

private:
    WeaponAimSystem() = default;

    using ProcessEventFn = void(__fastcall*)(void*, void*, void*, void*);
    using CallFunctionFn = void(__fastcall*)(void*, void*, void*, void*);
    using GetAimRotationFn = void(__fastcall*)(void*, void*, void*);
    using ScriptInvokeFn = void(__fastcall*)(void*, void*, void*);
    using GameplayStateFn = void(__fastcall*)(void*, void*, void*);
    using NativeExecFn = void(__fastcall*)(void*, void*, void*);
    using UsableSelectorFn = void*(__fastcall*)(void*, void*);

    static void __fastcall HookedProcessEvent(void* object, void* function,
                                               void* params, void* nullArg);
    static void __fastcall HookedCallFunction(void* object, void* frame,
                                               void* result, void* function);
    bool Install(uintptr_t target);
    bool InstallCallFunction(uintptr_t target);
    bool InstallNativeAimProbe(uint64_t moduleBase, uint32_t moduleSize);
    bool InstallGameplayStateProbes(uintptr_t injuredFunction,
                                    uintptr_t phaseWalkFunction,
                                    uint64_t moduleBase, uint32_t moduleSize);
    bool InstallScriptInvokeProbe(uintptr_t function, uint64_t moduleBase,
                                  uint32_t moduleSize);
    bool InstallInteractionAimHook(uintptr_t function, uint64_t moduleBase,
                                   uint32_t moduleSize);
    uintptr_t FindUsableSelector(uint64_t moduleBase, uint32_t moduleSize);
    bool InstallUsableSelectorHook(uintptr_t function, uint64_t moduleBase,
                                   uint32_t moduleSize);
    static void* __fastcall HookedUsableSelector(void* controller, void* outRef);
    bool InstallTraceHook(uintptr_t function, uint64_t moduleBase,
                          uint32_t moduleSize);
    bool InstallTraceProbe(uintptr_t function, uint64_t moduleBase,
                           uint32_t moduleSize, const char* tag,
                           uintptr_t& targetOut, NativeExecFn hookFn,
                           NativeExecFn& originalOut,
                           std::atomic<bool>& installedFlag);
    static void __fastcall HookedGetAimRotation(void* object, void* frame, void* result);
    static void __fastcall HookedScriptInvoke(void* object, void* frame, void* result);
    static void __fastcall HookedTickTargets(void* object, void* frame, void* result);
    static void __fastcall HookedTrace(void* object, void* frame, void* result);
    static void __fastcall HookedTraceActors(void* object, void* frame, void* result);
    static void __fastcall HookedFastTrace(void* object, void* frame, void* result);
    static void __fastcall HookedViewPoint(void* object, void* frame, void* result);
    static bool TraceGateScan(void* frame, const char* tag, uint32_t& startOffsetOut,
                              float& lengthOut, bool logOnly);
    static void __fastcall HookedIsInjured(void* object, void* frame, void* result);
    static void __fastcall HookedPhaseWalkVisibility(
        void* object, void* frame, void* result);
    static void __fastcall HookedMeleeAttack(void* object, void* frame, void* result);
    static void __fastcall HookedFindMeleeTarget(void* object, void* frame, void* result);
    bool InstallMeleeAttackHook(uintptr_t function, uintptr_t target,
                                uintptr_t meleeDefinitionClass,
                                int32_t objectClassOffset, int32_t contextParameterOffset,
                                int32_t traceScaleOffset, int32_t radiusScaleOffset);
    bool InstallFindMeleeTargetHook(uintptr_t function, uintptr_t target,
                                    int32_t parameterOffset);
    uintptr_t FindProcessEvent(uint64_t controllerAddress, uint64_t moduleBase,
                               uint32_t moduleSize);
    uintptr_t FindCallFunction(uint64_t moduleBase, uint32_t moduleSize);
    bool FindPawnAimRotation(uint64_t controllerAddress);
    void SampleInteractionFields();

    std::atomic<bool> m_initialized{false};
    std::atomic<uint64_t> m_runtimeModuleBase{0};
    std::atomic<uint32_t> m_runtimeModuleSize{0};
    std::atomic<bool> m_hookInstalled{false};
    std::atomic<uint64_t> m_getAdjustedAimName{0};
    std::atomic<uint64_t> m_isInjuredName{0};
    std::atomic<uint64_t> m_phaseWalkVisibilityName{0};
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
    std::atomic<bool> m_playerInjured{false};
    std::atomic<bool> m_phaseWalkActive{false};
    std::atomic<uint64_t> m_nativeAimCalls{0};
    std::atomic<uint64_t> m_processEventAimCalls{0};
    std::atomic<uint64_t> m_interactionRedirectCount{0};
    std::atomic<bool> m_interactionHookInstalled{false};
    std::atomic<uintptr_t> m_tickTargetsFunction{0};
    std::atomic<uintptr_t> m_traceFunction{0};
    std::atomic<uint64_t> m_traceRedirectCount{0};
    std::atomic<bool> m_traceHookInstalled{false};
    std::atomic<uintptr_t> m_traceActorsFunction{0};
    std::atomic<uintptr_t> m_fastTraceFunction{0};
    std::atomic<uintptr_t> m_viewPointFunction{0};
    std::atomic<uintptr_t> m_allowUseEventFunction{0};
    std::atomic<uintptr_t> m_playerTickFunctions[8]{};
    std::atomic<uint32_t> m_playerTickFunctionCount{0};
    std::atomic<uintptr_t> m_allowUseFunctions[8]{};
    std::atomic<uint32_t> m_allowUseFunctionCount{0};
    std::atomic<int32_t> m_frameNodeOffset{-1};
    std::atomic<uint64_t> m_traceProbeHits{0};
    std::atomic<uint64_t> m_viewPointCalls{0};
    std::atomic<uint64_t> m_allowUseEvents{0};
    std::atomic<uint64_t> m_scriptInvokeAimCalls{0};
    std::atomic<uint64_t> m_nextInteractionSampleMs{0};
    std::atomic<uintptr_t> m_sampledInteractionController{0};
    std::atomic<uintptr_t> m_sampledTouchedObject{0};
    std::atomic<uintptr_t> m_sampledSeenObject{0};
    std::atomic<uintptr_t> m_sampledUsableObject{0};
    std::atomic<uintptr_t> m_sampledInteractionClient{0};
    std::atomic<uintptr_t> m_sampledInteractionsData{0};
    std::atomic<int32_t> m_sampledInteractionsCount{-1};
    std::atomic<uint32_t> m_sampledInteractionScalars{0xFFFFFFFFu};
    uintptr_t m_nativeAimTarget = 0;
    GetAimRotationFn m_originalGetAimRotation = nullptr;
    uintptr_t m_processEventTarget = 0;
    ProcessEventFn m_originalProcessEvent = nullptr;
    std::atomic<bool> m_callFunctionInstalled{false};
    uintptr_t m_callFunctionTarget = 0;
    CallFunctionFn m_originalCallFunction = nullptr;
    uintptr_t m_scriptInvokeTarget = 0;
    ScriptInvokeFn m_originalScriptInvoke = nullptr;
    std::atomic<bool> m_scriptInvokeInstalled{false};
    uintptr_t m_tickTargetsTarget = 0;
    NativeExecFn m_originalTickTargets = nullptr;
    uintptr_t m_traceTarget = 0;
    NativeExecFn m_originalTrace = nullptr;
    uintptr_t m_traceActorsTarget = 0;
    NativeExecFn m_originalTraceActors = nullptr;
    std::atomic<bool> m_traceActorsInstalled{false};
    uintptr_t m_fastTraceTarget = 0;
    NativeExecFn m_originalFastTrace = nullptr;
    std::atomic<bool> m_fastTraceInstalled{false};
    uintptr_t m_viewPointTarget = 0;
    NativeExecFn m_originalViewPoint = nullptr;
    std::atomic<bool> m_viewPointInstalled{false};
    uintptr_t m_isInjuredTarget = 0;
    GameplayStateFn m_originalIsInjured = nullptr;
    uintptr_t m_phaseWalkVisibilityTarget = 0;
    GameplayStateFn m_originalPhaseWalkVisibility = nullptr;
    std::atomic<bool> m_meleeAttackInstalled{false};
    uintptr_t m_meleeAttackFunction = 0;
    uintptr_t m_meleeAttackTarget = 0;
    NativeExecFn m_originalMeleeAttack = nullptr;
    uintptr_t m_meleeDefinitionClass = 0;
    int32_t m_meleeObjectClassOffset = -1;
    int32_t m_meleeContextParameterOffset = -1;
    int32_t m_traceScaleOffset = -1;
    int32_t m_radiusScaleOffset = -1;
    mutable SRWLOCK m_meleeRangeLock = SRWLOCK_INIT;
    std::atomic<bool> m_meleeMutationSafe{true};
    std::atomic<DWORD> m_meleeExecutionThread{0};
    std::atomic<uint64_t> m_meleeRangeApplies{0};
    std::atomic<bool> m_findMeleeTargetInstalled{false};
    uintptr_t m_findMeleeTargetFunction = 0;
    uintptr_t m_findMeleeTargetTarget = 0;
    NativeExecFn m_originalFindMeleeTarget = nullptr;
    int32_t m_maxLungeDistanceParameterOffset = -1;
    std::atomic<uint64_t> m_lungeRangeApplies{0};
    std::atomic<bool> m_meleeHooksStopping{false};
    std::atomic<uint32_t> m_inFlightMeleeHooks{0};
    std::atomic<bool> m_usableSelectorInstalled{false};
    std::atomic<uint64_t> m_usableSelectorRedirects{0};
    uintptr_t m_usableSelectorTarget = 0;
    UsableSelectorFn m_originalUsableSelector = nullptr;
};

}} // namespace bl1gotyvr::input
