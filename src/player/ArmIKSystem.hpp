#pragma once

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bl1gotyvr::player {

struct ArmRigStatus {
    bool rigValid = false;
    bool poseHookInstalled = false;
    uintptr_t component = 0;
    uintptr_t skeletalMesh = 0;
    uintptr_t componentPose = 0;
    int boneCount = 0;
    int rightShoulder = -1;
    int rightElbow = -1;
    int rightWrist = -1;
    int leftShoulder = -1;
    int leftElbow = -1;
    int leftWrist = -1;
    uint64_t solvedGeneration = 0;
    uint64_t poseHookCalls = 0;
    uint64_t poseHookApplies = 0;
};

enum class ComponentRole : uint8_t {
    Unknown,
    ProtectedWeapon,
    PawnBody,
    ProbableFirstPersonArms,
};

struct ComponentInventoryEntry {
    ComponentRole role = ComponentRole::Unknown;
    uintptr_t component = 0;
    uintptr_t outer = 0;
    uintptr_t classObject = 0;
    uintptr_t skeletalMesh = 0;
    uint64_t objectNameToken = 0;
    uint64_t updateCount = 0;
    int boneCount = 0;
    int skeletalMeshOffset = 0;
    int localToWorldOffset = 0;
    float cameraDistance = -1.0f;
    bool exactPawnOuter = false;
    bool exactWeaponOuter = false;
    bool conservativeWeaponEvidence = false;
    bool torsoSignature = false;
    bool lowerBodySignature = false;
    bool rightArmChain = false;
    bool leftArmChain = false;
    char objectName[64] = {};
    char className[64] = {};
    char outerName[64] = {};
    char meshName[64] = {};
};

struct ComponentInventoryStatus {
    static constexpr size_t kCapacity = 128;
    std::array<ComponentInventoryEntry, kCapacity> entries{};
    size_t count = 0;
    uint64_t generation = 0;
    uintptr_t controller = 0;
    uintptr_t pawn = 0;
    uintptr_t weapon = 0;
    bool pawnIdentityValid = false;
    bool weaponIdentityValid = false;
    int weaponComponentCount = 0;
    int truncatedComponentCount = 0;
};

class ArmIKSystem {
public:
    static ArmIKSystem& Instance();

    void StartDiscovery();
    void Shutdown();
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled.load(); }
    void SetVisibilityEnabled(bool enabled);
    bool IsVisibilityEnabled() const { return m_visibilityEnabled.load(); }
    void SetSimulationEnabled(bool enabled);
    bool IsSimulationEnabled() const { return m_simulationEnabled.load(); }
    void RequestRescan();
    void RequestInventoryScan();
    void RequestCalibrationReset();
    uint64_t UpdateTargets(const float cameraLocation[3], float gamePitchRadians,
                           float gameYawRadians,
                           const float trackingReferencePosition[3],
                           const float trackingReferenceRotation[4]);
    void SetRenderContext(uint64_t renderGeneration, uint64_t targetGeneration);
    bool ApplyPostAnimation(void* component);
    void Restore();
    ArmRigStatus GetStatus() const;
    ComponentInventoryStatus GetComponentInventory() const;

private:
    ArmIKSystem() = default;
    static DWORD WINAPI DiscoveryThreadProc(void* context);
    static void __fastcall HookedUpdateSkelPose(void* component, float deltaTime,
                                                uint32_t tickFaceFx);
    void DiscoveryLoop();
    bool ProbeRig(uint64_t inventoryRequestGeneration);
    bool ApplyVisibility(const ComponentInventoryStatus& inventory,
                         int32_t visibilityOffset, uint32_t hiddenGameMask,
                         uint64_t inventoryRequestGeneration);
    void RestoreVisibility();
    void CheckVisibilityWatchdog();
    bool InstallPoseHook();
    void ObserveComponent(void* component);
    bool Apply(uint64_t renderGeneration, uint64_t targetGeneration,
               bool restoreAfterRender);

    using UpdateSkelPoseFn = void(__fastcall*)(void*, float, uint32_t);

    struct Rig;
    struct TargetSnapshot;
    Rig* m_rig = nullptr;
    TargetSnapshot* m_targets = nullptr;
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_visibilityEnabled{false};
    std::atomic<bool> m_simulationEnabled{false};
    std::atomic<bool> m_calibrationResetRequested{false};
    std::atomic<bool> m_discoveryRequested{false};
    std::atomic<uint64_t> m_inventoryRequestGeneration{0};
    std::atomic<uint64_t> m_inventoryCompletedGeneration{0};
    std::atomic<uint64_t> m_rigRequestGeneration{0};
    std::atomic<uint64_t> m_scanEpoch{0};
    std::atomic<bool> m_poseHookInstalled{false};
    std::atomic<uint64_t> m_latestTargetGeneration{0};
    std::atomic<uint64_t> m_renderGeneration{0};
    std::atomic<uint64_t> m_renderTargetGeneration{0};
    std::atomic<uint64_t> m_simulationGeneration{0};
    std::atomic<uint64_t> m_poseHookCalls{0};
    std::atomic<uint64_t> m_poseHookApplies{0};
    std::atomic<uint32_t> m_inFlightPoseHooks{0};
    std::atomic<bool> m_shuttingDown{false};
    HANDLE m_thread = nullptr;
    uintptr_t m_poseHookTarget = 0;
    UpdateSkelPoseFn m_originalUpdateSkelPose = nullptr;
    std::array<std::atomic<uintptr_t>, 256> m_observedComponents{};
    std::array<std::atomic<uint64_t>, 256> m_observedComponentUpdates{};
    std::atomic<uint32_t> m_observedComponentsTruncated{0};
    mutable SRWLOCK m_scanResetLock = SRWLOCK_INIT;
    mutable SRWLOCK m_cameraCacheLock = SRWLOCK_INIT;
    bool m_hasGoodCameraLocation = false;
    float m_goodCameraLocation[3] = {};
    mutable SRWLOCK m_rigLock = SRWLOCK_INIT;
    mutable SRWLOCK m_targetLock = SRWLOCK_INIT;
    mutable SRWLOCK m_inventoryLock = SRWLOCK_INIT;
    ComponentInventoryStatus m_inventory;
    mutable SRWLOCK m_visibilityLock = SRWLOCK_INIT;
    bool m_visibilityActive = false;
    uintptr_t m_visibilityController = 0;
    uintptr_t m_visibilityPawn = 0;
    uintptr_t m_visibilityWeapon = 0;
    uintptr_t m_visibilityComponents[2] = {};
    uintptr_t m_visibilityOuters[2] = {};
    uintptr_t m_visibilityClassObjects[2] = {};
    uint64_t m_visibilityNameTokens[2] = {};
    uint32_t m_visibilityOriginalWords[2] = {};
    char m_visibilityObjectNames[2][64] = {};
    char m_visibilityClassNames[2][64] = {};
    int32_t m_visibilityWordOffset = -1;
    uint32_t m_visibilityHiddenMask = 0;
    int m_visibilityArmsMatrixOffset = 0;
    std::atomic<uintptr_t> m_hiddenArmsComponent{0};
};

} // namespace bl1gotyvr::player
