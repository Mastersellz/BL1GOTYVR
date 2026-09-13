#include "WeaponAimSystem.hpp"

#include "../camera/UE3Scanner.hpp"
#include "../camera/CameraHook.hpp"
#include "../camera/SignatureScanner.hpp"
#include "../config/Config.hpp"
#include "../core/VRMod.hpp"
#include "../hook/MinHookWrapper.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "psapi.lib")

namespace bl1gotyvr { namespace input {

namespace {

constexpr int32_t kRotUnisPerTurn = 65536;
constexpr float kRadiansToUnis = 65536.0f / 6.2831853071795864769f;
bool DirectionToRotator(const float direction[3], int32_t rotation[3]) {
    if (!direction || !rotation) return false;
    const float horizontal = sqrtf(direction[0] * direction[0] +
                                   direction[1] * direction[1]);
    const float length = sqrtf(horizontal * horizontal + direction[2] * direction[2]);
    if (!std::isfinite(horizontal) || !std::isfinite(length) || length < 1.0e-5f)
        return false;
    rotation[0] = static_cast<int32_t>(lroundf(
        atan2f(direction[2], horizontal) * kRadiansToUnis));
    rotation[1] = static_cast<int32_t>(lroundf(
        atan2f(direction[1], direction[0]) * kRadiansToUnis));
    rotation[2] = 0;
    return true;
}

struct TArray64 {
    uint64_t data = 0;
    int32_t count = 0;
    int32_t capacity = 0;
};

struct AimFunctionCandidate {
    uintptr_t function = 0;
    uintptr_t owner = 0;
    uint64_t nameToken = 0;
};

static AimFunctionCandidate s_aimFunctionCandidates[128] = {};
static size_t s_aimFunctionCandidateCount = 0;
static bool s_aimFunctionCacheValid = false;
static SRWLOCK s_aimFunctionCacheLock = SRWLOCK_INIT;
thread_local unsigned s_meleeAttackDepth = 0;

enum class InteractionCallSource : uint32_t {
    ProcessEvent = 1,
    CallFunction = 2,
    ProcessInternal = 3
};

struct RecentInteractionCall {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uintptr_t> object{0};
    std::atomic<uintptr_t> function{0};
    std::atomic<uint32_t> source{0};
};

constexpr size_t kRecentInteractionCallCount = 2048;
static RecentInteractionCall s_recentInteractionCalls[kRecentInteractionCallCount];
static std::atomic<uint64_t> s_recentInteractionCallSequence{0};
static std::atomic<uintptr_t> s_focusGuardPage{0};
static std::atomic<uintptr_t> s_focusGuardAddress{0};
static std::atomic<uintptr_t> s_focusGuardHitRip{0};
static std::atomic<uintptr_t> s_focusGuardHitAddress{0};
static std::atomic<bool> s_focusGuardArmed{false};
static std::atomic<bool> s_focusGuardTracking{false};
static std::atomic<bool> s_focusGuardNeedsRearm{false};
static std::atomic<bool> s_focusGuardAttempted{false};
static std::atomic<uint64_t> s_focusGuardDeadlineMs{0};
static std::atomic<DWORD> s_focusGuardBaseProtect{PAGE_READWRITE};
static PVOID s_focusGuardHandler = nullptr;
static uintptr_t s_focusGuardStack[32] = {};

void RecordInteractionCall(void* object, void* function, InteractionCallSource source) {
    const uint64_t sequence = s_recentInteractionCallSequence.fetch_add(
        1, std::memory_order_relaxed) + 1;
    auto& call = s_recentInteractionCalls[sequence % kRecentInteractionCallCount];
    call.sequence.store(0, std::memory_order_relaxed);
    call.object.store(reinterpret_cast<uintptr_t>(object), std::memory_order_relaxed);
    call.function.store(reinterpret_cast<uintptr_t>(function), std::memory_order_relaxed);
    call.source.store(static_cast<uint32_t>(source), std::memory_order_relaxed);
    call.sequence.store(sequence, std::memory_order_release);
}

struct InFlightMeleeHook {
    explicit InFlightMeleeHook(std::atomic<uint32_t>& counter) : counter(counter) {
        counter.fetch_add(1, std::memory_order_acq_rel);
    }
    ~InFlightMeleeHook() { counter.fetch_sub(1, std::memory_order_acq_rel); }
    std::atomic<uint32_t>& counter;
};

bool ReadMem(uintptr_t address, void* output, size_t size) {
    SIZE_T read = 0;
    return address >= 0x10000 && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address), output, size, &read) && read == size;
}

bool ReadDirect(uintptr_t address, void* output, size_t size) {
    if (address < 0x10000 || !output || size == 0) return false;
    __try {
        memcpy(output, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteDirect(uintptr_t address, const void* input, size_t size) {
    if (address < 0x10000 || !input || size == 0) return false;
    __try {
        memcpy(reinterpret_cast<void*>(address), input, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteMem(uintptr_t address, const void* input, size_t size) {
    SIZE_T written = 0;
    return address >= 0x10000 && WriteProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(address), input, size, &written) && written == size;
}

bool ReadAscii(uintptr_t address, char* output, size_t capacity) {
    if (!output || capacity < 2) return false;
    for (size_t index = 0; index < capacity; ++index) {
        if (!ReadMem(address + index, output + index, 1)) return false;
        if (output[index] == '\0') return index > 0;
        if (output[index] < 0x20 || output[index] > 0x7e) return false;
    }
    output[capacity - 1] = '\0';
    return false;
}

bool ReadName(const camera::UE3Globals& globals, const TArray64& names,
              int32_t index, char* output, size_t capacity) {
    uint64_t entry = 0;
    return globals.gNamesValid && index >= 0 && index < names.count &&
        ReadMem(names.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                &entry, sizeof(entry)) && entry &&
        ReadAscii(entry + globals.gNameStringOffset, output, capacity);
}

bool ReadObjectName(const camera::UE3Globals& globals, const TArray64& names,
                    uintptr_t object, char* output, size_t capacity) {
    int32_t index = -1;
    return ReadMem(object + globals.gObjectNameOffset, &index, sizeof(index)) &&
        ReadName(globals, names, index, output, capacity);
}

bool ReadClassName(const camera::UE3Globals& globals, const TArray64& names,
                    uintptr_t object, char* output, size_t capacity) {
    uintptr_t classObject = 0;
    return ReadMem(object + globals.gObjectClassOffset, &classObject,
                   sizeof(classObject)) && classObject &&
        ReadObjectName(globals, names, classObject, output, capacity);
}

LONG CALLBACK FocusGuardExceptionHandler(EXCEPTION_POINTERS* exception) {
    if (!exception || !exception->ExceptionRecord || !exception->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (exception->ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP &&
        s_focusGuardNeedsRearm.exchange(false, std::memory_order_acq_rel)) {
        const uintptr_t page = s_focusGuardPage.load(std::memory_order_relaxed);
        if (page && s_focusGuardTracking.load(std::memory_order_relaxed) &&
            GetTickCount64() <= s_focusGuardDeadlineMs.load(std::memory_order_relaxed)) {
            DWORD oldProtect = 0;
            if (VirtualProtect(reinterpret_cast<void*>(page), 0x1000,
                    s_focusGuardBaseProtect.load(std::memory_order_relaxed) | PAGE_GUARD,
                    &oldProtect)) {
                s_focusGuardArmed.store(true, std::memory_order_relaxed);
            }
        } else {
            s_focusGuardTracking.store(false, std::memory_order_relaxed);
        }
        exception->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (exception->ExceptionRecord->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t accessAddress = static_cast<uintptr_t>(
        exception->ExceptionRecord->ExceptionInformation[1]);
    const uintptr_t page = s_focusGuardPage.load(std::memory_order_relaxed);
    if (!page || (accessAddress & ~uintptr_t{0xFFF}) != page)
        return EXCEPTION_CONTINUE_SEARCH;

    s_focusGuardArmed.store(false, std::memory_order_relaxed);
    const ULONG_PTR accessType = exception->ExceptionRecord->ExceptionInformation[0];
    const uintptr_t watched = s_focusGuardAddress.load(std::memory_order_relaxed);
    // watched == controller + 0xE60: capture touched (+0xE64) and seen (+0xE74)
    // writers. The already-solved CurrentUsableObject slot (+0xE84) is treated as
    // noise so it cannot consume the hit.
    if (accessType == 1 && accessAddress >= watched + 4 &&
        accessAddress < watched + 0x24) {
        const uintptr_t stackPointer = static_cast<uintptr_t>(
            exception->ContextRecord->Rsp);
        __try {
            memcpy(s_focusGuardStack, reinterpret_cast<const void*>(stackPointer),
                   sizeof(s_focusGuardStack));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(s_focusGuardStack, 0, sizeof(s_focusGuardStack));
        }
        s_focusGuardHitAddress.store(accessAddress, std::memory_order_relaxed);
        s_focusGuardHitRip.store(
            static_cast<uintptr_t>(exception->ContextRecord->Rip),
            std::memory_order_release);
        s_focusGuardTracking.store(false, std::memory_order_relaxed);
    } else if (s_focusGuardTracking.load(std::memory_order_relaxed) &&
               GetTickCount64() <=
                   s_focusGuardDeadlineMs.load(std::memory_order_relaxed)) {
        s_focusGuardNeedsRearm.store(true, std::memory_order_release);
        exception->ContextRecord->EFlags |= 0x100u;
    } else {
        s_focusGuardTracking.store(false, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

[[maybe_unused]] void PollAndArmFocusGuard(uintptr_t controller, uintptr_t moduleBase,
                                           uint32_t moduleSize, uintptr_t touchedObject,
                                           uintptr_t seenObject, uintptr_t usableObject) {
    const uintptr_t watched = controller + 0xE60;
    const uintptr_t page = watched & ~uintptr_t{0xFFF};
    const uintptr_t hitRip = s_focusGuardHitRip.exchange(0, std::memory_order_acq_rel);
    if (hitRip) {
        const uintptr_t hitAddress = s_focusGuardHitAddress.load(std::memory_order_relaxed);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION runtimeFunction = RtlLookupFunctionEntry(
            static_cast<DWORD64>(hitRip), &imageBase, nullptr);
        const uintptr_t functionStart = runtimeFunction
            ? static_cast<uintptr_t>(imageBase + runtimeFunction->BeginAddress) : 0;
        Log("[InteractionAim] Focus write watchpoint RIP=%p offset=0x%llX "
            "address=%p function=%p rva=0x%llX",
            reinterpret_cast<void*>(hitRip),
            static_cast<unsigned long long>(hitAddress - watched + 0xE60),
            reinterpret_cast<void*>(hitAddress),
            reinterpret_cast<void*>(functionStart),
            static_cast<unsigned long long>(
                functionStart >= moduleBase ? functionStart - moduleBase : 0));
        uintptr_t uniqueFunctions[12] = {};
        size_t uniqueCount = 0;
        for (uintptr_t address : s_focusGuardStack) {
            if (address < moduleBase || address >= moduleBase + moduleSize) continue;
            DWORD64 stackImageBase = 0;
            PRUNTIME_FUNCTION stackFunction = RtlLookupFunctionEntry(
                static_cast<DWORD64>(address), &stackImageBase, nullptr);
            const uintptr_t start = stackFunction
                ? static_cast<uintptr_t>(stackImageBase + stackFunction->BeginAddress) : 0;
            if (!start) continue;
            bool duplicate = false;
            for (size_t index = 0; index < uniqueCount; ++index)
                duplicate |= uniqueFunctions[index] == start;
            if (duplicate) continue;
            uniqueFunctions[uniqueCount++] = start;
            Log("[InteractionAim] Focus write stack[%zu] return=%p function=%p rva=0x%llX",
                uniqueCount - 1, reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(start),
                static_cast<unsigned long long>(start - moduleBase));
            if (uniqueCount == _countof(uniqueFunctions)) break;
        }
        unsigned char bytes[32] = {};
        if (ReadMem(hitRip, bytes, sizeof(bytes))) {
            Log("[InteractionAim] Focus write bytes: %02X %02X %02X %02X %02X %02X "
                "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15]);
        }
    }

    if (!s_focusGuardHandler)
        s_focusGuardHandler = AddVectoredExceptionHandler(1, FocusGuardExceptionHandler);
    if (!s_focusGuardHandler) return;

    const bool focusPresent = touchedObject || seenObject || usableObject;
    if (!focusPresent) {
        s_focusGuardAttempted.store(false, std::memory_order_relaxed);
        if (!s_focusGuardHitRip.load(std::memory_order_relaxed))
            s_focusGuardTracking.store(false, std::memory_order_relaxed);
        return;
    }
    if (s_focusGuardAttempted.load(std::memory_order_relaxed)) {
        if (s_focusGuardTracking.load(std::memory_order_relaxed)) return;
        if (GetTickCount64() <
            s_focusGuardDeadlineMs.load(std::memory_order_relaxed)) return;
        s_focusGuardAttempted.store(false, std::memory_order_relaxed);
    }
    s_focusGuardAttempted.store(true, std::memory_order_relaxed);
    s_focusGuardDeadlineMs.store(GetTickCount64() + 2000, std::memory_order_relaxed);
    s_focusGuardTracking.store(true, std::memory_order_release);

    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(reinterpret_cast<void*>(page), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT) return;
    if (!(memory.Protect & PAGE_GUARD))
        s_focusGuardArmed.store(false, std::memory_order_relaxed);
    if (s_focusGuardArmed.load(std::memory_order_relaxed)) return;
    DWORD oldProtect = 0;
    const DWORD baseProtect = memory.Protect & ~PAGE_GUARD;
    s_focusGuardBaseProtect.store(baseProtect, std::memory_order_relaxed);
    s_focusGuardAddress.store(watched, std::memory_order_relaxed);
    s_focusGuardPage.store(page, std::memory_order_relaxed);
    if (VirtualProtect(reinterpret_cast<void*>(page), 0x1000,
                       baseProtect | PAGE_GUARD, &oldProtect)) {
        s_focusGuardArmed.store(true, std::memory_order_release);
    }
}

void ShutdownFocusGuard() {
    const uintptr_t page = s_focusGuardPage.exchange(0, std::memory_order_acq_rel);
    if (page) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (VirtualQuery(reinterpret_cast<void*>(page), &memory, sizeof(memory)) &&
            (memory.Protect & PAGE_GUARD)) {
            DWORD oldProtect = 0;
            VirtualProtect(reinterpret_cast<void*>(page), 0x1000,
                           memory.Protect & ~PAGE_GUARD, &oldProtect);
        }
    }
    s_focusGuardArmed.store(false, std::memory_order_release);
    s_focusGuardTracking.store(false, std::memory_order_release);
    s_focusGuardNeedsRearm.store(false, std::memory_order_release);
    s_focusGuardAttempted.store(false, std::memory_order_release);
    if (s_focusGuardHandler) {
        RemoveVectoredExceptionHandler(s_focusGuardHandler);
        s_focusGuardHandler = nullptr;
    }
}

// Dot-based pickup selection. The game tracks nearby pickups on the controller
// and chooses touched/seen by proximity; rewrite those two interface refs with
// the candidate the right-hand dot is pointing at.
constexpr uintptr_t kDotPickupTouchedOffset = 0xE64;
constexpr uintptr_t kDotPickupSeenOffset = 0xE74;
constexpr uintptr_t kDotPickupNearbyOffset = 0x182C;
static std::atomic<int32_t> s_dotPickupStride{0};
static std::atomic<uintptr_t> s_dotPickupLocationOffset{0};
static std::atomic<uintptr_t> s_dotPickupInterfaceDelta{0};
static std::atomic<bool> s_dotPickupDumped{false};
static std::atomic<uint64_t> s_dotPickupLogs{0};

struct DotPickupRef {
    uintptr_t object = 0;
    uintptr_t interfacePointer = 0;
};

uintptr_t ResolvePickupLocationOffset(const camera::UE3Globals& globals,
                                      const TArray64& names, uintptr_t actorClass) {
    for (int depth = 0; depth < 16 && actorClass >= 0x10000; ++depth) {
        uintptr_t property = 0;
        if (!ReadMem(actorClass + 0xB0, &property, sizeof(property))) break;
        size_t visited = 0;
        while (property >= 0x10000 && visited++ < 2048) {
            char propertyName[128] = {};
            char propertyClass[128] = {};
            int32_t offset = -1;
            int32_t elementSize = 0;
            if (ReadObjectName(globals, names, property, propertyName,
                               sizeof(propertyName)) &&
                ReadClassName(globals, names, property, propertyClass,
                              sizeof(propertyClass)) &&
                ReadMem(property + 0x8C, &offset, sizeof(offset)) &&
                ReadMem(property + 0x6C, &elementSize, sizeof(elementSize)) &&
                strcmp(propertyName, "Location") == 0 && elementSize == 12 &&
                offset >= 0) {
                return static_cast<uintptr_t>(offset);
            }
            uintptr_t next = 0;
            if (!ReadMem(property + 0x90, &next, sizeof(next)) || next == property)
                break;
            property = next;
        }
        uintptr_t superClass = 0;
        if (!ReadMem(actorClass + 0x78, &superClass, sizeof(superClass)) ||
            superClass == actorClass)
            break;
        actorClass = superClass;
    }
    return 0;
}

void ApplyDotPickupSelection(uintptr_t controller, const float origin[3],
                             const float direction[3]) {
    // Throttle: the usable selector runs many times per frame and this
    // selection only needs to keep up with the player's aim, not the tick.
    static std::atomic<uint64_t> lastRunMs{0};
    const uint64_t nowMs = GetTickCount64();
    uint64_t lastRun = lastRunMs.load(std::memory_order_relaxed);
    if (nowMs >= lastRun && nowMs - lastRun < 30) return;
    if (!lastRunMs.compare_exchange_strong(lastRun, nowMs, std::memory_order_relaxed))
        return;

    TArray64 nearby = {};
    if (!ReadMem(controller + kDotPickupNearbyOffset, &nearby, sizeof(nearby)) ||
        !nearby.data || nearby.count <= 0 || nearby.count > 64)
        return;

    int32_t stride = s_dotPickupStride.load(std::memory_order_acquire);
    uintptr_t locationOffset = s_dotPickupLocationOffset.load(
        std::memory_order_acquire);
    const bool probeLayout = stride == 0 || !locationOffset ||
        !s_dotPickupDumped.load(std::memory_order_relaxed);
    const auto globals = probeLayout ? camera::GetUE3GlobalsSnapshot()
                                     : camera::UE3Globals{};
    TArray64 names = {};
    const bool namesValid = probeLayout && globals.gNamesValid &&
        ReadMem(globals.gNamesAddress, &names, sizeof(names));

    // Element layout: 8-byte raw pointers or 16-byte interface refs.
    if (stride == 0) {
        uintptr_t first = 0;
        uintptr_t second = 0;
        if (!ReadMem(nearby.data, &first, sizeof(first)) ||
            !ReadMem(nearby.data + sizeof(uintptr_t), &second, sizeof(second)) ||
            first < 0x10000)
            return;
        const uintptr_t difference = second > first ? second - first : first - second;
        stride = (nearby.count >= 2 && difference >= 0x100 && difference <= 0x1000)
            ? 16 : 8;
        if (stride == 16)
            s_dotPickupInterfaceDelta.store(difference, std::memory_order_release);
        s_dotPickupStride.store(stride, std::memory_order_release);
        Log("[PickupAim] NearbyPickupable layout: data=%p count=%d capacity=%d "
            "stride=%d", reinterpret_cast<void*>(nearby.data), nearby.count,
            nearby.capacity, stride);
    }

    if (!locationOffset && namesValid) {
        uintptr_t firstObject = 0;
        if (!ReadMem(nearby.data, &firstObject, sizeof(firstObject)) ||
            firstObject < 0x10000)
            return;
        uintptr_t firstClass = 0;
        if (!ReadMem(firstObject + globals.gObjectClassOffset, &firstClass,
                     sizeof(firstClass)) || firstClass < 0x10000)
            return;
        locationOffset = ResolvePickupLocationOffset(globals, names, firstClass);
        if (!locationOffset) return;
        s_dotPickupLocationOffset.store(locationOffset, std::memory_order_release);
        Log("[PickupAim] Pickup Location property offset=+0x%llX",
            static_cast<unsigned long long>(locationOffset));
    }

    uintptr_t interfaceDelta = s_dotPickupInterfaceDelta.load(
        std::memory_order_acquire);
    if (!interfaceDelta) {
        DotPickupRef currentTouched = {};
        if (ReadMem(controller + kDotPickupTouchedOffset, &currentTouched,
                    sizeof(currentTouched)) &&
            currentTouched.object >= 0x10000 &&
            currentTouched.interfacePointer > currentTouched.object) {
            const uintptr_t difference =
                currentTouched.interfacePointer - currentTouched.object;
            if (difference >= 0x40 && difference <= 0x1000) {
                interfaceDelta = difference;
                s_dotPickupInterfaceDelta.store(difference,
                                                std::memory_order_release);
            }
        }
    }
    if (!interfaceDelta && stride != 16) return;

    const int32_t count = nearby.count < 8 ? nearby.count : 8;
    uintptr_t bestObject = 0;
    float bestCosine = 0.966f;
    float bestDistance = 0.0f;
    DotPickupRef bestRef = {};
    for (int32_t index = 0; index < count; ++index) {
        DotPickupRef candidate = {};
        uintptr_t object = 0;
        if (stride == 16) {
            if (!ReadMem(nearby.data + static_cast<uint64_t>(index) * 16,
                         &candidate, sizeof(candidate)))
                continue;
            object = candidate.object;
        } else {
            if (!ReadMem(nearby.data + static_cast<uint64_t>(index) * 8,
                         &object, sizeof(object)))
                continue;
            candidate = {object, object + interfaceDelta};
        }
        if (object < 0x10000 || candidate.interfacePointer < 0x10000) continue;
        float location[3] = {};
        if (!ReadMem(object + locationOffset, location, sizeof(location))) continue;
        if (!std::isfinite(location[0]) || !std::isfinite(location[1]) ||
            !std::isfinite(location[2]))
            continue;
        const float dx = location[0] - origin[0];
        const float dy = location[1] - origin[1];
        const float dz = location[2] - origin[2];
        const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (distance < 1.0f || distance > 800.0f) continue;
        const float cosine = (dx * direction[0] + dy * direction[1] +
                              dz * direction[2]) / distance;
        if (!std::isfinite(cosine) || cosine <= bestCosine) continue;
        bestCosine = cosine;
        bestDistance = distance;
        bestObject = object;
        bestRef = candidate;
    }

    if (namesValid &&
        !s_dotPickupDumped.exchange(true, std::memory_order_acq_rel)) {
        const int32_t probeCount = count < 4 ? count : 4;
        for (int32_t index = 0; index < probeCount; ++index) {
            DotPickupRef candidate = {};
            if (stride == 16) {
                ReadMem(nearby.data + static_cast<uint64_t>(index) * 16,
                        &candidate, sizeof(candidate));
            } else {
                uintptr_t object = 0;
                ReadMem(nearby.data + static_cast<uint64_t>(index) * 8,
                        &object, sizeof(object));
                candidate = {object, object + interfaceDelta};
            }
            if (candidate.object < 0x10000) continue;
            char className[128] = {};
            char objectName[128] = {};
            ReadClassName(globals, names, candidate.object, className,
                          sizeof(className));
            ReadObjectName(globals, names, candidate.object, objectName,
                           sizeof(objectName));
            float location[3] = {};
            ReadMem(candidate.object + locationOffset, location, sizeof(location));
            Log("[PickupAim] candidate[%d] object=%p iface=%p %s.%s "
                "location=(%.1f,%.1f,%.1f)",
                index, reinterpret_cast<void*>(candidate.object),
                reinterpret_cast<void*>(candidate.interfacePointer), className,
                objectName, location[0], location[1], location[2]);
        }
    }

    if (!bestObject) return;
    if (!WriteMem(controller + kDotPickupTouchedOffset, &bestRef,
                  sizeof(bestRef)) ||
        !WriteMem(controller + kDotPickupSeenOffset, &bestRef, sizeof(bestRef)))
        return;
    const uint64_t logCount = s_dotPickupLogs.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (logCount <= 5 || logCount % 300 == 0) {
        Log("[PickupAim] Dot pickup selected: object=%p distance=%.1f cos=%.3f "
            "nearby=%d", reinterpret_cast<void*>(bestObject), bestDistance,
            bestCosine, nearby.count);
    }
}

void DumpRecentInteractionCalls(const char* tag, uintptr_t oldValue,
                                uintptr_t newValue) {
    static std::atomic<uint32_t> dumpCount{0};
    const uint32_t dump = dumpCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (dump > 40) return;

    const auto globals = camera::GetUE3GlobalsSnapshot();
    TArray64 names = {};
    if (!globals.gNamesValid ||
        !ReadMem(globals.gNamesAddress, &names, sizeof(names))) return;
    const uint64_t head = s_recentInteractionCallSequence.load(std::memory_order_acquire);
    Log("[InteractionAim] FocusCallHistory #%u tag=%s %p->%p head=%llu",
        dump, tag ? tag : "?", reinterpret_cast<void*>(oldValue),
        reinterpret_cast<void*>(newValue),
        static_cast<unsigned long long>(head));

    uintptr_t uniqueFunctions[40] = {};
    size_t uniqueCount = 0;
    for (uint64_t age = 0; age < kRecentInteractionCallCount && uniqueCount < 40;
         ++age) {
        if (head <= age) break;
        const uint64_t expected = head - age;
        auto& call = s_recentInteractionCalls[expected % kRecentInteractionCallCount];
        if (call.sequence.load(std::memory_order_acquire) != expected) continue;
        const uintptr_t function = call.function.load(std::memory_order_relaxed);
        const uintptr_t object = call.object.load(std::memory_order_relaxed);
        const uint32_t source = call.source.load(std::memory_order_relaxed);
        if (call.sequence.load(std::memory_order_acquire) != expected ||
            function < 0x10000) continue;
        bool duplicate = false;
        for (size_t index = 0; index < uniqueCount; ++index) {
            if (uniqueFunctions[index] == function) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        uniqueFunctions[uniqueCount++] = function;

        char functionName[128] = {};
        char ownerName[128] = {};
        uintptr_t owner = 0;
        ReadObjectName(globals, names, function, functionName, sizeof(functionName));
        if (ReadMem(function + 0x40, &owner, sizeof(owner)) && owner >= 0x10000)
            ReadObjectName(globals, names, owner, ownerName, sizeof(ownerName));
        const char* sourceName = "ProcessEvent";
        if (source == static_cast<uint32_t>(InteractionCallSource::CallFunction))
            sourceName = "CallFunction";
        else if (source == static_cast<uint32_t>(InteractionCallSource::ProcessInternal))
            sourceName = "ProcessInternal";
        Log("[InteractionAim] FocusCallHistory age=%llu source=%s object=%p "
            "function=%p %s.%s",
            static_cast<unsigned long long>(age),
            sourceName,
            reinterpret_cast<void*>(object), reinterpret_cast<void*>(function),
            ownerName[0] ? ownerName : "?",
            functionName[0] ? functionName : "?");
    }
}

void LogInteractionFocusWriter(const char* source, uintptr_t object,
                               uintptr_t function, uintptr_t oldUsable,
                               uintptr_t newUsable) {
    const auto globals = camera::GetUE3GlobalsSnapshot();
    TArray64 names = {};
    char functionName[128] = {};
    char ownerName[128] = {};
    uintptr_t owner = 0;
    if (globals.gNamesValid &&
        ReadMem(globals.gNamesAddress, &names, sizeof(names))) {
        ReadObjectName(globals, names, function, functionName, sizeof(functionName));
        if (ReadMem(function + 0x40, &owner, sizeof(owner)) && owner >= 0x10000)
            ReadObjectName(globals, names, owner, ownerName, sizeof(ownerName));
    }
    Log("[InteractionAim] Focus writer source=%s object=%p function=%p %s.%s "
        "usable=%p->%p",
        source, reinterpret_cast<void*>(object), reinterpret_cast<void*>(function),
        ownerName[0] ? ownerName : "?", functionName[0] ? functionName : "?",
        reinterpret_cast<void*>(oldUsable), reinterpret_cast<void*>(newUsable));
}

bool ReadObjectClass(const camera::UE3Globals& globals, uintptr_t object,
                     uintptr_t& classObject) {
    classObject = 0;
    return object >= 0x10000 && globals.gObjectClassOffset >= 0 &&
        ReadMem(object + globals.gObjectClassOffset, &classObject, sizeof(classObject)) &&
        classObject >= 0x10000;
}

bool ReadOuter(const camera::UE3Globals& globals, uintptr_t object, uintptr_t& outer) {
    outer = 0;
    return object >= 0x10000 && globals.gObjectNameOffset >= 8 &&
        ReadMem(object + globals.gObjectNameOffset - 8, &outer, sizeof(outer));
}

int ClassDistance(uintptr_t derivedClass, uintptr_t targetClass) {
    uintptr_t current = derivedClass;
    for (int depth = 0; depth < 64 && current >= 0x10000; ++depth) {
        if (current == targetClass) return depth;
        uintptr_t superClass = 0;
        if (!ReadMem(current + 0x78, &superClass, sizeof(superClass)) ||
            superClass == current) break;
        current = superClass;
    }
    return -1;
}

bool SelectCachedAimFunction(uintptr_t weaponClass, uintptr_t& function,
                             uintptr_t& owner, uint64_t& nameToken,
                             int& ownerDistance) {
    function = 0;
    owner = 0;
    nameToken = 0;
    ownerDistance = 65;
    bool ambiguous = false;
    AcquireSRWLockShared(&s_aimFunctionCacheLock);
    if (s_aimFunctionCacheValid) {
        for (size_t index = 0; index < s_aimFunctionCandidateCount; ++index) {
            const auto& candidate = s_aimFunctionCandidates[index];
            const int distance = ClassDistance(weaponClass, candidate.owner);
            if (distance < 0 || distance > ownerDistance) continue;
            if (distance < ownerDistance) {
                ownerDistance = distance;
                function = candidate.function;
                owner = candidate.owner;
                nameToken = candidate.nameToken;
                ambiguous = false;
            } else if (candidate.function != function) {
                ambiguous = true;
            }
        }
    }
    ReleaseSRWLockShared(&s_aimFunctionCacheLock);
    if (ambiguous) {
        function = 0;
        owner = 0;
        nameToken = 0;
    }
    return function != 0 && nameToken != 0;
}

bool ClassDerivesFrom(const camera::UE3Globals& globals, const TArray64& names,
                      uintptr_t derivedClass, const char* baseClassName) {
    uintptr_t current = derivedClass;
    for (int depth = 0; depth < 64 && current >= 0x10000; ++depth) {
        char className[128] = {};
        if (!ReadObjectName(globals, names, current, className, sizeof(className))) return false;
        if (strcmp(className, baseClassName) == 0) return true;
        uintptr_t superClass = 0;
        if (!ReadMem(current + 0x78, &superClass, sizeof(superClass)) ||
            superClass == current) break;
        current = superClass;
    }
    return false;
}

bool ValidateRuntimeObject(const camera::UE3Globals& globals, const TArray64& names,
                           uintptr_t object, const char* baseClassName,
                           uintptr_t& classObject, char* objectName,
                           size_t objectNameCapacity, char* className,
                           size_t classNameCapacity) {
    if (!ReadObjectClass(globals, object, classObject) ||
        !ReadObjectName(globals, names, object, objectName, objectNameCapacity) ||
        strncmp(objectName, "Default__", 9) == 0 ||
        !ReadObjectName(globals, names, classObject, className, classNameCapacity)) return false;
    return ClassDerivesFrom(globals, names, classObject, baseClassName);
}

bool FindObjectProperty(const camera::UE3Globals& globals, const TArray64& names,
                        const TArray64& objects, const char* propertyName,
                        uintptr_t instanceClass, int32_t& propertyOffset,
                        uintptr_t& propertyObject) {
    propertyOffset = -1;
    propertyObject = 0;
    int bestDistance = 65;
    bool ambiguous = false;
    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t object = 0;
        char objectName[128] = {};
        char className[128] = {};
        if (!ReadMem(objects.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                     &object, sizeof(object)) || !object ||
            !ReadObjectName(globals, names, object, objectName, sizeof(objectName)) ||
            strcmp(objectName, propertyName) != 0 ||
            !ReadClassName(globals, names, object, className, sizeof(className)) ||
            strcmp(className, "ObjectProperty") != 0) continue;

        uintptr_t ownerClass = 0;
        if (!ReadOuter(globals, object, ownerClass) || ownerClass < 0x10000) continue;
        const int distance = ClassDistance(instanceClass, ownerClass);
        if (distance < 0 || distance > bestDistance) continue;

        int32_t arrayDim = 0;
        int32_t elementSize = 0;
        int32_t offset = -1;
        if (!ReadMem(object + 0x68, &arrayDim, sizeof(arrayDim)) || arrayDim != 1 ||
            !ReadMem(object + 0x6C, &elementSize, sizeof(elementSize)) || elementSize != 8 ||
            !ReadMem(object + 0x8C, &offset, sizeof(offset)) ||
            offset <= 0 || offset >= 0x10000) continue;

        if (distance < bestDistance) {
            bestDistance = distance;
            propertyOffset = offset;
            propertyObject = object;
            ambiguous = false;
        } else if (object != propertyObject || offset != propertyOffset) {
            ambiguous = true;
        }
    }
    return propertyObject != 0 && !ambiguous;
}

bool FindUniqueNamedObject(const camera::UE3Globals& globals, const TArray64& names,
                           const TArray64& objects, const char* objectName,
                           const char* className, uintptr_t& result) {
    result = 0;
    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t object = 0;
        char candidateName[128] = {};
        char candidateClass[128] = {};
        if (!ReadMem(objects.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                     &object, sizeof(object)) || object < 0x10000 ||
            !ReadObjectName(globals, names, object, candidateName, sizeof(candidateName)) ||
            strcmp(candidateName, objectName) != 0 ||
            !ReadClassName(globals, names, object, candidateClass, sizeof(candidateClass)) ||
            strcmp(candidateClass, className) != 0) continue;
        if (result && result != object) return false;
        result = object;
    }
    return result != 0;
}

bool FindUniqueOwnedObject(const camera::UE3Globals& globals, const TArray64& names,
                           const TArray64& objects, const char* objectName,
                           const char* className, uintptr_t expectedOwner,
                           uintptr_t& result) {
    result = 0;
    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t object = 0;
        uintptr_t owner = 0;
        char candidateName[128] = {};
        char candidateClass[128] = {};
        if (!ReadMem(objects.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                     &object, sizeof(object)) || object < 0x10000 ||
            !ReadObjectName(globals, names, object, candidateName, sizeof(candidateName)) ||
            strcmp(candidateName, objectName) != 0 ||
            !ReadClassName(globals, names, object, candidateClass, sizeof(candidateClass)) ||
            strcmp(candidateClass, className) != 0 ||
            !ReadOuter(globals, object, owner) || owner != expectedOwner) continue;
        if (result && result != object) return false;
        result = object;
    }
    return result != 0;
}

bool ReadPropertyLayout(uintptr_t property, int32_t expectedElementSize,
                        int32_t& offset, int32_t& elementSize) {
    int32_t arrayDim = 0;
    offset = -1;
    elementSize = 0;
    return ReadMem(property + 0x68, &arrayDim, sizeof(arrayDim)) && arrayDim == 1 &&
        ReadMem(property + 0x6C, &elementSize, sizeof(elementSize)) &&
        elementSize == expectedElementSize &&
        ReadMem(property + 0x8C, &offset, sizeof(offset)) &&
        offset >= 0 && offset < 0x10000;
}

bool IsInputParameter(uintptr_t property) {
    constexpr uint64_t kParameter = 0x0000000000000080ull;
    constexpr uint64_t kOutOrReturn = 0x0000000000000500ull;
    uint64_t flags = 0;
    return ReadMem(property + 0x70, &flags, sizeof(flags)) &&
        (flags & kParameter) != 0 && (flags & kOutOrReturn) == 0;
}

bool PropertyReferencesStruct(uintptr_t property, uintptr_t expectedStruct) {
    int matches = 0;
    for (uintptr_t offset = 0x90; offset < 0xF0; offset += sizeof(uintptr_t)) {
        uintptr_t candidate = 0;
        if (ReadMem(property + offset, &candidate, sizeof(candidate)) &&
            candidate == expectedStruct) ++matches;
    }
    return matches == 1;
}

bool ResolveNativeExecTarget(uintptr_t function, uint64_t moduleBase,
                             uint32_t moduleSize, uintptr_t& target,
                             uint16_t& parameterSize) {
    constexpr uint32_t kFunctionNative = 0x00000400;
    uint32_t flags = 0;
    target = 0;
    parameterSize = 0;
    if (!ReadMem(function + 0xD0, &flags, sizeof(flags)) ||
        !(flags & kFunctionNative) ||
        !ReadMem(function + 0xE2, &parameterSize, sizeof(parameterSize)) ||
        !ReadMem(function + 0xF0, &target, sizeof(target)) ||
        target < moduleBase || target >= moduleBase + moduleSize) return false;

    MEMORY_BASIC_INFORMATION memory = {};
    return VirtualQuery(reinterpret_cast<void*>(target), &memory, sizeof(memory)) &&
        memory.State == MEM_COMMIT &&
        (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

struct MeleeDiscovery {
    uintptr_t meleeDefinitionClass = 0;
    uintptr_t meleeAttackFunction = 0;
    uintptr_t meleeAttackTarget = 0;
    uintptr_t findMeleeTargetFunction = 0;
    uintptr_t findMeleeTargetTarget = 0;
    int32_t contextParameterOffset = -1;
    int32_t traceScaleOffset = -1;
    int32_t radiusScaleOffset = -1;
    int32_t maxLungeDistanceParameterOffset = -1;
    uint16_t meleeAttackParameterSize = 0;
    uint16_t findMeleeTargetParameterSize = 0;
};

bool DiscoverMeleeMetadata(const camera::UE3Globals& globals, const TArray64& names,
                           const TArray64& objects, uint64_t moduleBase,
                           uint32_t moduleSize, MeleeDiscovery& discovery) {
    uintptr_t attributeInitializationData = 0;
    uintptr_t meleeAttackFunction = 0;
    uintptr_t traceDistanceProperty = 0;
    uintptr_t damageRadiusProperty = 0;
    uintptr_t scaleConstantProperty = 0;
    uintptr_t contextProperty = 0;
    int32_t traceOffset = -1;
    int32_t radiusOffset = -1;
    int32_t scaleOffset = -1;
    int32_t contextElementSize = 0;
    int32_t formulaSize = 0;
    int32_t radiusFormulaSize = 0;
    int32_t scaleSize = 0;
    uint32_t meleeDefinitionPropertySize = 0;

    const bool meleeMetadataValid =
        FindUniqueNamedObject(globals, names, objects, "MeleeDefinition", "Class",
                              discovery.meleeDefinitionClass) &&
        FindUniqueOwnedObject(globals, names, objects, "MeleeAttack", "Function",
                              discovery.meleeDefinitionClass, meleeAttackFunction) &&
        FindUniqueOwnedObject(globals, names, objects, "TraceDistanceFormula",
                              "StructProperty", discovery.meleeDefinitionClass,
                              traceDistanceProperty) &&
        FindUniqueOwnedObject(globals, names, objects, "DamageRadiusFormula",
                              "StructProperty", discovery.meleeDefinitionClass,
                              damageRadiusProperty) &&
        FindUniqueOwnedObject(globals, names, objects, "ContextObject", "ObjectProperty",
                              meleeAttackFunction, contextProperty) &&
        FindUniqueNamedObject(globals, names, objects, "AttributeInitializationData",
                              "ScriptStruct", attributeInitializationData) &&
        FindUniqueOwnedObject(globals, names, objects, "BaseValueScaleConstant",
                              "FloatProperty", attributeInitializationData,
                              scaleConstantProperty) &&
        ReadMem(discovery.meleeDefinitionClass + 0x84,
                &meleeDefinitionPropertySize, sizeof(meleeDefinitionPropertySize)) &&
        ReadPropertyLayout(traceDistanceProperty, 0x20, traceOffset, formulaSize) &&
        ReadPropertyLayout(damageRadiusProperty, formulaSize, radiusOffset,
                           radiusFormulaSize) &&
        traceOffset + formulaSize <= static_cast<int32_t>(meleeDefinitionPropertySize) &&
        radiusOffset + radiusFormulaSize <=
            static_cast<int32_t>(meleeDefinitionPropertySize) &&
        PropertyReferencesStruct(traceDistanceProperty, attributeInitializationData) &&
        PropertyReferencesStruct(damageRadiusProperty, attributeInitializationData) &&
        ReadPropertyLayout(scaleConstantProperty, 4, scaleOffset, scaleSize) &&
        ReadPropertyLayout(contextProperty, 8, discovery.contextParameterOffset,
                           contextElementSize) &&
        IsInputParameter(contextProperty) &&
        scaleOffset + scaleSize <= formulaSize &&
        ResolveNativeExecTarget(meleeAttackFunction, moduleBase, moduleSize,
                                discovery.meleeAttackTarget,
                                discovery.meleeAttackParameterSize) &&
        discovery.contextParameterOffset + contextElementSize <=
            discovery.meleeAttackParameterSize;
    if (meleeMetadataValid) {
        discovery.meleeAttackFunction = meleeAttackFunction;
        discovery.traceScaleOffset = traceOffset + scaleOffset;
        discovery.radiusScaleOffset = radiusOffset + scaleOffset;
    } else {
        discovery.meleeDefinitionClass = 0;
        discovery.meleeAttackFunction = 0;
        discovery.meleeAttackTarget = 0;
    }

    uintptr_t willowPawnClass = 0;
    uintptr_t findMeleeTargetFunction = 0;
    uintptr_t maxLungeDistanceProperty = 0;
    int32_t lungeElementSize = 0;
    const bool lungeMetadataValid =
        FindUniqueNamedObject(globals, names, objects, "WillowPawn", "Class",
                              willowPawnClass) &&
        FindUniqueOwnedObject(globals, names, objects, "FindMeleeTarget", "Function",
                              willowPawnClass, findMeleeTargetFunction) &&
        FindUniqueOwnedObject(globals, names, objects, "MaxLungeDistance", "FloatProperty",
                              findMeleeTargetFunction, maxLungeDistanceProperty) &&
        ReadPropertyLayout(maxLungeDistanceProperty, 4,
                           discovery.maxLungeDistanceParameterOffset,
                           lungeElementSize) &&
        IsInputParameter(maxLungeDistanceProperty) &&
        ResolveNativeExecTarget(findMeleeTargetFunction, moduleBase, moduleSize,
                                discovery.findMeleeTargetTarget,
                                discovery.findMeleeTargetParameterSize) &&
        discovery.maxLungeDistanceParameterOffset + lungeElementSize <=
            discovery.findMeleeTargetParameterSize;
    if (!lungeMetadataValid) {
        discovery.findMeleeTargetFunction = 0;
        discovery.findMeleeTargetTarget = 0;
        discovery.maxLungeDistanceParameterOffset = -1;
    } else {
        discovery.findMeleeTargetFunction = findMeleeTargetFunction;
    }
    if (discovery.meleeAttackTarget &&
        discovery.meleeAttackTarget == discovery.findMeleeTargetTarget) {
        discovery.findMeleeTargetFunction = 0;
        discovery.findMeleeTargetTarget = 0;
        discovery.maxLungeDistanceParameterOffset = -1;
    }
    return meleeMetadataValid || lungeMetadataValid;
}

} // namespace

WeaponAimSystem& WeaponAimSystem::Instance() {
    static WeaponAimSystem system;
    return system;
}

void BuildCalibratedLocalForward(float pitchDegrees, float yawDegrees, float output[3]) {
    if (!output) return;
    constexpr float kDegreesToRadians = 0.01745329251994329577f;
    const float pitch = pitchDegrees * kDegreesToRadians;
    const float yaw = yawDegrees * kDegreesToRadians;
    const float cosPitch = cosf(pitch);
    output[0] = sinf(yaw) * cosPitch;
    output[1] = sinf(pitch);
    output[2] = -cosf(yaw) * cosPitch;
}

void WeaponAimSystem::UpdateDirection(const float worldOrigin[3],
                                      const float worldDirection[3],
                                      float convergenceMeters) {
    if (!worldOrigin || !worldDirection) return;
    const float x = worldDirection[0];
    const float y = worldDirection[1];
    const float z = worldDirection[2];
    const float horizontal = sqrtf(x * x + y * y);
    const float length = sqrtf(x * x + y * y + z * z);
    if (!std::isfinite(horizontal) || !std::isfinite(z) ||
        !std::isfinite(length) || length < 1.0e-5f ||
        !std::isfinite(worldOrigin[0]) || !std::isfinite(worldOrigin[1]) ||
        !std::isfinite(worldOrigin[2])) return;

    const int32_t pitch = static_cast<int32_t>(lroundf(
        atan2f(z, horizontal) * kRadiansToUnis));
    const int32_t yaw = static_cast<int32_t>(lroundf(
        atan2f(y, x) * kRadiansToUnis));

    if (!std::isfinite(convergenceMeters)) return;
    const float targetDistanceUe = (std::max)(1.0f,
        (std::min)(100.0f, convergenceMeters)) * 100.0f;
    const float target[3] = {
        worldOrigin[0] + x / length * targetDistanceUe,
        worldOrigin[1] + y / length * targetDistanceUe,
        worldOrigin[2] + z / length * targetDistanceUe
    };

    // Publish one coherent ballistic packet. The script hook may run on another
    // engine thread and must not combine an old origin with a new target.
    AcquireSRWLockExclusive(&m_ballisticOverrideLock);
    m_aimPitch.store(pitch, std::memory_order_relaxed);
    m_aimYaw.store(yaw, std::memory_order_relaxed);
    m_aimRoll.store(0, std::memory_order_relaxed);
    m_aimOriginX.store(worldOrigin[0], std::memory_order_relaxed);
    m_aimOriginY.store(worldOrigin[1], std::memory_order_relaxed);
    m_aimOriginZ.store(worldOrigin[2], std::memory_order_relaxed);
    m_aimTargetX.store(target[0], std::memory_order_relaxed);
    m_aimTargetY.store(target[1], std::memory_order_relaxed);
    m_aimTargetZ.store(target[2], std::memory_order_relaxed);
    m_aimTargetValid.store(true, std::memory_order_release);
    m_aimUpdatedMs.store(GetTickCount64(), std::memory_order_release);
    m_aimValid.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&m_ballisticOverrideLock);

    static std::atomic<bool> loggedFirst{false};
    if (!loggedFirst.exchange(true)) {
        Log("[WeaponAim] UpdateDirection first call: dir=(%.3f,%.3f,%.3f) "
            "rot=(%d,%d,%d) target=%.1fm", x, y, z, pitch, yaw, 0,
            targetDistanceUe * 0.01f);
    }
    SampleInteractionFields();
}

void WeaponAimSystem::SampleInteractionFields() {
    constexpr uintptr_t kTouchedOffset = 0xE64;
    constexpr uintptr_t kSeenOffset = 0xE74;
    constexpr uintptr_t kUsableOffset = 0xE84;
    constexpr uintptr_t kInteractionClientOffset = 0xDF0;
    constexpr uintptr_t kInteractionsOffset = 0x8E4;
    constexpr uintptr_t kInteractDistanceOffset = 0x944;
    constexpr uintptr_t kInteractionIconOffset = 0xC60;

    const uint64_t nowMs = GetTickCount64();
    uint64_t nextMs = m_nextInteractionSampleMs.load(std::memory_order_relaxed);
    if (nowMs < nextMs || !m_nextInteractionSampleMs.compare_exchange_strong(
            nextMs, nowMs + 50, std::memory_order_acq_rel)) return;

    const uintptr_t controller = m_localController.load(std::memory_order_acquire);
    if (controller < 0x10000 ||
        !m_pawnIdentityValid.load(std::memory_order_acquire)) return;
    struct InterfaceRef {
        uintptr_t object;
        uintptr_t interfacePointer;
    };
    InterfaceRef touched = {};
    InterfaceRef seen = {};
    InterfaceRef usable = {};
    TArray64 interactions = {};
    uintptr_t interactionClient = 0;
    float interactDistance = 0.0f;
    uint8_t icon = 0;
    if (!ReadMem(controller + kTouchedOffset, &touched, sizeof(touched)) ||
        !ReadMem(controller + kSeenOffset, &seen, sizeof(seen)) ||
        !ReadMem(controller + kUsableOffset, &usable, sizeof(usable)) ||
        !ReadMem(controller + kInteractionClientOffset, &interactionClient,
                 sizeof(interactionClient)) ||
        !ReadMem(controller + kInteractionsOffset, &interactions,
                 sizeof(interactions)) ||
        !ReadMem(controller + kInteractDistanceOffset, &interactDistance,
                 sizeof(interactDistance)) ||
        !ReadMem(controller + kInteractionIconOffset, &icon, sizeof(icon))) return;

    uint32_t distanceBits = 0;
    memcpy(&distanceBits, &interactDistance, sizeof(distanceBits));
    const uint32_t scalars = (distanceBits * 16777619u) ^ icon;
    const uintptr_t oldUsable = m_sampledUsableObject.exchange(usable.object);
    const uintptr_t oldTouched = m_sampledTouchedObject.exchange(touched.object);
    const uintptr_t oldSeen = m_sampledSeenObject.exchange(seen.object);
    bool changed = m_sampledInteractionController.exchange(controller) != controller;
    changed |= oldTouched != touched.object;
    changed |= oldSeen != seen.object;
    changed |= oldUsable != usable.object;
    changed |= m_sampledInteractionClient.exchange(interactionClient) != interactionClient;
    changed |= m_sampledInteractionsData.exchange(interactions.data) != interactions.data;
    changed |= m_sampledInteractionsCount.exchange(interactions.count) != interactions.count;
    changed |= m_sampledInteractionScalars.exchange(scalars) != scalars;
    if (!changed) return;

    Log("[InteractionAim] FocusState ctl=%p touched=%p/%p seen=%p/%p "
        "usable=%p/%p client=%p interactions=%p[%d/%d] distance=%.2f icon=%u",
        reinterpret_cast<void*>(controller),
        reinterpret_cast<void*>(touched.object),
        reinterpret_cast<void*>(touched.interfacePointer),
        reinterpret_cast<void*>(seen.object),
        reinterpret_cast<void*>(seen.interfacePointer),
        reinterpret_cast<void*>(usable.object),
        reinterpret_cast<void*>(usable.interfacePointer),
        reinterpret_cast<void*>(interactionClient),
        reinterpret_cast<void*>(interactions.data), interactions.count,
        interactions.capacity, interactDistance, static_cast<unsigned>(icon));
    if (oldUsable != usable.object)
        DumpRecentInteractionCalls("usable", oldUsable, usable.object);
    if ((oldTouched != touched.object || oldSeen != seen.object) &&
        (touched.object || seen.object))
        DumpRecentInteractionCalls("pickup", oldTouched,
                                   touched.object ? touched.object : seen.object);

    const auto globals = camera::GetUE3GlobalsSnapshot();
    if (!globals.gNamesValid) return;
    TArray64 names = {};
    if (!ReadMem(globals.gNamesAddress, &names, sizeof(names))) return;
    const uintptr_t objects[] = {touched.object, seen.object, usable.object,
                                 interactionClient};
    const char* tags[] = {"touched", "seen", "usable", "client"};
    for (size_t index = 0; index < _countof(objects); ++index) {
        if (objects[index] < 0x10000) continue;
        char className[128] = {};
        char objectName[128] = {};
        if (ReadClassName(globals, names, objects[index], className,
                          sizeof(className)) &&
            ReadObjectName(globals, names, objects[index], objectName,
                           sizeof(objectName))) {
            Log("[InteractionAim] FocusState %s=%p %s::%s", tags[index],
                reinterpret_cast<void*>(objects[index]), className, objectName);
        }
    }
}

void WeaponAimSystem::InvalidateDirection() {
    AcquireSRWLockExclusive(&m_ballisticOverrideLock);
    m_aimValid.store(false, std::memory_order_release);
    m_aimTargetValid.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&m_ballisticOverrideLock);
}

void WeaponAimSystem::SetBallisticOverrideEnabled(bool enabled) {
    AcquireSRWLockExclusive(&m_ballisticOverrideLock);
    m_ballisticOverrideEnabled.store(enabled, std::memory_order_release);
    ReleaseSRWLockExclusive(&m_ballisticOverrideLock);
}

uintptr_t WeaponAimSystem::FindProcessEvent(uint64_t controllerAddress,
                                               uint64_t moduleBase,
                                               uint32_t moduleSize) {
    (void)controllerAddress;
    // Exact BL1 Enhanced signature and ABI from bl-sdk/unrealsdk's BL1E
    // implementation. The old vtable[67] target was ProcessInternal.
    const auto matches = camera::ScanPattern(
        static_cast<uintptr_t>(moduleBase), moduleSize,
        "40 55 41 56 41 57 48 81 EC 90 00 00 00");
    uintptr_t candidate = 0;
    size_t executableMatches = 0;
    for (const auto& match : matches) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(reinterpret_cast<void*>(match.address), &memory,
                          sizeof(memory)) || memory.State != MEM_COMMIT ||
            !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;
        candidate = match.address;
        ++executableMatches;
    }
    if (executableMatches != 1) {
        Log("[WeaponAim] BL1E ProcessEvent signature ambiguous: total=%zu executable=%zu",
            matches.size(), executableMatches);
        return 0;
    }
    Log("[WeaponAim] BL1E ProcessEvent signature validated: %p (RVA 0x%llX)",
        reinterpret_cast<void*>(candidate),
        static_cast<unsigned long long>(candidate - moduleBase));
    return candidate;
}

uintptr_t WeaponAimSystem::FindCallFunction(uint64_t moduleBase, uint32_t moduleSize) {
    const auto matches = camera::ScanPattern(
        static_cast<uintptr_t>(moduleBase), moduleSize,
        "40 55 53 56 57 41 54 41 55 41 56 41 57 48 81 EC A8 04 00 00");
    uintptr_t candidate = 0;
    size_t executableMatches = 0;
    for (const auto& match : matches) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(reinterpret_cast<void*>(match.address), &memory,
                          sizeof(memory)) || memory.State != MEM_COMMIT ||
            !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;
        candidate = match.address;
        ++executableMatches;
    }
    if (executableMatches != 1) {
        Log("[InteractionAim] BL1E CallFunction signature ambiguous: total=%zu executable=%zu",
            matches.size(), executableMatches);
        return 0;
    }
    Log("[InteractionAim] BL1E CallFunction signature validated: %p (RVA 0x%llX)",
        reinterpret_cast<void*>(candidate),
        static_cast<unsigned long long>(candidate - moduleBase));
    return candidate;
}

uintptr_t WeaponAimSystem::FindUsableSelector(uint64_t moduleBase,
                                              uint32_t moduleSize) {
    // Confirmed by the PAGE_GUARD watchpoint on CurrentUsableObject (+0xE84):
    // the writer at RVA 0x1461710 calls this selector with the controller and a
    // 16-byte InterfaceRef out parameter. The selector traces from the
    // controller's CalcViewLocation (+0xCA8) along CalcViewRotation (+0xCB4).
    const auto matches = camera::ScanPattern(
        static_cast<uintptr_t>(moduleBase), moduleSize,
        "48 89 7C 24 20 55 41 54 41 56 48 8D 6C 24 D0 48 81 EC 30 01 00 00 "
        "4C 8B F1 4C 8B E2");
    uintptr_t candidate = 0;
    size_t executableMatches = 0;
    for (const auto& match : matches) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(reinterpret_cast<void*>(match.address), &memory,
                          sizeof(memory)) || memory.State != MEM_COMMIT ||
            !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;
        candidate = match.address;
        ++executableMatches;
    }
    if (executableMatches != 1) {
        Log("[InteractionAim] Usable selector signature ambiguous: total=%zu "
            "executable=%zu",
            matches.size(), executableMatches);
        return 0;
    }
    Log("[InteractionAim] Usable selector signature validated: %p (RVA 0x%llX)",
        reinterpret_cast<void*>(candidate),
        static_cast<unsigned long long>(candidate - moduleBase));
    return candidate;
}

bool WeaponAimSystem::FindPawnAimRotation(uint64_t controllerAddress) {
    const auto globals = camera::GetUE3GlobalsSnapshot();
    if (!globals.gNamesValid || !globals.gObjectsValid) return false;

    TArray64 names = {};
    if (!ReadMem(globals.gNamesAddress, &names, sizeof(names))) return false;

    // Scan controller memory for pointers to Pawn-class objects
    uintptr_t pawn = 0;
    for (uintptr_t offset = 0x40; offset < 0x2000; offset += 8) {
        uintptr_t candidate = 0;
        if (!ReadMem(controllerAddress + offset, &candidate, sizeof(candidate))) continue;
        if (candidate < 0x10000) continue;

        char className[128] = {};
        if (ReadClassName(globals, names, candidate, className, sizeof(className)) &&
            strstr(className, "Pawn") && !strstr(className, "Default__")) {
            pawn = candidate;
            Log("[WeaponAim] Pawn found at controller+0x%llX: %p class=%s",
                static_cast<unsigned long long>(offset),
                reinterpret_cast<void*>(pawn), className);
            break;
        }
    }

    if (!pawn) {
        Log("[WeaponAim] Pawn not found in controller memory");
        return false;
    }

    // Read the current camera rotation to use as reference
    const int32_t* cameraRot = camera::GetCameraRotation();
    if (!cameraRot) return false;
    int32_t refPitch = cameraRot[0];
    int32_t refYaw = cameraRot[1];

    // Scan pawn memory for a FRotator that matches the camera rotation
    // AimRotation is typically 3 int32s (Pitch, Yaw, Roll)
    for (uintptr_t offset = 0x80; offset < 0x2000; offset += 4) {
        int32_t rot[3] = {};
        if (!ReadMem(pawn + offset, rot, sizeof(rot))) continue;

        // Check if this looks like a valid rotation matching the camera
        const int32_t pitchDiff = abs(rot[0] - refPitch);
        const int32_t yawDiff = abs(rot[1] - refYaw);
        const bool pitchMatch = pitchDiff < 5000 || pitchDiff > 60536;
        const bool yawMatch = yawDiff < 5000 || yawDiff > 60536;

        if (pitchMatch && yawMatch && abs(rot[2]) < 1000) {
            // Validate: read again to confirm it's stable
            int32_t rot2[3] = {};
            if (ReadMem(pawn + offset, rot2, sizeof(rot2)) &&
                abs(rot2[0] - rot[0]) < 100 && abs(rot2[1] - rot[1]) < 100) {
                m_pawnAimRotationAddr.store(pawn + offset, std::memory_order_release);
                Log("[WeaponAim] AimRotation found at pawn+0x%llX: (%d,%d,%d) "
                    "camera=(%d,%d,%d)",
                    static_cast<unsigned long long>(offset),
                    rot[0], rot[1], rot[2], refPitch, refYaw, cameraRot[2]);
                return true;
            }
        }
    }

    Log("[WeaponAim] AimRotation not found on pawn (camera=(%d,%d,%d))",
        refPitch, refYaw, cameraRot[2]);
    return false;
}

bool WeaponAimSystem::Install(uintptr_t target) {
    if (m_hookInstalled.load(std::memory_order_acquire)) return true;
    const MH_STATUS createStatus = MH_CreateHook(reinterpret_cast<void*>(target),
        &HookedProcessEvent,
        reinterpret_cast<void**>(&m_originalProcessEvent));
    if (createStatus != MH_OK) {
        Log("[WeaponAim] ProcessEvent hook creation failed: %s",
            MH_StatusToString(createStatus));
        m_originalProcessEvent = nullptr;
        return false;
    }

    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enableStatus != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        Log("[WeaponAim] ProcessEvent hook enable failed: %s",
            MH_StatusToString(enableStatus));
        m_originalProcessEvent = nullptr;
        return false;
    }
    m_processEventTarget = target;
    m_hookInstalled.store(true, std::memory_order_release);
    Log("[WeaponAim] Read-only ProcessEvent probe installed at %p",
        reinterpret_cast<void*>(target));
    return true;
}

bool WeaponAimSystem::InstallNativeAimProbe(uint64_t moduleBase, uint32_t moduleSize) {
    if (m_nativeAimTarget && m_originalGetAimRotation) return true;
    constexpr uintptr_t kGetAimRotationRva = 0x013075A0;
    const uintptr_t target = static_cast<uintptr_t>(moduleBase) + kGetAimRotationRva;
    if (target < moduleBase || target >= moduleBase + moduleSize) {
        Log("[WeaponAim] Native GetAimRotation RVA is outside the game image");
        return false;
    }

    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(reinterpret_cast<void*>(target), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT ||
        !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        Log("[WeaponAim] Native GetAimRotation target is not executable: %p protect=0x%X",
            reinterpret_cast<void*>(target), memory.Protect);
        return false;
    }

    unsigned char bytes[16] = {};
    if (!ReadMem(target, bytes, sizeof(bytes))) return false;
    Log("[WeaponAim] Native GetAimRotation probe target=%p RVA=0x%llX "
        "bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
        reinterpret_cast<void*>(target),
        static_cast<unsigned long long>(kGetAimRotationRva),
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);

    const MH_STATUS createStatus = MH_CreateHook(reinterpret_cast<void*>(target),
        &HookedGetAimRotation, reinterpret_cast<void**>(&m_originalGetAimRotation));
    if (createStatus != MH_OK) {
        Log("[WeaponAim] Native GetAimRotation probe creation failed: %s",
            MH_StatusToString(createStatus));
        m_originalGetAimRotation = nullptr;
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enableStatus != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        Log("[WeaponAim] Native GetAimRotation probe enable failed: %s",
            MH_StatusToString(enableStatus));
        m_originalGetAimRotation = nullptr;
        return false;
    }
    m_nativeAimTarget = target;
    Log("[WeaponAim] Native GetAimRotation read-only probe installed");
    return true;
}

bool WeaponAimSystem::InstallCallFunction(uintptr_t target) {
    if (m_callFunctionInstalled.load(std::memory_order_acquire)) return true;
    const MH_STATUS createStatus = MH_CreateHook(
        reinterpret_cast<void*>(target), &HookedCallFunction,
        reinterpret_cast<void**>(&m_originalCallFunction));
    if (createStatus != MH_OK) {
        Log("[InteractionAim] CallFunction hook creation failed: %s",
            MH_StatusToString(createStatus));
        m_originalCallFunction = nullptr;
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enableStatus != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        Log("[InteractionAim] CallFunction hook enable failed: %s",
            MH_StatusToString(enableStatus));
        m_originalCallFunction = nullptr;
        return false;
    }
    m_callFunctionTarget = target;
    m_callFunctionInstalled.store(true, std::memory_order_release);
    Log("[InteractionAim] BL1E CallFunction probe installed at %p",
        reinterpret_cast<void*>(target));
    return true;
}

bool WeaponAimSystem::InstallGameplayStateProbes(
        uintptr_t injuredFunction, uintptr_t phaseWalkFunction,
        uint64_t moduleBase, uint32_t moduleSize) {
    struct Probe {
        const char* name;
        uintptr_t function;
        uintptr_t* target;
        GameplayStateFn hook;
        GameplayStateFn* original;
    };
    Probe probes[] = {
        {"IsInjured", injuredFunction, &m_isInjuredTarget,
         &HookedIsInjured, &m_originalIsInjured},
        {"PhaseWalk", phaseWalkFunction, &m_phaseWalkVisibilityTarget,
         &HookedPhaseWalkVisibility, &m_originalPhaseWalkVisibility}
    };
    bool installedAny = false;
    for (Probe& probe : probes) {
        if (*probe.target && *probe.original) {
            installedAny = true;
            continue;
        }
        uintptr_t target = 0;
        if (probe.function < 0x10000 ||
            !ReadMem(probe.function + 0xF0, &target, sizeof(target)) ||
            target < moduleBase || target >= moduleBase + moduleSize) {
            Log("[WeaponAim] %s native state target unavailable: function=%p target=%p",
                probe.name, reinterpret_cast<void*>(probe.function),
                reinterpret_cast<void*>(target));
            continue;
        }
        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(reinterpret_cast<void*>(target), &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT ||
            !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            Log("[WeaponAim] %s native state target is not executable: %p",
                probe.name, reinterpret_cast<void*>(target));
            continue;
        }
        const MH_STATUS createStatus = MH_CreateHook(
            reinterpret_cast<void*>(target), probe.hook,
            reinterpret_cast<void**>(probe.original));
        if (createStatus != MH_OK) {
            Log("[WeaponAim] %s native state hook creation failed: %s",
                probe.name, MH_StatusToString(createStatus));
            *probe.original = nullptr;
            continue;
        }
        const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
        if (enableStatus != MH_OK) {
            MH_RemoveHook(reinterpret_cast<void*>(target));
            *probe.original = nullptr;
            Log("[WeaponAim] %s native state hook enable failed: %s",
                probe.name, MH_StatusToString(enableStatus));
            continue;
        }
        *probe.target = target;
        installedAny = true;
        Log("[WeaponAim] %s native state probe installed at %p (RVA 0x%llX)",
            probe.name, reinterpret_cast<void*>(target),
            static_cast<unsigned long long>(target - moduleBase));
    }
    return installedAny;
}

bool WeaponAimSystem::InstallMeleeAttackHook(
        uintptr_t function, uintptr_t target, uintptr_t meleeDefinitionClass,
        int32_t objectClassOffset, int32_t contextParameterOffset,
        int32_t traceScaleOffset, int32_t radiusScaleOffset) {
    if (m_meleeAttackInstalled.load(std::memory_order_acquire))
        return target == m_meleeAttackTarget;
    if (function < 0x10000 || target < 0x10000 || meleeDefinitionClass < 0x10000 ||
        objectClassOffset < 0 || contextParameterOffset < 0 ||
        traceScaleOffset <= 0 || radiusScaleOffset <= 0 ||
        traceScaleOffset == radiusScaleOffset) return false;

    const MH_STATUS createStatus = MH_CreateHook(
        reinterpret_cast<void*>(target), &HookedMeleeAttack,
        reinterpret_cast<void**>(&m_originalMeleeAttack));
    if (createStatus != MH_OK) {
        Log("[MeleeRange] MeleeAttack hook creation failed: %s",
            MH_StatusToString(createStatus));
        m_originalMeleeAttack = nullptr;
        return false;
    }

    m_meleeAttackFunction = function;
    m_meleeAttackTarget = target;
    m_meleeDefinitionClass = meleeDefinitionClass;
    m_meleeObjectClassOffset = objectClassOffset;
    m_meleeContextParameterOffset = contextParameterOffset;
    m_traceScaleOffset = traceScaleOffset;
    m_radiusScaleOffset = radiusScaleOffset;
    m_meleeMutationSafe.store(true, std::memory_order_release);
    m_meleeHooksStopping.store(false, std::memory_order_release);
    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enableStatus != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        Log("[MeleeRange] MeleeAttack hook enable failed: %s",
            MH_StatusToString(enableStatus));
        m_meleeAttackTarget = 0;
        m_meleeAttackFunction = 0;
        m_originalMeleeAttack = nullptr;
        m_meleeDefinitionClass = 0;
        m_meleeObjectClassOffset = -1;
        m_meleeContextParameterOffset = -1;
        m_traceScaleOffset = -1;
        m_radiusScaleOffset = -1;
        return false;
    }
    m_meleeAttackInstalled.store(true, std::memory_order_release);
    Log("[MeleeRange] MeleeAttack hook installed at %p: context=+0x%X "
        "traceScale=+0x%X radiusScale=+0x%X multiplier=%.2f",
        reinterpret_cast<void*>(target), contextParameterOffset,
        traceScaleOffset, radiusScaleOffset,
        config::GetMeleeRangeMultiplier());
    return true;
}

bool WeaponAimSystem::InstallFindMeleeTargetHook(uintptr_t function, uintptr_t target,
                                                  int32_t parameterOffset) {
    if (m_findMeleeTargetInstalled.load(std::memory_order_acquire))
        return target == m_findMeleeTargetTarget;
    if (function < 0x10000 || target < 0x10000 || parameterOffset < 0) return false;

    const MH_STATUS createStatus = MH_CreateHook(
        reinterpret_cast<void*>(target), &HookedFindMeleeTarget,
        reinterpret_cast<void**>(&m_originalFindMeleeTarget));
    if (createStatus != MH_OK) {
        Log("[MeleeRange] FindMeleeTarget hook creation failed: %s",
            MH_StatusToString(createStatus));
        m_originalFindMeleeTarget = nullptr;
        return false;
    }
    m_findMeleeTargetFunction = function;
    m_findMeleeTargetTarget = target;
    m_maxLungeDistanceParameterOffset = parameterOffset;
    m_meleeHooksStopping.store(false, std::memory_order_release);
    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enableStatus != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        Log("[MeleeRange] FindMeleeTarget hook enable failed: %s",
            MH_StatusToString(enableStatus));
        m_findMeleeTargetTarget = 0;
        m_findMeleeTargetFunction = 0;
        m_originalFindMeleeTarget = nullptr;
        m_maxLungeDistanceParameterOffset = -1;
        return false;
    }
    m_findMeleeTargetInstalled.store(true, std::memory_order_release);
    Log("[MeleeRange] FindMeleeTarget hook installed at %p: MaxLungeDistance=+0x%X",
        reinterpret_cast<void*>(target), parameterOffset);
    return true;
}

bool WeaponAimSystem::InstallTraceProbe(uintptr_t function,
                                         uint64_t moduleBase,
                                         uint32_t moduleSize, const char* tag,
                                         uintptr_t& targetOut, NativeExecFn hookFn,
                                         NativeExecFn& originalOut,
                                         std::atomic<bool>& installedFlag) {
    if (installedFlag.load(std::memory_order_acquire)) return true;
    uintptr_t target = 0;
    uint16_t parameterSize = 0;
    if (!ResolveNativeExecTarget(function, moduleBase, moduleSize, target,
                                 parameterSize)) {
        Log("[InteractionAim] %s native target unavailable: function=%p",
            tag, reinterpret_cast<void*>(function));
        return false;
    }
    const MH_STATUS createStatus = MH_CreateHook(
        reinterpret_cast<void*>(target), hookFn,
        reinterpret_cast<void**>(&originalOut));
    if (createStatus != MH_OK) {
        Log("[InteractionAim] %s hook creation failed: %s",
            tag, MH_StatusToString(createStatus));
        originalOut = nullptr;
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enableStatus != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        originalOut = nullptr;
        Log("[InteractionAim] %s hook enable failed: %s",
            tag, MH_StatusToString(enableStatus));
        return false;
    }
    targetOut = target;
    installedFlag.store(true, std::memory_order_release);
    Log("[InteractionAim] %s probe installed: function=%p target=%p params=%u",
        tag, reinterpret_cast<void*>(function),
        reinterpret_cast<void*>(target), parameterSize);
    return true;
}

bool WeaponAimSystem::TraceGateScan(void* frame, const char* tag,
                                    uint32_t& startOffsetOut,
                                    float& lengthOut, bool logOnly) {
    // Log-only scan: reports viewpoint-originated traces without rewriting.
    // Returns true when Start sits at the live camera with a sane length.
    auto& system = Instance();
    if (!frame) return false;
    uintptr_t locals = 0;
    if (!ReadMem(reinterpret_cast<uintptr_t>(frame) + 0x18,
                 &locals, sizeof(locals)) || locals < 0x10000) return false;
    const float* cameraLocation = camera::GetCameraLocation();
    float camLoc[3] = {};
    if (!cameraLocation ||
        !ReadDirect(reinterpret_cast<uintptr_t>(cameraLocation),
                    camLoc, sizeof(camLoc))) return false;
    static const uint32_t kStartCandidates[] = {52, 56, 60, 64, 68, 72, 76, 80, 84, 88};
    for (uint32_t startOffset : kStartCandidates) {
        const uint32_t endOffset = startOffset - 12;
        float traceStart[3] = {};
        float traceEnd[3] = {};
        if (!ReadDirect(locals + startOffset, traceStart, sizeof(traceStart)) ||
            !ReadDirect(locals + endOffset, traceEnd, sizeof(traceEnd))) continue;
        const float sdx = traceStart[0] - camLoc[0];
        const float sdy = traceStart[1] - camLoc[1];
        const float sdz = traceStart[2] - camLoc[2];
        if (sdx * sdx + sdy * sdy + sdz * sdz > 50.0f * 50.0f) continue;
        const float edx = traceEnd[0] - traceStart[0];
        const float edy = traceEnd[1] - traceStart[1];
        const float edz = traceEnd[2] - traceStart[2];
        const float length = sqrtf(edx * edx + edy * edy + edz * edz);
        if (!std::isfinite(length) || length < 50.0f || length > 12000.0f) continue;
        startOffsetOut = startOffset;
        lengthOut = length;
        if (logOnly) {
            const uint64_t count = system.m_traceProbeHits.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (count <= 10 || count % 600 == 0) {
                Log("[InteractionAim] %s viewpoint trace: count=%llu startOff=%u len=%.0f",
                    tag, static_cast<unsigned long long>(count), startOffset, length);
            }
        }
        return true;
    }
    return false;
}

void __fastcall WeaponAimSystem::HookedTraceActors(void* object, void* frame,
                                                   void* result) {
    auto& system = Instance();
    {
        static std::atomic<uint64_t> entries{0};
        const uint64_t entry = entries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (entry <= 3 || entry % 2000 == 0) {
            Log("[InteractionAim] TraceActors entry #%llu object=%p",
                static_cast<unsigned long long>(entry), object);
        }
    }
    uint32_t startOffset = 0;
    float length = 0.0f;
    TraceGateScan(frame, "TraceActors", startOffset, length, true);
    if (system.m_originalTraceActors)
        system.m_originalTraceActors(object, frame, result);
}

void __fastcall WeaponAimSystem::HookedFastTrace(void* object, void* frame,
                                                 void* result) {
    auto& system = Instance();
    {
        static std::atomic<uint64_t> entries{0};
        const uint64_t entry = entries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (entry <= 3 || entry % 2000 == 0) {
            Log("[InteractionAim] FastTrace entry #%llu object=%p",
                static_cast<unsigned long long>(entry), object);
        }
    }
    uint32_t startOffset = 0;
    float length = 0.0f;
    TraceGateScan(frame, "FastTrace", startOffset, length, true);
    if (system.m_originalFastTrace)
        system.m_originalFastTrace(object, frame, result);
}

void __fastcall WeaponAimSystem::HookedViewPoint(void* object, void* frame,
                                                 void* result) {
    auto& system = Instance();
    const uint64_t count = system.m_viewPointCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (count <= 40 || count % 500 == 0) {
        char callerName[128] = {};
        char callerOwner[128] = {};
        const auto globals = camera::GetUE3GlobalsSnapshot();
        TArray64 names = {};
        if (globals.gNamesValid &&
            ReadMem(globals.gNamesAddress, &names, sizeof(names))) {
            const uint64_t head = s_recentInteractionCallSequence.load(
                std::memory_order_acquire);
            for (uint64_t age = 1; age <= 16 && age < head; ++age) {
                const uint64_t expected = head - age;
                auto& call = s_recentInteractionCalls[
                    expected % kRecentInteractionCallCount];
                if (call.sequence.load(std::memory_order_acquire) != expected)
                    continue;
                const uintptr_t function = call.function.load(
                    std::memory_order_relaxed);
                if (function < 0x10000) continue;
                ReadObjectName(globals, names, function, callerName,
                               sizeof(callerName));
                uintptr_t owner = 0;
                if (ReadMem(function + 0x40, &owner, sizeof(owner)) &&
                    owner >= 0x10000)
                    ReadObjectName(globals, names, owner, callerOwner,
                                   sizeof(callerOwner));
                break;
            }
        }
        Log("[InteractionAim] GetPlayerViewPoint call #%llu object=%p caller=%s.%s",
            static_cast<unsigned long long>(count), object,
            callerOwner[0] ? callerOwner : "?", callerName[0] ? callerName : "?");
    }
    if (system.m_originalViewPoint)
        system.m_originalViewPoint(object, frame, result);
}

bool WeaponAimSystem::InstallScriptInvokeProbe(uintptr_t function,
                                                 uint64_t moduleBase,
                                                 uint32_t moduleSize) {
    uintptr_t target = 0;
    if (!ReadMem(function + 0xF0, &target, sizeof(target)) ||
        target < moduleBase || target >= moduleBase + moduleSize) {
        Log("[WeaponAim] GetAdjustedAim script invoke target is invalid: function=%p target=%p",
            reinterpret_cast<void*>(function), reinterpret_cast<void*>(target));
        return false;
    }
    if (m_scriptInvokeInstalled.load(std::memory_order_acquire)) {
        if (target != m_scriptInvokeTarget) {
            Log("[WeaponAim] GetAdjustedAim script invoke target changed: installed=%p current=%p",
                reinterpret_cast<void*>(m_scriptInvokeTarget),
                reinterpret_cast<void*>(target));
            return false;
        }
        return true;
    }

    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(reinterpret_cast<void*>(target), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT ||
        !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        Log("[WeaponAim] GetAdjustedAim script invoke target is not executable: %p protect=0x%X",
            reinterpret_cast<void*>(target), memory.Protect);
        return false;
    }

    unsigned char bytes[16] = {};
    if (!ReadMem(target, bytes, sizeof(bytes))) return false;
    Log("[WeaponAim] GetAdjustedAim script invoke target=%p RVA=0x%llX "
        "bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
        reinterpret_cast<void*>(target),
        static_cast<unsigned long long>(target - moduleBase),
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7]);

    const MH_STATUS createStatus = MH_CreateHook(reinterpret_cast<void*>(target),
        &HookedScriptInvoke, reinterpret_cast<void**>(&m_originalScriptInvoke));
    if (createStatus != MH_OK) {
        Log("[WeaponAim] Script invoke probe creation failed: %s",
            MH_StatusToString(createStatus));
        m_originalScriptInvoke = nullptr;
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enableStatus != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        Log("[WeaponAim] Script invoke probe enable failed: %s",
            MH_StatusToString(enableStatus));
        m_originalScriptInvoke = nullptr;
        return false;
    }

    m_scriptInvokeTarget = target;
    m_scriptInvokeInstalled.store(true, std::memory_order_release);
    Log("[WeaponAim] Guarded GetAdjustedAim script invoke hook installed");
    return true;
}

bool WeaponAimSystem::InstallInteractionAimHook(uintptr_t function,
                                                 uint64_t moduleBase,
                                                 uint32_t moduleSize) {
    if (m_interactionHookInstalled.load(std::memory_order_acquire)) return true;
    uintptr_t target = 0;
    uint16_t parameterSize = 0;
    if (!ResolveNativeExecTarget(function, moduleBase, moduleSize, target,
                                 parameterSize)) {
        Log("[InteractionAim] TickTargets native target unavailable: function=%p",
            reinterpret_cast<void*>(function));
        return false;
    }
    const MH_STATUS createStatus = MH_CreateHook(
        reinterpret_cast<void*>(target), &HookedTickTargets,
        reinterpret_cast<void**>(&m_originalTickTargets));
    if (createStatus != MH_OK) {
        Log("[InteractionAim] TickTargets hook creation failed: %s",
            MH_StatusToString(createStatus));
        m_originalTickTargets = nullptr;
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enableStatus != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        m_originalTickTargets = nullptr;
        Log("[InteractionAim] TickTargets hook enable failed: %s",
            MH_StatusToString(enableStatus));
        return false;
    }
    m_tickTargetsTarget = target;
    m_interactionHookInstalled.store(true, std::memory_order_release);
    Log("[InteractionAim] Native TickTargets hook installed: function=%p target=%p "
        "RVA=0x%llX params=%u",
        reinterpret_cast<void*>(function), reinterpret_cast<void*>(target),
        static_cast<unsigned long long>(target - moduleBase), parameterSize);
    return true;
}

void* __fastcall WeaponAimSystem::HookedUsableSelector(void* controller,
                                                       void* outRef) {
    auto& system = WeaponAimSystem::Instance();
    UsableSelectorFn original = system.m_originalUsableSelector;
    if (!original) return nullptr;
    if (!controller) return original(controller, outRef);

    const uintptr_t expectedController =
        system.m_localController.load(std::memory_order_acquire);
    const uint64_t aimUpdated =
        system.m_aimUpdatedMs.load(std::memory_order_acquire);
    const bool aimValid = system.m_aimValid.load(std::memory_order_acquire);
    const bool aimFresh =
        aimUpdated != 0 && GetTickCount64() - aimUpdated <= 500;
    const bool controllerMatches = expectedController &&
        reinterpret_cast<uintptr_t>(controller) == expectedController;

    static std::atomic<uint64_t> selectorCalls{0};
    const uint64_t callCount = selectorCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;

    // CalcViewLocation (+0xCA8, FVector) and CalcViewRotation (+0xCB4, FRotator
    // in Unis). The selector traces from exactly this pair, so swapping it for
    // the right-hand laser makes the interaction dot authoritative while the
    // real camera values are restored before anything else can read them.
    bool swapped = false;
    uintptr_t viewAddress = 0;
    uint8_t saved[24] = {};
    float origin[3] = {};
    if (controllerMatches && aimValid && aimFresh) {
        viewAddress = reinterpret_cast<uintptr_t>(controller) + 0xCA8;
        if (ReadMem(viewAddress, saved, sizeof(saved))) {
            origin[0] = system.m_aimOriginX.load(std::memory_order_relaxed);
            origin[1] = system.m_aimOriginY.load(std::memory_order_relaxed);
            origin[2] = system.m_aimOriginZ.load(std::memory_order_relaxed);
            const int32_t rotation[3] = {
                system.m_aimPitch.load(std::memory_order_relaxed),
                system.m_aimYaw.load(std::memory_order_relaxed),
                system.m_aimRoll.load(std::memory_order_relaxed)};
            uint8_t replacement[24] = {};
            memcpy(replacement, origin, sizeof(origin));
            memcpy(replacement + sizeof(origin), rotation, sizeof(rotation));
            if (memcmp(saved, replacement, sizeof(saved)) != 0 &&
                WriteMem(viewAddress, replacement, sizeof(replacement))) {
                swapped = true;
            }
        }
    }

    void* result = original(controller, outRef);

    if (swapped) {
        WriteMem(viewAddress, saved, sizeof(saved));
        static std::atomic<bool> loggedFirstOverride{false};
        if (!loggedFirstOverride.exchange(true, std::memory_order_relaxed)) {
            Log("[InteractionAim] Usable selector override active: controller=%p "
                "origin=(%.1f,%.1f,%.1f)",
                controller, origin[0], origin[1], origin[2]);
        }
        system.m_usableSelectorRedirects.fetch_add(1, std::memory_order_relaxed);
    }

    if (controllerMatches && aimValid && aimFresh) {
        const float targetX = system.m_aimTargetX.load(std::memory_order_relaxed);
        const float targetY = system.m_aimTargetY.load(std::memory_order_relaxed);
        const float targetZ = system.m_aimTargetZ.load(std::memory_order_relaxed);
        const float rayX = targetX - origin[0];
        const float rayY = targetY - origin[1];
        const float rayZ = targetZ - origin[2];
        const float rayLength = sqrtf(rayX * rayX + rayY * rayY + rayZ * rayZ);
        if (std::isfinite(rayLength) && rayLength > 1.0f) {
            const float direction[3] = {rayX / rayLength, rayY / rayLength,
                                        rayZ / rayLength};
            ApplyDotPickupSelection(reinterpret_cast<uintptr_t>(controller),
                                    origin, direction);
        }
    }

    if (callCount <= 8 || callCount % 600 == 0) {
        uintptr_t resultObject = 0;
        if (result)
            ReadMem(reinterpret_cast<uintptr_t>(result), &resultObject,
                    sizeof(resultObject));
        Log("[InteractionAim] Usable selector call=%llu ctl=%p match=%d aim=%d/%d "
            "swapped=%d result=%p caller=%p",
            static_cast<unsigned long long>(callCount), controller,
            controllerMatches ? 1 : 0, aimValid ? 1 : 0, aimFresh ? 1 : 0,
            swapped ? 1 : 0, reinterpret_cast<void*>(resultObject),
            _ReturnAddress());
    }
    return result;
}

bool WeaponAimSystem::InstallUsableSelectorHook(uintptr_t function,
                                                uint64_t moduleBase,
                                                uint32_t moduleSize) {
    (void)moduleSize;
    if (m_usableSelectorInstalled.load(std::memory_order_acquire)) return true;
    const MH_STATUS createStatus = MH_CreateHook(
        reinterpret_cast<void*>(function), &HookedUsableSelector,
        reinterpret_cast<void**>(&m_originalUsableSelector));
    if (createStatus != MH_OK) {
        Log("[InteractionAim] Usable selector hook creation failed: %s",
            MH_StatusToString(createStatus));
        m_originalUsableSelector = nullptr;
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(function));
    if (enableStatus != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(function));
        m_originalUsableSelector = nullptr;
        Log("[InteractionAim] Usable selector hook enable failed: %s",
            MH_StatusToString(enableStatus));
        return false;
    }
    m_usableSelectorTarget = function;
    m_usableSelectorInstalled.store(true, std::memory_order_release);
    Log("[InteractionAim] Usable selector hook installed: target=%p RVA=0x%llX",
        reinterpret_cast<void*>(function),
        static_cast<unsigned long long>(function - moduleBase));
    return true;
}

bool WeaponAimSystem::InstallTraceHook(uintptr_t function,
                                       uint64_t moduleBase,
                                       uint32_t moduleSize) {
    if (m_traceHookInstalled.load(std::memory_order_acquire)) return true;
    uintptr_t target = 0;
    uint16_t parameterSize = 0;
    if (!ResolveNativeExecTarget(function, moduleBase, moduleSize, target,
                                 parameterSize)) {
        Log("[InteractionAim] Trace native target unavailable: function=%p",
            reinterpret_cast<void*>(function));
        return false;
    }
    const MH_STATUS createStatus = MH_CreateHook(
        reinterpret_cast<void*>(target), &HookedTrace,
        reinterpret_cast<void**>(&m_originalTrace));
    if (createStatus != MH_OK) {
        Log("[InteractionAim] Trace hook creation failed: %s",
            MH_StatusToString(createStatus));
        m_originalTrace = nullptr;
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enableStatus != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        m_originalTrace = nullptr;
        Log("[InteractionAim] Trace hook enable failed: %s",
            MH_StatusToString(enableStatus));
        return false;
    }
    m_traceTarget = target;
    m_traceHookInstalled.store(true, std::memory_order_release);
    Log("[InteractionAim] Native Trace hook installed: function=%p target=%p "
        "RVA=0x%llX params=%u",
        reinterpret_cast<void*>(function), reinterpret_cast<void*>(target),
        static_cast<unsigned long long>(target - moduleBase), parameterSize);
    return true;
}

void __fastcall WeaponAimSystem::HookedTrace(void* object, void* frame,
                                             void* result) {
    auto& system = Instance();
    {
        const uint64_t entry = system.m_traceProbeHits.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (entry <= 3 || entry % 2000 == 0) {
            Log("[InteractionAim] Trace entry #%llu object=%p frame=%p",
                static_cast<unsigned long long>(entry), object, frame);
        }
    }
    // Steer viewpoint-originated traces (interaction focus, crosshair) onto
    // the hand ray. Self-gating: Start must sit at the live camera position
    // and End must look like a viewpoint-trace endpoint; a wrong params
    // layout simply never matches, keeping this a safe no-op.
    do {
        if (!frame) break;
        uintptr_t locals = 0;
        if (!ReadMem(reinterpret_cast<uintptr_t>(frame) + 0x18,
                     &locals, sizeof(locals)) || locals < 0x10000) break;
        float aimOrigin[3] = {};
        float aimDir[3] = {};
        bool aimFresh = false;
        AcquireSRWLockShared(&system.m_ballisticOverrideLock);
        const uint64_t updatedMs = system.m_aimUpdatedMs.load(std::memory_order_acquire);
        const uint64_t nowMs = GetTickCount64();
        aimFresh = system.m_aimValid.load(std::memory_order_acquire) &&
            updatedMs != 0 && nowMs >= updatedMs && nowMs - updatedMs <= 100;
        if (aimFresh) {
            constexpr float kUnisToRadians = 0.00009587379924285257f;
            const float pitch = static_cast<float>(
                system.m_aimPitch.load(std::memory_order_relaxed)) * kUnisToRadians;
            const float yaw = static_cast<float>(
                system.m_aimYaw.load(std::memory_order_relaxed)) * kUnisToRadians;
            aimOrigin[0] = system.m_aimOriginX.load(std::memory_order_relaxed);
            aimOrigin[1] = system.m_aimOriginY.load(std::memory_order_relaxed);
            aimOrigin[2] = system.m_aimOriginZ.load(std::memory_order_relaxed);
            const float cosPitch = cosf(pitch);
            aimDir[0] = cosPitch * cosf(yaw);
            aimDir[1] = cosPitch * sinf(yaw);
            aimDir[2] = sinf(pitch);
        }
        ReleaseSRWLockShared(&system.m_ballisticOverrideLock);
        if (!aimFresh) break;
        const float* cameraLocation = camera::GetCameraLocation();
        float camLoc[3] = {};
        if (!cameraLocation ||
            !ReadDirect(reinterpret_cast<uintptr_t>(cameraLocation),
                        camLoc, sizeof(camLoc))) break;
        // Trace params order: HitLocation, HitNormal, HitInfo, End, Start.
        // HitInfo size is unknown, so probe candidate Start offsets; End sits
        // 12 bytes before Start.
        static const uint32_t kStartCandidates[] = {52, 56, 60, 64, 68, 72, 76, 80, 84, 88};
        for (uint32_t startOffset : kStartCandidates) {
            const uint32_t endOffset = startOffset - 12;
            float traceStart[3] = {};
            float traceEnd[3] = {};
            if (!ReadDirect(locals + startOffset, traceStart, sizeof(traceStart)) ||
                !ReadDirect(locals + endOffset, traceEnd, sizeof(traceEnd))) continue;
            const float sdx = traceStart[0] - camLoc[0];
            const float sdy = traceStart[1] - camLoc[1];
            const float sdz = traceStart[2] - camLoc[2];
            if (sdx * sdx + sdy * sdy + sdz * sdz > 50.0f * 50.0f) continue;
            const float edx = traceEnd[0] - traceStart[0];
            const float edy = traceEnd[1] - traceStart[1];
            const float edz = traceEnd[2] - traceStart[2];
            const float length = sqrtf(edx * edx + edy * edy + edz * edz);
            if (!std::isfinite(length) || length < 50.0f || length > 12000.0f) continue;
            float newEnd[3] = {
                aimOrigin[0] + aimDir[0] * length,
                aimOrigin[1] + aimDir[1] * length,
                aimOrigin[2] + aimDir[2] * length
            };
            const bool startWritten = WriteDirect(locals + startOffset, aimOrigin,
                                                  sizeof(aimOrigin));
            const bool endWritten = startWritten && WriteDirect(locals + endOffset,
                                                                newEnd, sizeof(newEnd));
            const uint64_t count = system.m_traceRedirectCount.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (count <= 10 || count % 600 == 0) {
                Log("[InteractionAim] Trace redirected to hand ray: count=%llu "
                    "written=%d startOff=%u len=%.0f",
                    static_cast<unsigned long long>(count),
                    startWritten && endWritten, startOffset, length);
            }
            break;
        }
    } while (false);

    if (system.m_originalTrace)
        system.m_originalTrace(object, frame, result);
}

void __fastcall WeaponAimSystem::HookedTickTargets(void* object, void* frame,
                                                    void* result) {
    auto& system = Instance();
    {
        static std::atomic<uint64_t> nativeTickHits{0};
        const uint64_t nativeHit = nativeTickHits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (nativeHit <= 5 || nativeHit % 600 == 0) {
            Log("[InteractionAim] Native TickTargets entry hit=%llu object=%p controller=%p",
                static_cast<unsigned long long>(nativeHit), object,
                reinterpret_cast<void*>(system.m_localController.load(std::memory_order_acquire)));
        }
    }
    float* cameraLocation = nullptr;
    int32_t* cameraRotation = nullptr;
    float savedLocation[3] = {};
    int32_t savedRotation[3] = {};
    float aimOrigin[3] = {};
    int32_t aimRotation[3] = {};
    bool redirected = false;
    bool locationWritten = false;
    bool rotationWritten = false;

    const uintptr_t localController = system.m_localController.load(
        std::memory_order_acquire);
    bool aimFresh = false;
    if (localController >= 0x10000 &&
        reinterpret_cast<uintptr_t>(object) == localController) {
        AcquireSRWLockShared(&system.m_ballisticOverrideLock);
        const uint64_t updatedMs = system.m_aimUpdatedMs.load(std::memory_order_acquire);
        const uint64_t nowMs = GetTickCount64();
        aimFresh = system.m_aimValid.load(std::memory_order_acquire) &&
            updatedMs != 0 && nowMs >= updatedMs && nowMs - updatedMs <= 100;
        if (aimFresh) {
            aimOrigin[0] = system.m_aimOriginX.load(std::memory_order_relaxed);
            aimOrigin[1] = system.m_aimOriginY.load(std::memory_order_relaxed);
            aimOrigin[2] = system.m_aimOriginZ.load(std::memory_order_relaxed);
            aimRotation[0] = system.m_aimPitch.load(std::memory_order_relaxed);
            aimRotation[1] = system.m_aimYaw.load(std::memory_order_relaxed);
            aimRotation[2] = system.m_aimRoll.load(std::memory_order_relaxed);
        }
        ReleaseSRWLockShared(&system.m_ballisticOverrideLock);

        cameraLocation = camera::GetCameraLocation();
        cameraRotation = camera::GetCameraRotation();
        if (aimFresh && cameraLocation && cameraRotation &&
            ReadDirect(reinterpret_cast<uintptr_t>(cameraLocation), savedLocation,
                       sizeof(savedLocation)) &&
            ReadDirect(reinterpret_cast<uintptr_t>(cameraRotation), savedRotation,
                       sizeof(savedRotation))) {
            locationWritten = WriteDirect(
                reinterpret_cast<uintptr_t>(cameraLocation), aimOrigin,
                sizeof(aimOrigin));
            rotationWritten = locationWritten && WriteDirect(
                reinterpret_cast<uintptr_t>(cameraRotation), aimRotation,
                sizeof(aimRotation));
            redirected = locationWritten && rotationWritten;
            if (!redirected) {
                if (locationWritten) WriteDirect(
                    reinterpret_cast<uintptr_t>(cameraLocation), savedLocation,
                    sizeof(savedLocation));
                if (rotationWritten) WriteDirect(
                    reinterpret_cast<uintptr_t>(cameraRotation), savedRotation,
                    sizeof(savedRotation));
            }
        }
    }

    if (system.m_originalTickTargets)
        system.m_originalTickTargets(object, frame, result);

    if (redirected) {
        const bool locationRestored = WriteDirect(
            reinterpret_cast<uintptr_t>(cameraLocation), savedLocation,
            sizeof(savedLocation));
        const bool rotationRestored = WriteDirect(
            reinterpret_cast<uintptr_t>(cameraRotation), savedRotation,
            sizeof(savedRotation));
        const uint64_t count = system.m_interactionRedirectCount.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (count <= 5 || count % 600 == 0) {
            Log("[InteractionAim] Native TickTargets redirected to dot ray: "
                "count=%llu restored=%d aimOrigin=(%.1f,%.1f,%.1f) "
                "aimRot=(%d,%d,%d)",
                static_cast<unsigned long long>(count),
                locationRestored && rotationRestored,
                aimOrigin[0], aimOrigin[1], aimOrigin[2],
                aimRotation[0], aimRotation[1], aimRotation[2]);
        }
    }

    if (aimFresh && localController >= 0x10000 &&
        reinterpret_cast<uintptr_t>(object) == localController) {
        const float targetX = system.m_aimTargetX.load(std::memory_order_relaxed);
        const float targetY = system.m_aimTargetY.load(std::memory_order_relaxed);
        const float targetZ = system.m_aimTargetZ.load(std::memory_order_relaxed);
        const float rayX = targetX - aimOrigin[0];
        const float rayY = targetY - aimOrigin[1];
        const float rayZ = targetZ - aimOrigin[2];
        const float rayLength = sqrtf(rayX * rayX + rayY * rayY + rayZ * rayZ);
        if (std::isfinite(rayLength) && rayLength > 1.0f) {
            const float direction[3] = {rayX / rayLength, rayY / rayLength,
                                        rayZ / rayLength};
            ApplyDotPickupSelection(localController, aimOrigin, direction);
        }
    }
}

void __fastcall WeaponAimSystem::HookedGetAimRotation(void* object, void* frame,
                                                       void* result) {
    auto& system = Instance();
    if (system.m_originalGetAimRotation)
        system.m_originalGetAimRotation(object, frame, result);

    const uint64_t count = system.m_nativeAimCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    const bool firing = system.m_fireActive.load(std::memory_order_acquire);
    static std::atomic<bool> lastNativeFiring{false};
    const bool firingEdge = firing != lastNativeFiring.exchange(
        firing, std::memory_order_acq_rel);
    const bool periodicSample = firing ? (count <= 16 || count % 120 == 0) :
        (count <= 8 || count % 600 == 0);
    if (!result || (!periodicSample && !firingEdge)) return;

    __try {
        const int32_t* rotation = reinterpret_cast<const int32_t*>(result);
        Log("[WeaponAim] Native GetAimRotation call=%llu firing=%d object=%p "
            "frame=%p result=%p rot=(%d,%d,%d)",
            static_cast<unsigned long long>(count), firing, object, frame, result,
            rotation[0], rotation[1], rotation[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[WeaponAim] Native GetAimRotation result unreadable: call=%llu firing=%d result=%p",
            static_cast<unsigned long long>(count), firing, result);
    }
}

void __fastcall WeaponAimSystem::HookedScriptInvoke(void* object, void* frame,
                                                     void* result) {
    auto& system = Instance();
    uintptr_t function = 0;
    uintptr_t locals = 0;
    if (frame) {
        // BL1E keeps UE3's pack(4) layout even in x64.
        ReadDirect(reinterpret_cast<uintptr_t>(frame) + 0x14,
                   &function, sizeof(function));
    }
    RecordInteractionCall(object, reinterpret_cast<void*>(function),
                          InteractionCallSource::ProcessInternal);
    const uintptr_t localController = system.m_localController.load(
        std::memory_order_acquire);
    const bool monitorFocus = localController >= 0x10000 &&
        system.m_pawnIdentityValid.load(std::memory_order_acquire);
    uintptr_t usableBefore = 0;
    if (monitorFocus)
        ReadDirect(localController + 0xE84, &usableBefore, sizeof(usableBefore));
    const bool isLocalAdjustedAim =
        function != 0 &&
        function == system.m_getAdjustedAimFunction.load(std::memory_order_acquire) &&
        system.m_weaponIdentityValid.load(std::memory_order_acquire) &&
        (reinterpret_cast<uintptr_t>(object) ==
            system.m_localWeapon.load(std::memory_order_acquire) ||
         system.m_vehicleSecondaryFireActive.load(std::memory_order_acquire));
    const uint64_t identityGeneration = system.m_identityGeneration.load(
        std::memory_order_acquire);
    if (isLocalAdjustedAim) {
        ReadDirect(reinterpret_cast<uintptr_t>(frame) + 0x2C,
                   &locals, sizeof(locals));
    }

    if (system.m_originalScriptInvoke)
        system.m_originalScriptInvoke(object, frame, result);

    if (monitorFocus) {
        uintptr_t usableAfter = 0;
        if (ReadDirect(localController + 0xE84, &usableAfter, sizeof(usableAfter)) &&
            usableAfter != usableBefore) {
            LogInteractionFocusWriter("ProcessInternal",
                reinterpret_cast<uintptr_t>(object), function,
                usableBefore, usableAfter);
        }
    }

    const bool identityStillValid = isLocalAdjustedAim &&
        identityGeneration == system.m_identityGeneration.load(std::memory_order_acquire) &&
        function == system.m_getAdjustedAimFunction.load(std::memory_order_acquire) &&
        system.m_weaponIdentityValid.load(std::memory_order_acquire) &&
        (reinterpret_cast<uintptr_t>(object) ==
            system.m_localWeapon.load(std::memory_order_acquire) ||
         system.m_vehicleSecondaryFireActive.load(std::memory_order_acquire));
    if (!identityStillValid) return;
    const uint64_t count = system.m_scriptInvokeAimCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    const bool firing = system.m_fireActive.load(std::memory_order_acquire);
    const bool periodicSample = firing ? (count <= 8 || count % 120 == 0) :
        (count <= 8 || count % 600 == 0);
    int32_t rotation[3] = {};
    float origin[3] = {};
    const bool originReadable = locals && ReadDirect(locals, origin, sizeof(origin));
    int32_t desired[3] = {};
    bool finiteTargetUsed = false;
    bool overrideEnabled = false;
    bool resultReadable = false;
    bool written = false;

    AcquireSRWLockShared(&system.m_ballisticOverrideLock);
    const uint64_t aimUpdatedMs = system.m_aimUpdatedMs.load(std::memory_order_acquire);
    const uint64_t nowMs = GetTickCount64();
    const bool desiredValid = system.m_aimValid.load(std::memory_order_acquire) &&
        aimUpdatedMs != 0 && nowMs >= aimUpdatedMs && nowMs - aimUpdatedMs <= 100;
    desired[0] = system.m_aimPitch.load(std::memory_order_relaxed);
    desired[1] = system.m_aimYaw.load(std::memory_order_relaxed);
    desired[2] = system.m_aimRoll.load(std::memory_order_relaxed);
    if (desiredValid && originReadable &&
        system.m_aimTargetValid.load(std::memory_order_acquire)) {
        const float aimOrigin[3] = {
            system.m_aimOriginX.load(std::memory_order_relaxed),
            system.m_aimOriginY.load(std::memory_order_relaxed),
            system.m_aimOriginZ.load(std::memory_order_relaxed)
        };
        const float target[3] = {
            system.m_aimTargetX.load(std::memory_order_relaxed),
            system.m_aimTargetY.load(std::memory_order_relaxed),
            system.m_aimTargetZ.load(std::memory_order_relaxed)
        };
        const float originDx = origin[0] - aimOrigin[0];
        const float originDy = origin[1] - aimOrigin[1];
        const float originDz = origin[2] - aimOrigin[2];
        constexpr float kMaxFireOriginDistanceUe = 1000.0f;
        if (std::isfinite(originDx) && std::isfinite(originDy) &&
            std::isfinite(originDz) &&
            originDx * originDx + originDy * originDy + originDz * originDz <=
                kMaxFireOriginDistanceUe * kMaxFireOriginDistanceUe) {
            const float finiteDirection[3] = {
                target[0] - origin[0],
                target[1] - origin[1],
                target[2] - origin[2]
            };
            finiteTargetUsed = DirectionToRotator(finiteDirection, desired);
        }
    }
    overrideEnabled = system.m_ballisticOverrideEnabled.load(std::memory_order_acquire);
    resultReadable = result && (periodicSample || overrideEnabled) &&
        ReadDirect(reinterpret_cast<uintptr_t>(result), rotation, sizeof(rotation));
    static std::atomic<bool> lastScriptFiring{false};
    if (firing != lastScriptFiring.exchange(firing, std::memory_order_acq_rel)) {
        Log("[WeaponAim] Fire %s at GetAdjustedAim: call=%llu aimValid=%d "
            "desiredValid=%d originReadable=%d resultReadable=%d overrideEnabled=%d",
            firing ? "started" : "ended",
            static_cast<unsigned long long>(count),
            system.m_aimValid.load(std::memory_order_acquire), desiredValid,
            originReadable, resultReadable, overrideEnabled);
    }
    if (overrideEnabled && firing && desiredValid && resultReadable) {
        written = WriteDirect(reinterpret_cast<uintptr_t>(result), desired, sizeof(desired));
    }
    ReleaseSRWLockShared(&system.m_ballisticOverrideLock);
    if (written) {
        const uint64_t writeCount = system.m_overrideCount.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (writeCount <= 5 || writeCount % 600 == 0) {
            Log("[WeaponAim] Ballistic override write count=%llu rot=(%d,%d,%d)",
                static_cast<unsigned long long>(writeCount), desired[0], desired[1],
                desired[2]);
        }
    } else if (firing && !desiredValid) {
        static std::atomic<uint64_t> nextStaleLogMs{0};
        uint64_t next = nextStaleLogMs.load(std::memory_order_relaxed);
        if (nowMs >= next && nextStaleLogMs.compare_exchange_strong(
                next, nowMs + 1000, std::memory_order_relaxed)) {
            const uint64_t age = aimUpdatedMs && nowMs >= aimUpdatedMs
                ? nowMs - aimUpdatedMs : 0;
            Log("[WeaponAim] Override skipped while firing: aimValid=%d age=%llums "
                "originReadable=%d resultReadable=%d",
                system.m_aimValid.load(std::memory_order_acquire),
                static_cast<unsigned long long>(age), originReadable,
                resultReadable);
        }
    }
    if (!periodicSample) return;

    Log("[WeaponAim] Script GetAdjustedAim probe call=%llu firing=%d object=%p "
        "function=%p frame=%p locals=%p originReadable=%d origin=(%.3f,%.3f,%.3f) "
        "result=%p resultReadable=%d rot=(%d,%d,%d) desiredValid=%d "
        "desired=(%d,%d,%d) finiteTarget=%d overrideEnabled=%d write=%d",
        static_cast<unsigned long long>(count), firing, object,
        reinterpret_cast<void*>(function), frame, reinterpret_cast<void*>(locals),
        originReadable, origin[0], origin[1], origin[2], result, resultReadable,
        rotation[0], rotation[1], rotation[2], desiredValid,
        desired[0], desired[1], desired[2], finiteTargetUsed, overrideEnabled, written);
}

void __fastcall WeaponAimSystem::HookedIsInjured(
        void* object, void* frame, void* result) {
    auto& system = Instance();
    if (system.m_originalIsInjured)
        system.m_originalIsInjured(object, frame, result);
    if (reinterpret_cast<uintptr_t>(object) !=
        system.m_localPawn.load(std::memory_order_acquire)) return;
    uint32_t rawResult = 0;
    if (!result || !ReadDirect(reinterpret_cast<uintptr_t>(result),
                               &rawResult, sizeof(rawResult))) return;
    const bool injured = rawResult != 0;
    const bool previous = system.m_playerInjured.exchange(
        injured, std::memory_order_acq_rel);
    if (injured != previous)
        Log("[WeaponAim] Native injured state %s", injured ? "started" : "ended");
}

void __fastcall WeaponAimSystem::HookedPhaseWalkVisibility(
        void* object, void* frame, void* result) {
    auto& system = Instance();
    if (system.m_originalPhaseWalkVisibility)
        system.m_originalPhaseWalkVisibility(object, frame, result);
    if (reinterpret_cast<uintptr_t>(object) !=
        system.m_localPawn.load(std::memory_order_acquire)) return;
    uint32_t rawResult = 0;
    const bool resultReadable = result && ReadDirect(
        reinterpret_cast<uintptr_t>(result), &rawResult, sizeof(rawResult));
    const bool missingWeapon =
        !system.m_weaponIdentityValid.load(std::memory_order_acquire);
    if (missingWeapon) {
        const bool previous = system.m_phaseWalkActive.exchange(
            true, std::memory_order_acq_rel);
        if (!previous)
            Log("[WeaponAim] Native Phasewalk state started "
                "(resultReadable=%d result=%u weaponMissing=%d)",
                resultReadable, rawResult, missingWeapon);
    }
}

void __fastcall WeaponAimSystem::HookedMeleeAttack(
        void* object, void* frame, void* result) {
    auto& system = Instance();
    InFlightMeleeHook inFlight(system.m_inFlightMeleeHooks);
    NativeExecFn original = system.m_originalMeleeAttack;
    if (!original) return;
    if (system.m_meleeHooksStopping.load(std::memory_order_acquire) ||
        s_meleeAttackDepth != 0) {
        original(object, frame, result);
        return;
    }

    PlayerIdentitySnapshot identity = system.GetPlayerIdentity();
    uintptr_t frameNode = 0;
    uintptr_t locals = 0;
    uintptr_t context = 0;
    uintptr_t objectClass = 0;
    const bool localPlayerAttack = object && frame && identity.pawnValid &&
        ReadDirect(reinterpret_cast<uintptr_t>(frame) + 0x14,
                   &frameNode, sizeof(frameNode)) &&
        frameNode == system.m_meleeAttackFunction &&
        ReadDirect(reinterpret_cast<uintptr_t>(frame) + 0x2C,
                   &locals, sizeof(locals)) && locals >= 0x10000 &&
        ReadDirect(locals + static_cast<uintptr_t>(system.m_meleeContextParameterOffset),
                   &context, sizeof(context)) && context == identity.pawn &&
        ReadDirect(reinterpret_cast<uintptr_t>(object) +
                       static_cast<uintptr_t>(system.m_meleeObjectClassOffset),
                   &objectClass, sizeof(objectClass)) &&
        ClassDistance(objectClass, system.m_meleeDefinitionClass) >= 0;
    const float multiplier = config::GetMeleeRangeMultiplier();
    if (!localPlayerAttack ||
        !system.m_meleeMutationSafe.load(std::memory_order_acquire) ||
        !std::isfinite(multiplier) || multiplier <= 1.0001f) {
        original(object, frame, result);
        return;
    }

    const DWORD currentThread = GetCurrentThreadId();
    DWORD expectedThread = 0;
    system.m_meleeExecutionThread.compare_exchange_strong(
        expectedThread, currentThread, std::memory_order_acq_rel);
    if (expectedThread != 0 && expectedThread != currentThread) {
        original(object, frame, result);
        return;
    }
    if (!TryAcquireSRWLockExclusive(&system.m_meleeRangeLock)) {
        original(object, frame, result);
        return;
    }
    const bool identityStillValid =
        system.m_identityGeneration.load(std::memory_order_acquire) == identity.generation &&
        system.m_pawnIdentityValid.load(std::memory_order_acquire) &&
        system.m_localPawn.load(std::memory_order_acquire) == identity.pawn;
    if (!identityStillValid) {
        ReleaseSRWLockExclusive(&system.m_meleeRangeLock);
        original(object, frame, result);
        return;
    }

    ++s_meleeAttackDepth;
    const uintptr_t traceAddress = reinterpret_cast<uintptr_t>(object) +
        static_cast<uintptr_t>(system.m_traceScaleOffset);
    const uintptr_t radiusAddress = reinterpret_cast<uintptr_t>(object) +
        static_cast<uintptr_t>(system.m_radiusScaleOffset);
    float traceScale = 0.0f;
    float radiusScale = 0.0f;
    const bool valuesValid = ReadDirect(traceAddress, &traceScale, sizeof(traceScale)) &&
        ReadDirect(radiusAddress, &radiusScale, sizeof(radiusScale)) &&
        std::isfinite(traceScale) && std::isfinite(radiusScale) &&
        fabsf(traceScale) <= 1000000.0f && fabsf(radiusScale) <= 1000000.0f;
    const float scaledTrace = traceScale * multiplier;
    const float scaledRadius = radiusScale * multiplier;
    bool traceWritten = false;
    bool radiusWritten = false;
    if (valuesValid && std::isfinite(scaledTrace) && std::isfinite(scaledRadius)) {
        traceWritten = WriteDirect(traceAddress, &scaledTrace, sizeof(scaledTrace));
        radiusWritten = traceWritten &&
            WriteDirect(radiusAddress, &scaledRadius, sizeof(scaledRadius));
    }
    if (!traceWritten || !radiusWritten) {
        const bool traceRolledBack = !traceWritten ||
            WriteDirect(traceAddress, &traceScale, sizeof(traceScale));
        const bool radiusRolledBack = !radiusWritten ||
            WriteDirect(radiusAddress, &radiusScale, sizeof(radiusScale));
        system.m_meleeMutationSafe.store(false, std::memory_order_release);
        --s_meleeAttackDepth;
        ReleaseSRWLockExclusive(&system.m_meleeRangeLock);
        Log("[MeleeRange] ERROR: formula mutation rejected; scaling disabled "
            "values=%d traceWrite=%d radiusWrite=%d traceRollback=%d radiusRollback=%d",
            valuesValid, traceWritten, radiusWritten, traceRolledBack, radiusRolledBack);
        original(object, frame, result);
        return;
    }

    original(object, frame, result);

    const bool traceRestored = WriteDirect(traceAddress, &traceScale, sizeof(traceScale));
    const bool radiusRestored = WriteDirect(radiusAddress, &radiusScale, sizeof(radiusScale));
    const bool restored = traceRestored && radiusRestored;
    if (!restored)
        system.m_meleeMutationSafe.store(false, std::memory_order_release);
    const uint64_t count = system.m_meleeRangeApplies.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (count <= 8 || count % 100 == 0) {
        Log("[MeleeRange] Attack %llu object=%p context=%p multiplier=%.2f "
            "traceScale=%.4f->%.4f radiusScale=%.4f->%.4f restored=%d",
            static_cast<unsigned long long>(count), object,
            reinterpret_cast<void*>(context), multiplier,
            traceScale, scaledTrace, radiusScale, scaledRadius, restored);
    }
    --s_meleeAttackDepth;
    ReleaseSRWLockExclusive(&system.m_meleeRangeLock);
    if (!restored)
        Log("[MeleeRange] ERROR: failed to restore melee definition %p", object);
}

void __fastcall WeaponAimSystem::HookedFindMeleeTarget(
        void* object, void* frame, void* result) {
    auto& system = Instance();
    InFlightMeleeHook inFlight(system.m_inFlightMeleeHooks);
    NativeExecFn original = system.m_originalFindMeleeTarget;
    if (!original) return;
    const PlayerIdentitySnapshot identity = system.GetPlayerIdentity();
    uintptr_t frameNode = 0;
    const float multiplier = config::GetMeleeRangeMultiplier();
    if (system.m_meleeHooksStopping.load(std::memory_order_acquire) ||
        !identity.pawnValid || reinterpret_cast<uintptr_t>(object) != identity.pawn ||
        !frame || !ReadDirect(reinterpret_cast<uintptr_t>(frame) + 0x14,
                              &frameNode, sizeof(frameNode)) ||
        frameNode != system.m_findMeleeTargetFunction ||
        !system.m_meleeMutationSafe.load(std::memory_order_acquire) ||
        !std::isfinite(multiplier) || multiplier <= 1.0001f) {
        original(object, frame, result);
        return;
    }

    uintptr_t locals = 0;
    float distance = 0.0f;
    const bool valueValid = ReadDirect(reinterpret_cast<uintptr_t>(frame) + 0x2C,
                                       &locals, sizeof(locals)) &&
        locals >= 0x10000 &&
        ReadDirect(locals + static_cast<uintptr_t>(
                       system.m_maxLungeDistanceParameterOffset),
                   &distance, sizeof(distance)) &&
        std::isfinite(distance) && distance > 0.0f && distance <= 1000000.0f;
    const float scaledDistance = distance * multiplier;
    const uintptr_t distanceAddress = locals + static_cast<uintptr_t>(
        system.m_maxLungeDistanceParameterOffset);
    const bool identityStillValid =
        system.m_identityGeneration.load(std::memory_order_acquire) == identity.generation &&
        system.m_pawnIdentityValid.load(std::memory_order_acquire) &&
        system.m_localPawn.load(std::memory_order_acquire) == identity.pawn;
    const bool written = valueValid && identityStillValid &&
        std::isfinite(scaledDistance) &&
        WriteDirect(distanceAddress, &scaledDistance, sizeof(scaledDistance));
    original(object, frame, result);
    const bool restored = !written ||
        WriteDirect(distanceAddress, &distance, sizeof(distance));
    if (written && !restored)
        system.m_meleeMutationSafe.store(false, std::memory_order_release);
    if (written) {
        const uint64_t count = system.m_lungeRangeApplies.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (count <= 8 || count % 100 == 0) {
            Log("[MeleeRange] Lunge query %llu pawn=%p multiplier=%.2f "
                "distance=%.3f->%.3f restored=%d",
                static_cast<unsigned long long>(count), object, multiplier,
                distance, scaledDistance, restored);
        }
    }
    if (!restored)
        Log("[MeleeRange] ERROR: failed to restore MaxLungeDistance for pawn %p", object);
}

void WeaponAimSystem::Discover(const void* globalsAddress, uint64_t controllerAddress,
                                 uint64_t moduleBase, uint32_t moduleSize) {
    m_runtimeModuleBase.store(moduleBase, std::memory_order_release);
    m_runtimeModuleSize.store(moduleSize, std::memory_order_release);
    const auto& globals = *static_cast<const camera::UE3Globals*>(globalsAddress);
    if (!globals.gNamesValid || !globals.gObjectsValid || controllerAddress < 0x10000)
        return;

    TArray64 names = {};
    TArray64 objects = {};
    if (!ReadMem(globals.gNamesAddress, &names, sizeof(names)) ||
        !ReadMem(globals.gObjectsAddress, &objects, sizeof(objects)))
        return;

    AimFunctionCandidate aimCandidates[128] = {};
    size_t aimCandidateCount = 0;
    size_t totalAimCandidateCount = 0;
    uintptr_t getAdjustedAim = 0;
    uint64_t aimNameToken = 0;
    uint64_t tickTargetsNameToken = 0;
    uintptr_t tickTargetsFunction = 0;
    uintptr_t tickTargetsOwner = 0;
    uintptr_t traceFunction = 0;
    uintptr_t traceActorsFunction = 0;
    uintptr_t playerTickFunctions[8] = {};
    size_t playerTickFunctionCount = 0;
    uintptr_t allowUseFunctions[8] = {};
    size_t allowUseFunctionCount = 0;
    uintptr_t fastTraceFunction = 0;
    uintptr_t viewPointFunction = 0;
    uintptr_t allowUseEventFunction = 0;
    uint64_t isInjuredNameToken = 0;
    uint64_t phaseWalkVisibilityNameToken = 0;
    uintptr_t isInjuredFunction = 0;
    uintptr_t phaseWalkVisibilityFunction = 0;

    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t object = 0;
        char objectName[128] = {};
        char className[128] = {};
        if (!ReadMem(objects.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                     &object, sizeof(object)) || !object ||
            !ReadObjectName(globals, names, object, objectName, sizeof(objectName)) ||
            !ReadClassName(globals, names, object, className, sizeof(className))) continue;

        if (strcmp(objectName, "GetAdjustedAim") == 0 &&
            strcmp(className, "Function") == 0) {
            uint64_t nameToken = 0;
            uintptr_t owner = 0;
            if (ReadMem(object + 0x48, &nameToken, sizeof(nameToken)) && nameToken &&
                ReadOuter(globals, object, owner) && owner >= 0x10000) {
                ++totalAimCandidateCount;
                if (aimCandidateCount < _countof(aimCandidates)) {
                    aimCandidates[aimCandidateCount++] = {object, owner, nameToken};
                }
            }
        }
        if (strcmp(objectName, "TickTargets") == 0 &&
            strcmp(className, "Function") == 0) {
            uint64_t nameToken = 0;
            uintptr_t owner = 0;
            if (ReadMem(object + 0x48, &nameToken, sizeof(nameToken)) && nameToken &&
                ReadOuter(globals, object, owner) && owner >= 0x10000) {
                tickTargetsNameToken = nameToken;
                tickTargetsFunction = object;
                tickTargetsOwner = owner;
            }
        }
        if (strcmp(objectName, "Trace") == 0 &&
            strcmp(className, "Function") == 0 && traceFunction == 0) {
            // Interaction focus runs through native Actor.Trace; owner must
            // be the Actor class itself (not an override).
            uintptr_t owner = 0;
            char ownerName[128] = {};
            if (ReadOuter(globals, object, owner) && owner >= 0x10000 &&
                ReadObjectName(globals, names, owner, ownerName, sizeof(ownerName)) &&
                strcmp(ownerName, "Actor") == 0) {
                traceFunction = object;
            }
        }
        if (strcmp(className, "Function") == 0) {
            uintptr_t owner = 0;
            char ownerName[128] = {};
            const bool haveOwner = ReadOuter(globals, object, owner) &&
                owner >= 0x10000 &&
                ReadObjectName(globals, names, owner, ownerName, sizeof(ownerName));
            if (haveOwner && strcmp(ownerName, "Actor") == 0) {
                if (strcmp(objectName, "TraceActors") == 0 && traceActorsFunction == 0)
                    traceActorsFunction = object;
                if (strcmp(objectName, "FastTrace") == 0 && fastTraceFunction == 0)
                    fastTraceFunction = object;
            }
            if (haveOwner && strcmp(ownerName, "Controller") == 0 &&
                strcmp(objectName, "GetPlayerViewPoint") == 0 && viewPointFunction == 0)
                viewPointFunction = object;
            if ((strcmp(ownerName, "InteractionProxy") == 0 ||
                 strcmp(ownerName, "PawnInteractionProxy") == 0) &&
                strcmp(objectName, "AllowUseEvent") == 0 && allowUseEventFunction == 0)
                allowUseEventFunction = object;
        }
        // FFrame Node validation sets: every PlayerTick/AllowUseEvent
        // UFunction, so the hook can match Node pointers without names.
        if (strcmp(className, "Function") == 0) {
            if (strcmp(objectName, "PlayerTick") == 0 &&
                playerTickFunctionCount < 8) {
                playerTickFunctions[playerTickFunctionCount++] = object;
            }
            if (strcmp(objectName, "AllowUseEvent") == 0 &&
                allowUseFunctionCount < 8) {
                allowUseFunctions[allowUseFunctionCount++] = object;
            }
        }
        if (strcmp(className, "Function") == 0 &&
            (strcmp(objectName, "IsInjured") == 0 ||
             strcmp(objectName, "ShouldLocalPlayerSeeMePhaseWalk") == 0)) {
            uint64_t nameToken = 0;
            if (!ReadMem(object + 0x48, &nameToken, sizeof(nameToken)) || !nameToken)
                continue;
            if (strcmp(objectName, "IsInjured") == 0) {
                isInjuredNameToken = nameToken;
                isInjuredFunction = object;
            } else {
                phaseWalkVisibilityNameToken = nameToken;
                phaseWalkVisibilityFunction = object;
            }
        }
    }

    uint32_t aimFlags = 0;
    uint32_t aimPropertySize = 0;
    int32_t aimScriptSize = 0;
    uint16_t aimParameterSize = 0;

    uintptr_t controllerClass = 0;
    uintptr_t pawn = 0;
    uintptr_t pawnClass = 0;
    uintptr_t weapon = 0;
    uintptr_t weaponClass = 0;
    bool pawnValid = false;
    bool weaponValid = false;
    int32_t pawnOffset = -1;
    int32_t controllerOffset = -1;
    int32_t weaponOffset = -1;
    int32_t ownerOffset = -1;
    uintptr_t propertyObject = 0;
    char controllerName[128] = {};
    char controllerClassName[128] = {};
    char pawnName[128] = {};
    char pawnClassName[128] = {};
    char weaponName[128] = {};
    char weaponClassName[128] = {};

    if (ValidateRuntimeObject(globals, names, controllerAddress, "PlayerController",
            controllerClass, controllerName, sizeof(controllerName),
            controllerClassName, sizeof(controllerClassName)) &&
        FindObjectProperty(globals, names, objects, "Pawn", controllerClass,
                           pawnOffset, propertyObject) &&
        ReadMem(controllerAddress + static_cast<uintptr_t>(pawnOffset), &pawn, sizeof(pawn)) &&
        ValidateRuntimeObject(globals, names, pawn, "Pawn", pawnClass,
            pawnName, sizeof(pawnName), pawnClassName, sizeof(pawnClassName))) {
        uintptr_t controllerBackReference = 0;
        if (FindObjectProperty(globals, names, objects, "Controller", pawnClass,
                               controllerOffset, propertyObject) &&
            ReadMem(pawn + static_cast<uintptr_t>(controllerOffset),
                    &controllerBackReference, sizeof(controllerBackReference)) &&
            controllerBackReference == controllerAddress) {
            pawnValid = true;
        }
    }

    if (!controllerClass || !tickTargetsOwner ||
        ClassDistance(controllerClass, tickTargetsOwner) < 0) {
        tickTargetsNameToken = 0;
        tickTargetsFunction = 0;
        tickTargetsOwner = 0;
    }

    const bool weaponOffsetsValid = pawnValid &&
        FindObjectProperty(globals, names, objects, "Weapon", pawnClass,
                           weaponOffset, propertyObject) &&
        FindObjectProperty(globals, names, objects, "Owner", pawnClass,
                           ownerOffset, propertyObject);
    if (weaponOffsetsValid &&
        ReadMem(pawn + static_cast<uintptr_t>(weaponOffset), &weapon, sizeof(weapon)) && weapon &&
        ValidateRuntimeObject(globals, names, weapon, "Weapon", weaponClass,
            weaponName, sizeof(weaponName), weaponClassName, sizeof(weaponClassName))) {
        uintptr_t ownerBackReference = 0;
        int32_t weaponOwnerOffset = -1;
        if (FindObjectProperty(globals, names, objects, "Owner", weaponClass,
                               weaponOwnerOffset, propertyObject) &&
            weaponOwnerOffset == ownerOffset &&
            ReadMem(weapon + static_cast<uintptr_t>(ownerOffset),
                    &ownerBackReference, sizeof(ownerBackReference)) &&
            ownerBackReference == pawn) {
            weaponValid = true;
        }
    }

    int bestAimOwnerDistance = 65;
    uintptr_t aimOwner = 0;
    bool aimSelectionAmbiguous = false;
    const bool aimCandidatesTruncated = totalAimCandidateCount > aimCandidateCount;
    AcquireSRWLockExclusive(&s_aimFunctionCacheLock);
    s_aimFunctionCandidateCount = aimCandidatesTruncated ? 0 : aimCandidateCount;
    if (!aimCandidatesTruncated && aimCandidateCount)
        memcpy(s_aimFunctionCandidates, aimCandidates,
               aimCandidateCount * sizeof(AimFunctionCandidate));
    s_aimFunctionCacheValid = !aimCandidatesTruncated && aimCandidateCount != 0;
    ReleaseSRWLockExclusive(&s_aimFunctionCacheLock);
    if (weaponValid && !aimCandidatesTruncated) {
        if (!SelectCachedAimFunction(weaponClass, getAdjustedAim, aimOwner,
                                     aimNameToken, bestAimOwnerDistance))
            aimSelectionAmbiguous = true;
    }
    if (aimSelectionAmbiguous) {
        getAdjustedAim = 0;
        aimOwner = 0;
        aimNameToken = 0;
    }
    char aimOwnerName[128] = {};
    if (getAdjustedAim && aimNameToken) {
        ReadObjectName(globals, names, aimOwner, aimOwnerName, sizeof(aimOwnerName));
        ReadMem(getAdjustedAim + 0xD0, &aimFlags, sizeof(aimFlags));
        ReadMem(getAdjustedAim + 0x84, &aimPropertySize, sizeof(aimPropertySize));
        ReadMem(getAdjustedAim + 0x94, &aimScriptSize, sizeof(aimScriptSize));
        ReadMem(getAdjustedAim + 0xE2, &aimParameterSize, sizeof(aimParameterSize));
    } else {
        Log("[WeaponAim] No unambiguous GetAdjustedAim UFunction belongs to the equipped "
            "weapon hierarchy (stored=%zu total=%zu truncated=%d ambiguous=%d)",
            aimCandidateCount, totalAimCandidateCount, aimCandidatesTruncated,
            aimSelectionAmbiguous);
    }

    AcquireSRWLockExclusive(&m_identityLock);
    const bool identityChanged =
        m_pawnIdentityValid.load(std::memory_order_acquire) != pawnValid ||
        m_weaponIdentityValid.load(std::memory_order_acquire) != weaponValid ||
        m_localController.load(std::memory_order_acquire) != controllerAddress ||
        m_localPawn.load(std::memory_order_acquire) != (pawnValid ? pawn : 0) ||
        m_localWeapon.load(std::memory_order_acquire) != (weaponValid ? weapon : 0);
    m_getAdjustedAimName.store(aimNameToken, std::memory_order_release);
    m_isInjuredName.store(isInjuredNameToken, std::memory_order_release);
    m_phaseWalkVisibilityName.store(
        phaseWalkVisibilityNameToken, std::memory_order_release);
    m_getAdjustedAimFunction.store(getAdjustedAim, std::memory_order_release);
    m_getAdjustedAimOwnerClass.store(aimOwner, std::memory_order_release);
    m_tickTargetsFunction.store(tickTargetsFunction, std::memory_order_release);
    m_localController.store(controllerAddress, std::memory_order_release);
    m_localPawn.store(pawnValid ? pawn : 0, std::memory_order_release);
    m_localWeapon.store(weaponValid ? weapon : 0, std::memory_order_release);
    m_pawnIdentityValid.store(pawnValid, std::memory_order_release);
    m_weaponIdentityValid.store(weaponValid, std::memory_order_release);
    if (pawnOffset > 0) m_pawnPropertyOffset = pawnOffset;
    if (controllerOffset > 0) m_controllerPropertyOffset = controllerOffset;
    if (weaponOffset > 0) m_weaponPropertyOffset = weaponOffset;
    if (ownerOffset > 0) m_ownerPropertyOffset = ownerOffset;
    const uint64_t identityGeneration = identityChanged
        ? m_identityGeneration.fetch_add(1, std::memory_order_acq_rel) + 1
        : m_identityGeneration.load(std::memory_order_acquire);
    ReleaseSRWLockExclusive(&m_identityLock);

    Log("[WeaponAim] GetAdjustedAim UFunction=%p owner=%p(%s) ownerDistance=%d "
        "candidates=%zu/%zu nameToken=0x%llX flags=0x%08X propertySize=%u "
        "scriptSize=%d paramSize=%u",
        reinterpret_cast<void*>(getAdjustedAim),
        reinterpret_cast<void*>(aimOwner), aimOwnerName, bestAimOwnerDistance,
        aimCandidateCount, totalAimCandidateCount,
        static_cast<unsigned long long>(aimNameToken), aimFlags,
        aimPropertySize, aimScriptSize, aimParameterSize);
    Log("[WeaponAim] Player identity generation=%llu pawnValid=%d weaponValid=%d "
        "controller=%p(%s/%s) pawn=%p(%s/%s) weapon=%p(%s/%s) "
        "offsets=Pawn+0x%X Controller+0x%X Weapon+0x%X Owner+0x%X",
        static_cast<unsigned long long>(identityGeneration), pawnValid, weaponValid,
        reinterpret_cast<void*>(controllerAddress), controllerName, controllerClassName,
        reinterpret_cast<void*>(pawn), pawnName, pawnClassName,
        reinterpret_cast<void*>(weapon), weaponName, weaponClassName,
        pawnOffset, controllerOffset, weaponOffset, ownerOffset);
    Log("[WeaponAim] Gameplay state probes: IsInjured=0x%llX PhaseWalk=0x%llX",
        static_cast<unsigned long long>(isInjuredNameToken),
        static_cast<unsigned long long>(phaseWalkVisibilityNameToken));

    if (!m_hookInstalled.load(std::memory_order_acquire)) {
        const uintptr_t processEvent = FindProcessEvent(
            controllerAddress, moduleBase, moduleSize);
        if (processEvent) Install(processEvent);
    }
    if (!m_callFunctionInstalled.load(std::memory_order_acquire)) {
        const uintptr_t callFunction = FindCallFunction(moduleBase, moduleSize);
        if (callFunction) InstallCallFunction(callFunction);
    }
    if (!m_usableSelectorInstalled.load(std::memory_order_acquire)) {
        const uintptr_t usableSelector = FindUsableSelector(moduleBase, moduleSize);
        if (usableSelector)
            InstallUsableSelectorHook(usableSelector, moduleBase, moduleSize);
    }
    Log("[InteractionAim] Usable selector hook: %s",
        m_usableSelectorInstalled.load(std::memory_order_acquire)
            ? "active" : "unavailable");
    static std::atomic<bool> lootReconDumped{false};
    if (!lootReconDumped.exchange(true, std::memory_order_acq_rel)) {
        const uintptr_t base = static_cast<uintptr_t>(moduleBase);
        DumpCode(base + 0x14A800, 1024, "TouchedCaller");
        DumpCode(base + 0x14CFD0, 128, "TouchedStub");
        DumpCode(base + 0x1DF747, 1024, "SeenCaller");
        DumpCode(base + 0x1DFB34, 128, "SeenStub");
        DumpCode(base + 0x167550, 1024, "TouchedCaller2");
        Log("[LootRecon] complete");
    }
    InstallGameplayStateProbes(isInjuredFunction, phaseWalkVisibilityFunction,
                               moduleBase, moduleSize);
    if (!m_meleeAttackInstalled.load(std::memory_order_acquire) ||
        !m_findMeleeTargetInstalled.load(std::memory_order_acquire)) {
        MeleeDiscovery melee = {};
        if (DiscoverMeleeMetadata(globals, names, objects, moduleBase, moduleSize,
                                  melee)) {
            Log("[MeleeRange] Reflected metadata: class=%p function=%p attack=%p params=0x%X "
                "context=+0x%X traceScale=+0x%X radiusScale=+0x%X "
                "findFunction=%p findTarget=%p params=0x%X maxLunge=+0x%X",
                reinterpret_cast<void*>(melee.meleeDefinitionClass),
                reinterpret_cast<void*>(melee.meleeAttackFunction),
                reinterpret_cast<void*>(melee.meleeAttackTarget),
                melee.meleeAttackParameterSize, melee.contextParameterOffset,
                melee.traceScaleOffset, melee.radiusScaleOffset,
                reinterpret_cast<void*>(melee.findMeleeTargetFunction),
                reinterpret_cast<void*>(melee.findMeleeTargetTarget),
                melee.findMeleeTargetParameterSize,
                melee.maxLungeDistanceParameterOffset);
            if (melee.meleeAttackTarget &&
                !m_meleeAttackInstalled.load(std::memory_order_acquire)) {
                InstallMeleeAttackHook(
                    melee.meleeAttackFunction, melee.meleeAttackTarget,
                    melee.meleeDefinitionClass,
                    globals.gObjectClassOffset, melee.contextParameterOffset,
                    melee.traceScaleOffset, melee.radiusScaleOffset);
            }
            if (melee.findMeleeTargetTarget &&
                !m_findMeleeTargetInstalled.load(std::memory_order_acquire)) {
                InstallFindMeleeTargetHook(
                    melee.findMeleeTargetFunction,
                    melee.findMeleeTargetTarget,
                    melee.maxLungeDistanceParameterOffset);
            }
        } else {
            Log("[MeleeRange] Reflected melee metadata unavailable; hooks remain disabled");
        }
    }
    InstallNativeAimProbe(moduleBase, moduleSize);
    if (tickTargetsFunction)
        InstallInteractionAimHook(tickTargetsFunction, moduleBase, moduleSize);
    if (!m_scriptInvokeInstalled.load(std::memory_order_acquire)) {
        for (size_t index = 0; index < aimCandidateCount; ++index) {
            if (InstallScriptInvokeProbe(
                    aimCandidates[index].function, moduleBase, moduleSize)) break;
        }
    } else if (getAdjustedAim && weaponValid) {
        InstallScriptInvokeProbe(getAdjustedAim, moduleBase, moduleSize);
    }
    Log("[WeaponAim] Ballistic path: script=%d override=%d aimFunction=%p",
        m_scriptInvokeInstalled.load(std::memory_order_acquire),
        m_ballisticOverrideEnabled.load(std::memory_order_acquire),
        reinterpret_cast<void*>(getAdjustedAim));
    Log("[InteractionAim] TickTargets token=0x%llX owner=%p hook=%s",
        static_cast<unsigned long long>(tickTargetsNameToken),
        reinterpret_cast<void*>(tickTargetsOwner),
        m_interactionHookInstalled.load(std::memory_order_acquire)
            ? "active" : "unavailable");
    m_traceFunction.store(traceFunction, std::memory_order_release);
    if (traceFunction)
        InstallTraceHook(traceFunction, moduleBase, moduleSize);
    Log("[InteractionAim] Trace hook: func=%p hook=%s",
        reinterpret_cast<void*>(traceFunction),
        m_traceHookInstalled.load(std::memory_order_acquire)
            ? "active" : "unavailable");
    m_traceActorsFunction.store(traceActorsFunction, std::memory_order_release);
    m_fastTraceFunction.store(fastTraceFunction, std::memory_order_release);
    m_viewPointFunction.store(viewPointFunction, std::memory_order_release);
    m_allowUseEventFunction.store(allowUseEventFunction, std::memory_order_release);
    const uint32_t publishPlayerTicks = playerTickFunctionCount > 8 ? 8 :
        static_cast<uint32_t>(playerTickFunctionCount);
    for (uint32_t i = 0; i < publishPlayerTicks; ++i)
        m_playerTickFunctions[i].store(playerTickFunctions[i], std::memory_order_release);
    m_playerTickFunctionCount.store(publishPlayerTicks, std::memory_order_release);
    const uint32_t publishAllowUse = allowUseFunctionCount > 8 ? 8 :
        static_cast<uint32_t>(allowUseFunctionCount);
    for (uint32_t i = 0; i < publishAllowUse; ++i)
        m_allowUseFunctions[i].store(allowUseFunctions[i], std::memory_order_release);
    m_allowUseFunctionCount.store(publishAllowUse, std::memory_order_release);
    Log("[InteractionAim] Node sets: playerTick=%u allowUse=%u",
        publishPlayerTicks, publishAllowUse);
    if (traceActorsFunction)
        InstallTraceProbe(traceActorsFunction, moduleBase, moduleSize, "TraceActors",
                          m_traceActorsTarget, &HookedTraceActors,
                          m_originalTraceActors, m_traceActorsInstalled);
    if (fastTraceFunction)
        InstallTraceProbe(fastTraceFunction, moduleBase, moduleSize, "FastTrace",
                          m_fastTraceTarget, &HookedFastTrace,
                          m_originalFastTrace, m_fastTraceInstalled);
    if (viewPointFunction)
        InstallTraceProbe(viewPointFunction, moduleBase, moduleSize, "GetPlayerViewPoint",
                          m_viewPointTarget, &HookedViewPoint,
                          m_originalViewPoint, m_viewPointInstalled);
    Log("[InteractionAim] Probes: traceActors=%p fastTrace=%p viewPoint=%p allowUse=%p",
        reinterpret_cast<void*>(traceActorsFunction),
        reinterpret_cast<void*>(fastTraceFunction),
        reinterpret_cast<void*>(viewPointFunction),
        reinterpret_cast<void*>(allowUseEventFunction));
    // Globals are valid here, so the candidate dump always runs (the manual
    // command fires too early at startup). One-shot per session.
    static std::atomic<bool> interactionDumped{false};
    if (!interactionDumped.exchange(true, std::memory_order_acq_rel))
        DumpInteractionCandidates();
    m_initialized.store(true, std::memory_order_release);
}

void __fastcall WeaponAimSystem::HookedCallFunction(
        void* object, void* frame, void* result, void* function) {
    auto& system = Instance();
    RecordInteractionCall(object, function, InteractionCallSource::CallFunction);
    const uintptr_t functionAddress = reinterpret_cast<uintptr_t>(function);
    const uintptr_t localController = system.m_localController.load(
        std::memory_order_acquire);
    const bool monitorFocus = localController >= 0x10000 &&
        system.m_pawnIdentityValid.load(std::memory_order_acquire);
    uintptr_t usableBefore = 0;
    if (monitorFocus)
        ReadDirect(localController + 0xE84, &usableBefore, sizeof(usableBefore));
    const bool tickTargets = functionAddress >= 0x10000 &&
        functionAddress == system.m_tickTargetsFunction.load(std::memory_order_acquire);
    const bool localTickTargets = tickTargets && localController >= 0x10000 &&
        reinterpret_cast<uintptr_t>(object) == localController;
    const bool allowUseEvent = functionAddress >= 0x10000 &&
        functionAddress == system.m_allowUseEventFunction.load(std::memory_order_acquire);

    static std::atomic<uint64_t> entryCount{0};
    const uint64_t entry = entryCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (entry <= 3) {
        Log("[InteractionAim] BL1E CallFunction entry #%llu object=%p frame=%p "
            "result=%p function=%p",
            static_cast<unsigned long long>(entry), object, frame, result, function);
    }
    if (tickTargets) {
        static std::atomic<uint64_t> tickHits{0};
        const uint64_t hit = tickHits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hit <= 10 || hit % 600 == 0) {
            Log("[InteractionAim] BL1E CallFunction TickTargets hit=%llu object=%p "
                "controller=%p match=%d",
                static_cast<unsigned long long>(hit), object,
                reinterpret_cast<void*>(localController), localTickTargets);
        }
    }
    if (allowUseEvent) {
        const uint64_t hit = system.m_allowUseEvents.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (hit <= 10 || hit % 200 == 0) {
            Log("[InteractionAim] BL1E CallFunction AllowUseEvent hit=%llu proxy=%p",
                static_cast<unsigned long long>(hit), object);
        }
    }

    float* cameraLocation = nullptr;
    int32_t* cameraRotation = nullptr;
    float savedLocation[3] = {};
    int32_t savedRotation[3] = {};
    float aimOrigin[3] = {};
    int32_t aimRotation[3] = {};
    bool redirected = false;
    if (localTickTargets) {
        bool aimFresh = false;
        AcquireSRWLockShared(&system.m_ballisticOverrideLock);
        const uint64_t updatedMs = system.m_aimUpdatedMs.load(std::memory_order_acquire);
        const uint64_t nowMs = GetTickCount64();
        aimFresh = system.m_aimValid.load(std::memory_order_acquire) &&
            updatedMs != 0 && nowMs >= updatedMs && nowMs - updatedMs <= 100;
        if (aimFresh) {
            aimOrigin[0] = system.m_aimOriginX.load(std::memory_order_relaxed);
            aimOrigin[1] = system.m_aimOriginY.load(std::memory_order_relaxed);
            aimOrigin[2] = system.m_aimOriginZ.load(std::memory_order_relaxed);
            aimRotation[0] = system.m_aimPitch.load(std::memory_order_relaxed);
            aimRotation[1] = system.m_aimYaw.load(std::memory_order_relaxed);
            aimRotation[2] = system.m_aimRoll.load(std::memory_order_relaxed);
        }
        ReleaseSRWLockShared(&system.m_ballisticOverrideLock);

        cameraLocation = camera::GetCameraLocation();
        cameraRotation = camera::GetCameraRotation();
        bool locationWritten = false;
        bool rotationWritten = false;
        if (aimFresh && cameraLocation && cameraRotation &&
            ReadDirect(reinterpret_cast<uintptr_t>(cameraLocation),
                       savedLocation, sizeof(savedLocation)) &&
            ReadDirect(reinterpret_cast<uintptr_t>(cameraRotation),
                       savedRotation, sizeof(savedRotation))) {
            locationWritten = WriteDirect(reinterpret_cast<uintptr_t>(cameraLocation),
                                          aimOrigin, sizeof(aimOrigin));
            rotationWritten = locationWritten && WriteDirect(
                reinterpret_cast<uintptr_t>(cameraRotation),
                aimRotation, sizeof(aimRotation));
            redirected = locationWritten && rotationWritten;
            if (!redirected) {
                if (locationWritten)
                    WriteDirect(reinterpret_cast<uintptr_t>(cameraLocation),
                                savedLocation, sizeof(savedLocation));
                if (rotationWritten)
                    WriteDirect(reinterpret_cast<uintptr_t>(cameraRotation),
                                savedRotation, sizeof(savedRotation));
            }
        }
    }

    if (system.m_originalCallFunction)
        system.m_originalCallFunction(object, frame, result, function);

    if (monitorFocus) {
        uintptr_t usableAfter = 0;
        if (ReadDirect(localController + 0xE84, &usableAfter, sizeof(usableAfter)) &&
            usableAfter != usableBefore) {
            LogInteractionFocusWriter("CallFunction",
                reinterpret_cast<uintptr_t>(object), functionAddress,
                usableBefore, usableAfter);
        }
    }

    if (redirected) {
        const bool locationRestored = WriteDirect(
            reinterpret_cast<uintptr_t>(cameraLocation),
            savedLocation, sizeof(savedLocation));
        const bool rotationRestored = WriteDirect(
            reinterpret_cast<uintptr_t>(cameraRotation),
            savedRotation, sizeof(savedRotation));
        const uint64_t count = system.m_interactionRedirectCount.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (count <= 10 || count % 600 == 0) {
            Log("[InteractionAim] BL1E CallFunction TickTargets redirected: count=%llu "
                "restored=%d",
                static_cast<unsigned long long>(count),
                locationRestored && rotationRestored);
        }
    }
}

void __fastcall WeaponAimSystem::HookedProcessEvent(
        void* object, void* function, void* params, void* nullArg) {
    auto& system = Instance();
    RecordInteractionCall(object, function, InteractionCallSource::ProcessEvent);
    const uintptr_t functionAddress = reinterpret_cast<uintptr_t>(function);
    const uintptr_t localController = system.m_localController.load(
        std::memory_order_acquire);
    const bool monitorFocus = localController >= 0x10000 &&
        system.m_pawnIdentityValid.load(std::memory_order_acquire);
    uintptr_t usableBefore = 0;
    if (monitorFocus)
        ReadDirect(localController + 0xE84, &usableBefore, sizeof(usableBefore));
    const bool tickTargets = functionAddress >= 0x10000 &&
        functionAddress == system.m_tickTargetsFunction.load(std::memory_order_acquire);
    const bool localTickTargets = tickTargets && localController >= 0x10000 &&
        reinterpret_cast<uintptr_t>(object) == localController;
    const bool allowUseEvent = functionAddress >= 0x10000 &&
        functionAddress == system.m_allowUseEventFunction.load(std::memory_order_acquire);

    static std::atomic<uint64_t> entryCount{0};
    const uint64_t entry = entryCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (entry <= 3) {
        Log("[InteractionAim] BL1E ProcessEvent entry #%llu object=%p function=%p null=%p",
            static_cast<unsigned long long>(entry), object, function, nullArg);
    }

    if (tickTargets) {
        static std::atomic<uint64_t> tickHits{0};
        const uint64_t hit = tickHits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hit <= 10 || hit % 600 == 0) {
            Log("[InteractionAim] BL1E ProcessEvent TickTargets hit=%llu object=%p "
                "controller=%p match=%d",
                static_cast<unsigned long long>(hit), object,
                reinterpret_cast<void*>(localController), localTickTargets);
        }
    }
    if (allowUseEvent) {
        const uint64_t hit = system.m_allowUseEvents.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (hit <= 10 || hit % 200 == 0) {
            Log("[InteractionAim] BL1E ProcessEvent AllowUseEvent hit=%llu proxy=%p",
                static_cast<unsigned long long>(hit), object);
        }
    }

    float* cameraLocation = nullptr;
    int32_t* cameraRotation = nullptr;
    float savedLocation[3] = {};
    int32_t savedRotation[3] = {};
    float aimOrigin[3] = {};
    int32_t aimRotation[3] = {};
    bool redirected = false;
    if (localTickTargets) {
        bool aimFresh = false;
        AcquireSRWLockShared(&system.m_ballisticOverrideLock);
        const uint64_t updatedMs = system.m_aimUpdatedMs.load(std::memory_order_acquire);
        const uint64_t nowMs = GetTickCount64();
        aimFresh = system.m_aimValid.load(std::memory_order_acquire) &&
            updatedMs != 0 && nowMs >= updatedMs && nowMs - updatedMs <= 100;
        if (aimFresh) {
            aimOrigin[0] = system.m_aimOriginX.load(std::memory_order_relaxed);
            aimOrigin[1] = system.m_aimOriginY.load(std::memory_order_relaxed);
            aimOrigin[2] = system.m_aimOriginZ.load(std::memory_order_relaxed);
            aimRotation[0] = system.m_aimPitch.load(std::memory_order_relaxed);
            aimRotation[1] = system.m_aimYaw.load(std::memory_order_relaxed);
            aimRotation[2] = system.m_aimRoll.load(std::memory_order_relaxed);
        }
        ReleaseSRWLockShared(&system.m_ballisticOverrideLock);

        cameraLocation = camera::GetCameraLocation();
        cameraRotation = camera::GetCameraRotation();
        bool locationWritten = false;
        bool rotationWritten = false;
        if (aimFresh && cameraLocation && cameraRotation &&
            ReadDirect(reinterpret_cast<uintptr_t>(cameraLocation),
                       savedLocation, sizeof(savedLocation)) &&
            ReadDirect(reinterpret_cast<uintptr_t>(cameraRotation),
                       savedRotation, sizeof(savedRotation))) {
            locationWritten = WriteDirect(reinterpret_cast<uintptr_t>(cameraLocation),
                                          aimOrigin, sizeof(aimOrigin));
            rotationWritten = locationWritten && WriteDirect(
                reinterpret_cast<uintptr_t>(cameraRotation),
                aimRotation, sizeof(aimRotation));
            redirected = locationWritten && rotationWritten;
            if (!redirected) {
                if (locationWritten)
                    WriteDirect(reinterpret_cast<uintptr_t>(cameraLocation),
                                savedLocation, sizeof(savedLocation));
                if (rotationWritten)
                    WriteDirect(reinterpret_cast<uintptr_t>(cameraRotation),
                                savedRotation, sizeof(savedRotation));
            }
        }
    }

    if (system.m_originalProcessEvent)
        system.m_originalProcessEvent(object, function, params, nullArg);

    if (monitorFocus) {
        uintptr_t usableAfter = 0;
        if (ReadDirect(localController + 0xE84, &usableAfter, sizeof(usableAfter)) &&
            usableAfter != usableBefore) {
            LogInteractionFocusWriter("ProcessEvent",
                reinterpret_cast<uintptr_t>(object), functionAddress,
                usableBefore, usableAfter);
        }
    }

    if (redirected) {
        const bool locationRestored = WriteDirect(
            reinterpret_cast<uintptr_t>(cameraLocation),
            savedLocation, sizeof(savedLocation));
        const bool rotationRestored = WriteDirect(
            reinterpret_cast<uintptr_t>(cameraRotation),
            savedRotation, sizeof(savedRotation));
        const uint64_t count = system.m_interactionRedirectCount.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (count <= 10 || count % 600 == 0) {
            Log("[InteractionAim] BL1E ProcessEvent TickTargets redirected: count=%llu "
                "restored=%d",
                static_cast<unsigned long long>(count),
                locationRestored && rotationRestored);
        }
    }
}

PlayerIdentitySnapshot WeaponAimSystem::GetPlayerIdentity() {
    PlayerIdentitySnapshot snapshot;
    int32_t pawnOffset = -1;
    int32_t controllerOffset = -1;
    int32_t weaponOffset = -1;
    int32_t ownerOffset = -1;
    AcquireSRWLockShared(&m_identityLock);
    snapshot.generation = m_identityGeneration.load(std::memory_order_acquire);
    snapshot.pawnValid = m_pawnIdentityValid.load(std::memory_order_acquire);
    snapshot.weaponValid = m_weaponIdentityValid.load(std::memory_order_acquire);
    snapshot.controller = m_localController.load(std::memory_order_acquire);
    snapshot.pawn = m_localPawn.load(std::memory_order_acquire);
    snapshot.weapon = m_localWeapon.load(std::memory_order_acquire);
    pawnOffset = m_pawnPropertyOffset;
    controllerOffset = m_controllerPropertyOffset;
    weaponOffset = m_weaponPropertyOffset;
    ownerOffset = m_ownerPropertyOffset;
    ReleaseSRWLockShared(&m_identityLock);

    uintptr_t currentPawn = 0;
    uintptr_t currentController = 0;
    if (snapshot.pawnValid &&
        (pawnOffset <= 0 || controllerOffset <= 0 ||
         !ReadMem(snapshot.controller + static_cast<uintptr_t>(pawnOffset),
                  &currentPawn, sizeof(currentPawn)) || currentPawn != snapshot.pawn ||
         !ReadMem(snapshot.pawn + static_cast<uintptr_t>(controllerOffset),
                  &currentController, sizeof(currentController)) ||
         currentController != snapshot.controller)) {
        snapshot.pawnValid = false;
    }
    if (!snapshot.pawnValid) snapshot.weaponValid = false;
    uintptr_t currentWeapon = 0;
    uintptr_t currentOwner = 0;
    bool weaponRelationshipValid = snapshot.pawnValid && weaponOffset > 0 && ownerOffset > 0 &&
        ReadMem(snapshot.pawn + static_cast<uintptr_t>(weaponOffset),
                &currentWeapon, sizeof(currentWeapon)) && currentWeapon >= 0x10000 &&
        ReadMem(currentWeapon + static_cast<uintptr_t>(ownerOffset),
                &currentOwner, sizeof(currentOwner)) && currentOwner == snapshot.pawn;

    if (weaponRelationshipValid &&
        (!snapshot.weaponValid || currentWeapon != snapshot.weapon)) {
        const camera::UE3Globals globals = camera::GetUE3GlobalsSnapshot();
        TArray64 names = {};
        uintptr_t weaponClass = 0;
        char weaponName[128] = {};
        char weaponClassName[128] = {};
        weaponRelationshipValid = globals.gNamesValid &&
            ReadMem(globals.gNamesAddress, &names, sizeof(names)) &&
            ValidateRuntimeObject(globals, names, currentWeapon, "Weapon", weaponClass,
                weaponName, sizeof(weaponName), weaponClassName, sizeof(weaponClassName));
        if (weaponRelationshipValid) {
            bool changed = false;
            bool aimInvalidated = false;
            const uintptr_t selectedAimFunction =
                m_getAdjustedAimFunction.load(std::memory_order_acquire);
            const uintptr_t selectedAimOwner =
                m_getAdjustedAimOwnerClass.load(std::memory_order_acquire);
            const bool selectedAimCompatible = selectedAimFunction == 0 ||
                ClassDistance(weaponClass, selectedAimOwner) >= 0;
            AcquireSRWLockExclusive(&m_identityLock);
            uintptr_t publishWeapon = 0;
            uintptr_t publishOwner = 0;
            if (m_pawnIdentityValid.load(std::memory_order_acquire) &&
                m_localController.load(std::memory_order_acquire) == snapshot.controller &&
                m_localPawn.load(std::memory_order_acquire) == snapshot.pawn &&
                m_weaponPropertyOffset == weaponOffset && m_ownerPropertyOffset == ownerOffset &&
                ReadMem(snapshot.pawn + static_cast<uintptr_t>(weaponOffset),
                        &publishWeapon, sizeof(publishWeapon)) && publishWeapon == currentWeapon &&
                ReadMem(currentWeapon + static_cast<uintptr_t>(ownerOffset),
                        &publishOwner, sizeof(publishOwner)) && publishOwner == snapshot.pawn) {
                changed = m_localWeapon.load(std::memory_order_acquire) != currentWeapon ||
                    !m_weaponIdentityValid.load(std::memory_order_acquire);
                m_localWeapon.store(currentWeapon, std::memory_order_release);
                m_weaponIdentityValid.store(true, std::memory_order_release);
                if (changed && !selectedAimCompatible) {
                    m_getAdjustedAimName.store(0, std::memory_order_release);
                    m_getAdjustedAimFunction.store(0, std::memory_order_release);
                    m_getAdjustedAimOwnerClass.store(0, std::memory_order_release);
                    aimInvalidated = true;
                }
                if (changed) m_identityGeneration.fetch_add(1, std::memory_order_acq_rel);
                if (changed) m_scriptInvokeAimCalls.store(0, std::memory_order_release);
                snapshot.weapon = currentWeapon;
                snapshot.weaponValid = true;
                snapshot.generation = m_identityGeneration.load(std::memory_order_acquire);
            } else {
                weaponRelationshipValid = false;
            }
            ReleaseSRWLockExclusive(&m_identityLock);
            if (changed) {
                Log("[WeaponAim] Equipped weapon identity refreshed: weapon=%p(%s/%s) "
                    "owner=%p generation=%llu aimCompatible=%d",
                    reinterpret_cast<void*>(currentWeapon), weaponName, weaponClassName,
                    reinterpret_cast<void*>(currentOwner),
                    static_cast<unsigned long long>(snapshot.generation),
                    !aimInvalidated);
            }
        }
    }
    if (!weaponRelationshipValid) {
        bool invalidated = false;
        AcquireSRWLockExclusive(&m_identityLock);
        if (m_identityGeneration.load(std::memory_order_acquire) == snapshot.generation &&
            m_localController.load(std::memory_order_acquire) == snapshot.controller &&
            m_localPawn.load(std::memory_order_acquire) == snapshot.pawn) {
            invalidated = m_weaponIdentityValid.load(std::memory_order_acquire) ||
                m_localWeapon.load(std::memory_order_acquire) != 0;
            m_localWeapon.store(0, std::memory_order_release);
            m_weaponIdentityValid.store(false, std::memory_order_release);
            if (invalidated) m_identityGeneration.fetch_add(1, std::memory_order_acq_rel);
            snapshot.generation = m_identityGeneration.load(std::memory_order_acquire);
        }
        ReleaseSRWLockExclusive(&m_identityLock);
        snapshot.weaponValid = false;
        snapshot.weapon = 0;
    }
    if (!snapshot.pawnValid) {
        snapshot.controller = 0;
        snapshot.pawn = 0;
        snapshot.weapon = 0;
        snapshot.weaponValid = false;
    } else if (!snapshot.weaponValid) {
        snapshot.weapon = 0;
    }
    return snapshot;
}

bool WeaponAimSystem::IsVehicleTerminalUiActive() const {
    const uintptr_t controller = m_localController.load(std::memory_order_acquire);
    int32_t pawnOffset = -1;
    AcquireSRWLockShared(&m_identityLock);
    pawnOffset = m_pawnPropertyOffset;
    ReleaseSRWLockShared(&m_identityLock);
    if (controller < 0x10000 || pawnOffset <= 0) return false;

    uintptr_t livePawn = 0;
    if (!ReadMem(controller + static_cast<uintptr_t>(pawnOffset),
                 &livePawn, sizeof(livePawn)) || livePawn < 0x10000) return false;
    const camera::UE3Globals globals = camera::GetUE3GlobalsSnapshot();
    TArray64 names = {};
    char className[128] = {};
    return globals.gNamesValid &&
        ReadMem(globals.gNamesAddress, &names, sizeof(names)) &&
        ReadClassName(globals, names, livePawn, className, sizeof(className)) &&
        strcmp(className, "WillowWeaponPawn") == 0;
}

bool WeaponAimSystem::IsPhaseWalkActive() {
    if (!m_phaseWalkActive.load(std::memory_order_acquire)) return false;
    const PlayerIdentitySnapshot identity = GetPlayerIdentity();
    if (identity.pawnValid && !identity.weaponValid) return true;
    if (m_phaseWalkActive.exchange(false, std::memory_order_acq_rel))
        Log("[WeaponAim] Native Phasewalk state ended");
    return false;
}

bool WeaponAimSystem::RefreshIdentityFromLivePawn(uintptr_t controller, uintptr_t pawn) {
    if (controller < 0x10000 || pawn < 0x10000) return false;
    int32_t pawnOffset = -1;
    int32_t controllerOffset = -1;
    int32_t weaponOffset = -1;
    int32_t ownerOffset = -1;
    AcquireSRWLockShared(&m_identityLock);
    pawnOffset = m_pawnPropertyOffset;
    controllerOffset = m_controllerPropertyOffset;
    weaponOffset = m_weaponPropertyOffset;
    ownerOffset = m_ownerPropertyOffset;
    ReleaseSRWLockShared(&m_identityLock);
    // Reflected and runtime-validated for this fixed BL1 GOTY Enhanced build.
    // They let live identity publish immediately while the async scanner catches up.
    if (pawnOffset <= 0) pawnOffset = 0x260;
    if (controllerOffset <= 0) controllerOffset = 0x26C;
    if (weaponOffset <= 0) weaponOffset = 0x584;
    if (ownerOffset <= 0) ownerOffset = 0xD4;

    uintptr_t publishedPawn = 0;
    uintptr_t publishedController = 0;
    uintptr_t weapon = 0;
    uintptr_t owner = 0;
    if (!ReadMem(controller + static_cast<uintptr_t>(pawnOffset),
                 &publishedPawn, sizeof(publishedPawn)) || publishedPawn != pawn ||
        !ReadMem(pawn + static_cast<uintptr_t>(controllerOffset),
                  &publishedController, sizeof(publishedController)) ||
        publishedController != controller) return false;

    const camera::UE3Globals globals = camera::GetUE3GlobalsSnapshot();
    TArray64 names = {};
    uintptr_t pawnClass = 0;
    uintptr_t weaponClass = 0;
    char pawnName[128] = {};
    char pawnClassName[128] = {};
    char weaponName[128] = {};
    char weaponClassName[128] = {};
    if (!globals.gNamesValid ||
        !ReadMem(globals.gNamesAddress, &names, sizeof(names)) ||
        !ValidateRuntimeObject(globals, names, pawn, "Pawn", pawnClass,
                               pawnName, sizeof(pawnName), pawnClassName,
                               sizeof(pawnClassName))) return false;
    const bool weaponValid =
        ReadMem(pawn + static_cast<uintptr_t>(weaponOffset), &weapon, sizeof(weapon)) &&
        weapon >= 0x10000 &&
        ReadMem(weapon + static_cast<uintptr_t>(ownerOffset), &owner, sizeof(owner)) &&
        owner == pawn &&
        ValidateRuntimeObject(globals, names, weapon, "Weapon", weaponClass,
                              weaponName, sizeof(weaponName), weaponClassName,
                              sizeof(weaponClassName));
    if (!weaponValid) weapon = 0;

    bool changed = false;
    uintptr_t selectedAimFunction = 0;
    uintptr_t selectedAimOwner = 0;
    uint64_t selectedAimName = 0;
    int selectedAimOwnerDistance = 65;
    const bool selectedAimCompatible = weaponValid && SelectCachedAimFunction(
        weaponClass, selectedAimFunction, selectedAimOwner,
        selectedAimName, selectedAimOwnerDistance);
    AcquireSRWLockExclusive(&m_identityLock);
    changed = m_localController.load(std::memory_order_acquire) != controller ||
        m_localPawn.load(std::memory_order_acquire) != pawn ||
        m_localWeapon.load(std::memory_order_acquire) != weapon ||
        !m_pawnIdentityValid.load(std::memory_order_acquire) ||
        m_weaponIdentityValid.load(std::memory_order_acquire) != weaponValid;
    m_localController.store(controller, std::memory_order_release);
    m_localPawn.store(pawn, std::memory_order_release);
    m_localWeapon.store(weapon, std::memory_order_release);
    m_pawnIdentityValid.store(true, std::memory_order_release);
    m_weaponIdentityValid.store(weaponValid, std::memory_order_release);
    m_pawnPropertyOffset = pawnOffset;
    m_controllerPropertyOffset = controllerOffset;
    m_weaponPropertyOffset = weaponOffset;
    m_ownerPropertyOffset = ownerOffset;
    m_getAdjustedAimName.store(selectedAimCompatible ? selectedAimName : 0,
                               std::memory_order_release);
    m_getAdjustedAimFunction.store(selectedAimCompatible ? selectedAimFunction : 0,
                                   std::memory_order_release);
    m_getAdjustedAimOwnerClass.store(selectedAimCompatible ? selectedAimOwner : 0,
                                     std::memory_order_release);
    if (changed) m_identityGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (changed) m_scriptInvokeAimCalls.store(0, std::memory_order_release);
    const uint64_t generation = m_identityGeneration.load(std::memory_order_acquire);
    ReleaseSRWLockExclusive(&m_identityLock);
    if (changed) {
        Log("[WeaponAim] Fast identity refreshed: controller=%p pawn=%p(%s/%s) "
            "weapon=%p(%s/%s) generation=%llu aimCompatible=%d ownerDistance=%d",
            reinterpret_cast<void*>(controller), reinterpret_cast<void*>(pawn),
            pawnName, pawnClassName, reinterpret_cast<void*>(weapon), weaponName,
            weaponClassName, static_cast<unsigned long long>(generation),
            selectedAimCompatible, selectedAimOwnerDistance);
    }
    return weaponValid;
}

void WeaponAimSystem::DumpInteractionCandidates() {
    // Diagnostic: list UFunction addresses for interaction-related names so
    // they can be matched against sampled ProcessEvent func pointers from the
    // same session (heap addresses change every run).
    static const char* kWatch[] = {
        "AllowUseEvent", "TickTargets", "GetPlayerViewPoint", "PlayerTick",
        "Trace", "FastTrace", "GetAdjustedAim", "IsInteractionDebugEnabled",
        "GetPlayerInteractionManager", "GetInteractionPlayers", "DirtyViewPoint",
        "CheckViewTarget", "GetViewTarget", "PickTarget", "Interact", "Focus",
        "Use", "Aim", "Look", "Prompt", "Hud", "HUD", "Crosshair", "Highlight",
        "View", "Target", "Tick"
    };
    const auto globals = camera::GetUE3GlobalsSnapshot();
    if (!globals.gNamesValid || !globals.gObjectsValid) {
        Log("[InteractionAim] Dump unavailable: globals not discovered yet");
        return;
    }
    TArray64 names = {};
    TArray64 objects = {};
    if (!ReadMem(globals.gNamesAddress, &names, sizeof(names)) ||
        !ReadMem(globals.gObjectsAddress, &objects, sizeof(objects))) {
        Log("[InteractionAim] Dump unavailable: cannot read GNames/GObjects");
        return;
    }
    size_t logged = 0;
    size_t matched = 0;
    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t object = 0;
        char objectName[128] = {};
        char className[128] = {};
        if (!ReadMem(objects.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                     &object, sizeof(object)) || !object ||
            !ReadObjectName(globals, names, object, objectName, sizeof(objectName)) ||
            !ReadClassName(globals, names, object, className, sizeof(className)) ||
            strcmp(className, "Function") != 0) continue;
        bool watched = false;
        for (const char* candidate : kWatch) {
            if (strstr(objectName, candidate) != nullptr) { watched = true; break; }
        }
        if (!watched) continue;
        uintptr_t owner = 0;
        char ownerName[128] = {};
        uint32_t flags = 0;
        uint16_t paramSize = 0;
        uintptr_t target = 0;
        ReadOuter(globals, object, owner);
        if (owner) ReadObjectName(globals, names, owner, ownerName, sizeof(ownerName));
        ReadMem(object + 0xD0, &flags, sizeof(flags));
        ReadMem(object + 0xE2, &paramSize, sizeof(paramSize));
        ReadMem(object + 0xF0, &target, sizeof(target));
        if (logged < 2048) {
            Log("[InteractionAim] Candidate %s owner=%s func=%p ownerPtr=%p flags=0x%X params=%u native=%p",
                objectName, ownerName, reinterpret_cast<void*>(object),
                reinterpret_cast<void*>(owner), flags, paramSize,
                reinterpret_cast<void*>(target));
            ++logged;
        }
        ++matched;
    }
    Log("[InteractionAim] Candidate dump complete: %llu matched %llu logged",
        static_cast<unsigned long long>(matched),
        static_cast<unsigned long long>(logged));
}

void WeaponAimSystem::ResolveAddress(uintptr_t address) {
    // Live diagnostic: resolve any heap pointer to Class::Name. Runs in the
    // command-poll context (unlike the ProcessEvent hook) to isolate whether
    // the in-hook resolution failures are contextual.
    const auto globals = camera::GetUE3GlobalsSnapshot();
    if (address < 0x10000) {
        Log("[InteractionAim] Resolve: address too low");
        return;
    }
    TArray64 names = {};
    const bool namesRead = globals.gNamesValid &&
        ReadMem(globals.gNamesAddress, &names, sizeof(names));
    int32_t index = -1;
    uintptr_t classObject = 0;
    const bool indexRead = ReadMem(address + globals.gObjectNameOffset,
                                   &index, sizeof(index));
    const bool classRead = ReadMem(address + globals.gObjectClassOffset,
                                   &classObject, sizeof(classObject));
    char objectName[128] = {};
    char className[128] = {};
    const bool nameOk = namesRead && indexRead &&
        ReadName(globals, names, index, objectName, sizeof(objectName));
    const bool classOk = namesRead && classRead && classObject >= 0x10000 &&
        ReadObjectName(globals, names, classObject, className, sizeof(className));
    uintptr_t outer = 0;
    char outerName[128] = {};
    const bool outerOk = ReadOuter(globals, address, outer) && outer >= 0x10000 &&
        namesRead && ReadObjectName(globals, names, outer, outerName, sizeof(outerName));
    Log("[InteractionAim] Resolve %p: index=%d class=%p name='%s' className='%s' "
        "outer=%p('%s') namesCount=%d",
        reinterpret_cast<void*>(address), index,
        reinterpret_cast<void*>(classObject),
        nameOk ? objectName : "?",
        classOk ? className : "?",
        reinterpret_cast<void*>(outer),
        outerOk ? outerName : "?",
        namesRead ? names.count : -1);
}

void WeaponAimSystem::DumpInteractionState() {
    // Live RE: enumerate InteractionProxy instances and dump the
    // PlayerInteractionManager pointer slots with resolved classes.
    // Run while facing a chest (prompt visible), then looking away, and diff.
    const auto globals = camera::GetUE3GlobalsSnapshot();
    if (!globals.gNamesValid || !globals.gObjectsValid) {
        Log("[InteractionAim] IProbe unavailable: globals not discovered yet");
        return;
    }
    TArray64 names = {};
    TArray64 objects = {};
    if (!ReadMem(globals.gNamesAddress, &names, sizeof(names)) ||
        !ReadMem(globals.gObjectsAddress, &objects, sizeof(objects))) {
        Log("[InteractionAim] IProbe unavailable: cannot read GNames/GObjects");
        return;
    }
    uintptr_t manager = 0;
    uintptr_t interactionClient = 0;
    size_t proxyCount = 0;
    size_t interactiveCount = 0;
    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t object = 0;
        if (!ReadMem(objects.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                     &object, sizeof(object)) || !object) continue;
        char className[128] = {};
        if (!ReadClassName(globals, names, object, className, sizeof(className)))
            continue;
        // Prefer live instances over class archetypes (Default__* holds
        // metadata, not live focus state).
        char liveName[128] = {};
        const bool named = ReadObjectName(globals, names, object,
                                          liveName, sizeof(liveName));
        const bool isDefault = named && strncmp(liveName, "Default__", 9) == 0;
        if (strcmp(className, "PlayerInteractionManager") == 0 &&
            (manager == 0 || !isDefault)) {
            char prevName[128] = {};
            bool prevDefault = true;
            if (manager != 0 && ReadObjectName(globals, names, manager,
                                               prevName, sizeof(prevName)))
                prevDefault = strncmp(prevName, "Default__", 9) == 0;
            if (manager == 0 || (prevDefault && !isDefault))
                manager = object;
        } else if (strcmp(className, "PlayerInteractionClient") == 0 &&
                   (interactionClient == 0 || !isDefault)) {
            char prevName[128] = {};
            bool prevDefault = true;
            if (interactionClient != 0 && ReadObjectName(globals, names, interactionClient,
                                                         prevName, sizeof(prevName)))
                prevDefault = strncmp(prevName, "Default__", 9) == 0;
            if (interactionClient == 0 || (prevDefault && !isDefault))
                interactionClient = object;
        }
        // Match subclasses too: anything with Proxy/Interact/Interactive,
        // excluding UI/GFx plumbing.
        if ((strstr(className, "Proxy") != nullptr ||
             strstr(className, "Interact") != nullptr) &&
            strstr(className, "UI") == nullptr &&
            strstr(className, "GFx") == nullptr &&
            strstr(className, "SceneClient") == nullptr) {
            if (proxyCount < 64) {
                char objectName[128] = {};
                ReadObjectName(globals, names, object, objectName, sizeof(objectName));
                Log("[InteractionAim] IProbe proxy[%llu]=%p class=%s name=%s",
                    static_cast<unsigned long long>(proxyCount),
                    reinterpret_cast<void*>(object), className, objectName);
            }
            ++proxyCount;
        }
        if (strstr(className, "InteractiveObject") != nullptr) {
            if (interactiveCount < 64) {
                char objectName[128] = {};
                ReadObjectName(globals, names, object, objectName, sizeof(objectName));
                Log("[InteractionAim] IProbe interactive[%llu]=%p class=%s name=%s",
                    static_cast<unsigned long long>(interactiveCount),
                    reinterpret_cast<void*>(object), className, objectName);
            }
            ++interactiveCount;
        }
    }
    char managerName[128] = {};
    char clientName[128] = {};
    if (manager >= 0x10000)
        ReadObjectName(globals, names, manager, managerName, sizeof(managerName));
    if (interactionClient >= 0x10000)
        ReadObjectName(globals, names, interactionClient, clientName, sizeof(clientName));
    Log("[InteractionAim] IProbe: manager=%p(%s) client=%p(%s) proxies=%llu interactive=%llu",
        reinterpret_cast<void*>(manager), managerName,
        reinterpret_cast<void*>(interactionClient), clientName,
        static_cast<unsigned long long>(proxyCount),
        static_cast<unsigned long long>(interactiveCount));
    // Dump pointer-sized slots of manager, client, local controller and
    // local pawn; the focus reference may live on any of them.
    const uintptr_t localController = m_localController.load(std::memory_order_acquire);
    const uintptr_t localPawn = m_localPawn.load(std::memory_order_acquire);
    const uintptr_t bases[4] = {manager, interactionClient, localController, localPawn};
    const char* baseTags[4] = {"mgr", "cli", "ctl", "pwn"};
    const char* baseNames[4] = {managerName, clientName, "controller", "pawn"};
    for (int pass = 0; pass < 4; ++pass) {
        const uintptr_t base = bases[pass];
        if (base < 0x10000) continue;
        Log("[InteractionAim] IProbe slots of %p (%s %s):",
            reinterpret_cast<void*>(base), baseTags[pass], baseNames[pass]);
        for (uint32_t offset = 0; offset < 0x800; offset += 8) {
            uintptr_t slot = 0;
            if (!ReadMem(base + offset, &slot, sizeof(slot)) || slot < 0x10000)
                continue;
        char targetClass[128] = {};
        char targetName[128] = {};
        const bool classOk = ReadClassName(globals, names, slot,
                                           targetClass, sizeof(targetClass));
        const bool nameOk = classOk && ReadObjectName(globals, names, slot,
                                                      targetName, sizeof(targetName));
        if (!classOk) continue;
        // Skip uninteresting engine singletons to keep the log focused.
        if (strcmp(targetClass, "Class") == 0 ||
            strcmp(targetClass, "ScriptStruct") == 0 ||
            strcmp(targetClass, "Function") == 0 ||
            strcmp(targetClass, "NameProperty") == 0 ||
            strcmp(targetClass, "ObjectProperty") == 0 ||
            strcmp(targetClass, "IntProperty") == 0 ||
            strcmp(targetClass, "FloatProperty") == 0 ||
            strcmp(targetClass, "BoolProperty") == 0 ||
            strcmp(targetClass, "StrProperty") == 0 ||
            strcmp(targetClass, "ArrayProperty") == 0 ||
            strcmp(targetClass, "StructProperty") == 0 ||
            strcmp(targetClass, "ByteProperty") == 0) continue;
        Log("[InteractionAim] IProbe %s+0x%X -> %p %s::%s",
            baseTags[pass], offset,
            reinterpret_cast<void*>(slot), targetClass,
            nameOk ? targetName : "?");
        }
    }
    Log("[InteractionAim] IProbe dump complete");
}

void WeaponAimSystem::FindInteractionReferrers() {
    // Reverse-pointer search: collect live WillowInteractiveObject actors,
    // then scan manager/client/controller/pawn/HUD memory for slots that
    // reference them. The holder of the faced actor owns the focus state.
    const auto globals = camera::GetUE3GlobalsSnapshot();
    if (!globals.gNamesValid || !globals.gObjectsValid) {
        Log("[InteractionAim] IFind unavailable: globals not discovered yet");
        return;
    }
    TArray64 names = {};
    TArray64 objects = {};
    if (!ReadMem(globals.gNamesAddress, &names, sizeof(names)) ||
        !ReadMem(globals.gObjectsAddress, &objects, sizeof(objects))) {
        Log("[InteractionAim] IFind unavailable: cannot read GNames/GObjects");
        return;
    }
    uintptr_t liveActors[64] = {};
    size_t liveCount = 0;
    uintptr_t hud = 0;
    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t object = 0;
        if (!ReadMem(objects.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                     &object, sizeof(object)) || !object) continue;
        char className[128] = {};
        if (!ReadClassName(globals, names, object, className, sizeof(className)))
            continue;
        if (strcmp(className, "WillowInteractiveObject") == 0) {
            char objectName[128] = {};
            ReadObjectName(globals, names, object, objectName, sizeof(objectName));
            if (strncmp(objectName, "Default__", 9) == 0) continue;
            if (liveCount < 64) liveActors[liveCount++] = object;
        } else if (strcmp(className, "WillowHUD") == 0 && hud == 0) {
            char objectName[128] = {};
            ReadObjectName(globals, names, object, objectName, sizeof(objectName));
            if (strncmp(objectName, "Default__", 9) != 0) hud = object;
        }
    }
    Log("[InteractionAim] IFind: liveInteractives=%llu hud=%p",
        static_cast<unsigned long long>(liveCount),
        reinterpret_cast<void*>(hud));
    if (!liveCount) return;
    const uintptr_t localController = m_localController.load(std::memory_order_acquire);
    const uintptr_t localPawn = m_localPawn.load(std::memory_order_acquire);
    uintptr_t manager = 0;
    for (int32_t index = 0; index < objects.count && manager == 0; ++index) {
        uintptr_t object = 0;
        if (!ReadMem(objects.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                     &object, sizeof(object)) || !object) continue;
        char className[128] = {};
        char objectName[128] = {};
        if (!ReadClassName(globals, names, object, className, sizeof(className)))
            continue;
        if (strcmp(className, "PlayerInteractionManager") != 0) continue;
        if (!ReadObjectName(globals, names, object, objectName, sizeof(objectName)) ||
            strncmp(objectName, "Default__", 9) == 0) continue;
        manager = object;
    }
    const uintptr_t bases[4] = {manager, localController, localPawn, hud};
    const char* baseTags[4] = {"mgr", "ctl", "pwn", "hud"};
    for (int pass = 0; pass < 4; ++pass) {
        const uintptr_t base = bases[pass];
        if (base < 0x10000) {
            Log("[InteractionAim] IFind %s: holder unavailable", baseTags[pass]);
            continue;
        }
        size_t hits = 0;
        for (uint32_t offset = 0; offset < 0x2000 && hits < 16; offset += 8) {
            uintptr_t slot = 0;
            if (!ReadMem(base + offset, &slot, sizeof(slot)) || slot < 0x10000)
                continue;
            for (size_t a = 0; a < liveCount; ++a) {
                if (slot == liveActors[a]) {
                    Log("[InteractionAim] IFind HIT %s+0x%X -> live actor %p",
                        baseTags[pass], offset,
                        reinterpret_cast<void*>(slot));
                    ++hits;
                    break;
                }
            }
        }
        if (!hits)
            Log("[InteractionAim] IFind %s: no live-actor references", baseTags[pass]);
    }
    Log("[InteractionAim] IFind complete");
}

void WeaponAimSystem::FindInteractionReferrersIndirect() {
    // Indirect search: live actors may be referenced through TArrays.
    // For each holder slot holding a heap pointer P, scan P[0..256] for
    // exact live-actor addresses.
    const auto globals = camera::GetUE3GlobalsSnapshot();
    if (!globals.gNamesValid || !globals.gObjectsValid) {
        Log("[InteractionAim] IFind2 unavailable: globals not discovered yet");
        return;
    }
    TArray64 names = {};
    TArray64 objects = {};
    if (!ReadMem(globals.gNamesAddress, &names, sizeof(names)) ||
        !ReadMem(globals.gObjectsAddress, &objects, sizeof(objects))) {
        Log("[InteractionAim] IFind2 unavailable: cannot read GNames/GObjects");
        return;
    }
    uintptr_t liveActors[128] = {};
    size_t liveCount = 0;
    uintptr_t hud = 0;
    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t object = 0;
        if (!ReadMem(objects.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                     &object, sizeof(object)) || !object) continue;
        char className[128] = {};
        if (!ReadClassName(globals, names, object, className, sizeof(className)))
            continue;
        if (strcmp(className, "WillowInteractiveObject") == 0) {
            char objectName[128] = {};
            ReadObjectName(globals, names, object, objectName, sizeof(objectName));
            if (strncmp(objectName, "Default__", 9) == 0) continue;
            if (liveCount < 128) liveActors[liveCount++] = object;
        } else if (strcmp(className, "WillowHUD") == 0 && hud == 0) {
            char objectName[128] = {};
            ReadObjectName(globals, names, object, objectName, sizeof(objectName));
            if (strncmp(objectName, "Default__", 9) != 0) hud = object;
        }
    }
    Log("[InteractionAim] IFind2: liveInteractives=%llu hud=%p",
        static_cast<unsigned long long>(liveCount),
        reinterpret_cast<void*>(hud));
    if (!liveCount) return;
    auto isLive = [&](uintptr_t v) {
        for (size_t a = 0; a < liveCount; ++a)
            if (v == liveActors[a]) return true;
        return false;
    };
    const uintptr_t localController = m_localController.load(std::memory_order_acquire);
    const uintptr_t localPawn = m_localPawn.load(std::memory_order_acquire);
    const uintptr_t bases[3] = {localController, localPawn, hud};
    const char* baseTags[3] = {"ctl", "pwn", "hud"};
    for (int pass = 0; pass < 3; ++pass) {
        const uintptr_t base = bases[pass];
        if (base < 0x10000) {
            Log("[InteractionAim] IFind2 %s: holder unavailable", baseTags[pass]);
            continue;
        }
        size_t hits = 0;
        for (uint32_t offset = 0; offset < 0x2000 && hits < 16; offset += 8) {
            uintptr_t arrayData = 0;
            if (!ReadMem(base + offset, &arrayData, sizeof(arrayData)) ||
                arrayData < 0x10000) continue;
            for (uint32_t i = 0; i < 256 && hits < 16; ++i) {
                uintptr_t slot = 0;
                if (!ReadMem(arrayData + static_cast<uint64_t>(i) * 8,
                             &slot, sizeof(slot))) break;
                if (slot >= 0x10000 && isLive(slot)) {
                    Log("[InteractionAim] IFind2 HIT %s+0x%X[%u] -> live actor %p",
                        baseTags[pass], offset, i,
                        reinterpret_cast<void*>(slot));
                    ++hits;
                }
            }
        }
        if (!hits)
            Log("[InteractionAim] IFind2 %s: no indirect live-actor references",
                baseTags[pass]);
    }
    Log("[InteractionAim] IFind2 complete");
}

void WeaponAimSystem::ScanTraceCalls() {
    // Live disassembly-lite: scan execTrace bytes for CALL rel32 targets to
    // find the underlying C++ trace routine (the exe is packed on disk, so
    // static disassembly is useless; live memory is unpacked).
    const uintptr_t target = static_cast<uintptr_t>(m_traceTarget);
    if (target < 0x10000) {
        Log("[InteractionAim] TraceCall unavailable: trace hook not installed");
        return;
    }
    unsigned char bytes[256] = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void*>(target),
                           bytes, sizeof(bytes), &read) ||
        read != sizeof(bytes)) {
        Log("[InteractionAim] TraceCall unavailable: cannot read execTrace");
        return;
    }
    // Log prologue bytes for manual decode.
    char hex[256 * 3 + 1] = {};
    for (size_t i = 0; i < 64; ++i)
        sprintf_s(hex + i * 3, sizeof(hex) - i * 3, "%02X ", bytes[i]);
    Log("[InteractionAim] TraceCall execTrace=%p bytes: %s",
        reinterpret_cast<void*>(target), hex);
    for (size_t i = 0; i + 5 <= sizeof(bytes); ++i) {
        if (bytes[i] != 0xE8) continue;
        int32_t rel = 0;
        memcpy(&rel, bytes + i + 1, sizeof(rel));
        const uintptr_t dest = target + i + 5 + static_cast<int64_t>(rel);
        unsigned char prologue[16] = {};
        SIZE_T preread = 0;
        const bool codeOk = ReadProcessMemory(
            GetCurrentProcess(), reinterpret_cast<const void*>(dest),
            prologue, sizeof(prologue), &preread) && preread == sizeof(prologue);
        char phex[16 * 3 + 1] = {};
        if (codeOk) {
            for (size_t b = 0; b < 16; ++b)
                sprintf_s(phex + b * 3, sizeof(phex) - b * 3, "%02X ", prologue[b]);
        }
        Log("[InteractionAim] TraceCall +0x%zX CALL -> %p prologue: %s",
            i, reinterpret_cast<void*>(dest), codeOk ? phex : "?");
    }
    Log("[InteractionAim] TraceCall complete");
}

void WeaponAimSystem::DumpCode(uintptr_t address, uint32_t size, const char* tag) {
    // Hex dump of live (unpacked) module code for offline disassembly. The exe
    // is SteamStub-packed on disk, so only runtime bytes can be analyzed.
    if (address < 0x10000 || size == 0) return;
    constexpr uint32_t kChunk = 64;
    unsigned char buffer[kChunk] = {};
    for (uint32_t offset = 0; offset < size; offset += kChunk) {
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                               reinterpret_cast<const void*>(address + offset),
                               buffer, kChunk, &read) ||
            read != kChunk) {
            Log("[LootRecon] %s +0x%X unreadable", tag, offset);
            return;
        }
        char hex[kChunk * 3 + 1] = {};
        for (uint32_t i = 0; i < kChunk; ++i)
            sprintf_s(hex + i * 3, sizeof(hex) - i * 3, "%02X ", buffer[i]);
        Log("[LootRecon] %s +0x%X: %s", tag, offset, hex);
    }
}

void WeaponAimSystem::DisasmAt(uintptr_t address, const char* tag) {
    // Live disassembly-lite at any address: prologue bytes + E8 CALL / E9
    // JMP targets. The exe is packed on disk; live memory is unpacked.
    if (address < 0x10000) {
        Log("[InteractionAim] Disasm %s: address too low", tag);
        return;
    }
    unsigned char bytes[256] = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void*>(address),
                           bytes, sizeof(bytes), &read) ||
        read != sizeof(bytes)) {
        Log("[InteractionAim] Disasm %s: cannot read %p", tag,
            reinterpret_cast<void*>(address));
        return;
    }
    char hex[256 * 3 + 1] = {};
    for (size_t i = 0; i < 64; ++i)
        sprintf_s(hex + i * 3, sizeof(hex) - i * 3, "%02X ", bytes[i]);
    Log("[InteractionAim] Disasm %s @ %p bytes: %s", tag,
        reinterpret_cast<void*>(address), hex);
    for (size_t i = 0; i + 5 <= sizeof(bytes); ++i) {
        if (bytes[i] != 0xE8 && bytes[i] != 0xE9) continue;
        int32_t rel = 0;
        memcpy(&rel, bytes + i + 1, sizeof(rel));
        const uintptr_t dest = address + i + 5 + static_cast<int64_t>(rel);
        unsigned char prologue[16] = {};
        SIZE_T preread = 0;
        const bool codeOk = ReadProcessMemory(
            GetCurrentProcess(), reinterpret_cast<const void*>(dest),
            prologue, sizeof(prologue), &preread) && preread == sizeof(prologue);
        char phex[16 * 3 + 1] = {};
        if (codeOk) {
            for (size_t b = 0; b < 16; ++b)
                sprintf_s(phex + b * 3, sizeof(phex) - b * 3, "%02X ", prologue[b]);
        }
        Log("[InteractionAim] Disasm %s +0x%zX %s -> %p prologue: %s",
            tag, i, bytes[i] == 0xE8 ? "CALL" : "JMP",
            reinterpret_cast<void*>(dest), codeOk ? phex : "?");
    }
    Log("[InteractionAim] Disasm %s complete", tag);
}

void WeaponAimSystem::DumpInteractionProperties() {
    // BL1E reflection layout from bl-sdk/unrealsdk:
    // UStruct::SuperField=0x78, PropertyLink=0xB0;
    // ZProperty::Offset_Internal=0x8C, PropertyLinkNext=0x90.
    const auto globals = camera::GetUE3GlobalsSnapshot();
    if (!globals.gNamesValid || !globals.gObjectsValid) {
        Log("[InteractionAim] IProps unavailable: globals not discovered yet");
        return;
    }
    TArray64 names = {};
    TArray64 objects = {};
    if (!ReadMem(globals.gNamesAddress, &names, sizeof(names)) ||
        !ReadMem(globals.gObjectsAddress, &objects, sizeof(objects))) {
        Log("[InteractionAim] IProps unavailable: cannot read GNames/GObjects");
        return;
    }

    const char* targets[] = {
        "WillowPlayerController", "PlayerInteractionManager",
        "PlayerInteractionClient", "WillowHUD", "InteractionProxy",
        "PawnInteractionProxy", "WillowInteractiveObject"
    };
    uintptr_t classes[_countof(targets)] = {};
    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t object = 0;
        char objectName[128] = {};
        char className[128] = {};
        if (!ReadMem(objects.data + static_cast<uint64_t>(index) * sizeof(uintptr_t),
                     &object, sizeof(object)) || !object ||
            !ReadObjectName(globals, names, object, objectName, sizeof(objectName)) ||
            !ReadClassName(globals, names, object, className, sizeof(className)) ||
            strcmp(className, "Class") != 0) continue;
        for (size_t target = 0; target < _countof(targets); ++target) {
            if (classes[target] == 0 && strcmp(objectName, targets[target]) == 0)
                classes[target] = object;
        }
    }

    auto containsRelevantWord = [](const char* name) {
        static const char* words[] = {
            "interact", "target", "use", "focus", "trace", "look",
            "crosshair", "touch", "pickup", "loot", "current", "client",
            "manager", "prompt"
        };
        char lower[128] = {};
        size_t length = strlen(name);
        if (length >= sizeof(lower)) length = sizeof(lower) - 1;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char ch = static_cast<unsigned char>(name[i]);
            lower[i] = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A'))
                                                : static_cast<char>(ch);
        }
        for (const char* word : words)
            if (strstr(lower, word) != nullptr) return true;
        return false;
    };

    for (size_t target = 0; target < _countof(targets); ++target) {
        uintptr_t currentClass = classes[target];
        if (currentClass < 0x10000) {
            Log("[InteractionAim] IProps class %s not found", targets[target]);
            continue;
        }
        Log("[InteractionAim] IProps class %s=%p", targets[target],
            reinterpret_cast<void*>(currentClass));
        for (int depth = 0; depth < 16 && currentClass >= 0x10000; ++depth) {
            char ownerName[128] = {};
            ReadObjectName(globals, names, currentClass, ownerName, sizeof(ownerName));
            uintptr_t property = 0;
            ReadMem(currentClass + 0xB0, &property, sizeof(property));
            size_t visited = 0;
            while (property >= 0x10000 && visited++ < 1024) {
                char propertyName[128] = {};
                char propertyClass[128] = {};
                int32_t offset = -1;
                int32_t arrayDim = 0;
                int32_t elementSize = 0;
                uintptr_t next = 0;
                const bool valid =
                    ReadObjectName(globals, names, property,
                                   propertyName, sizeof(propertyName)) &&
                    ReadClassName(globals, names, property,
                                  propertyClass, sizeof(propertyClass)) &&
                    ReadMem(property + 0x8C, &offset, sizeof(offset)) &&
                    ReadMem(property + 0x68, &arrayDim, sizeof(arrayDim)) &&
                    ReadMem(property + 0x6C, &elementSize, sizeof(elementSize));
                ReadMem(property + 0x90, &next, sizeof(next));
                const bool dumpAll = target == 1 || target == 2 ||
                    target == 4 || target == 5;
                if (valid && (dumpAll || containsRelevantWord(propertyName))) {
                    Log("[InteractionAim] IProps %s.%s type=%s offset=0x%X "
                        "element=%d array=%d prop=%p",
                        ownerName, propertyName, propertyClass, offset,
                        elementSize, arrayDim, reinterpret_cast<void*>(property));
                }
                if (next == property) break;
                property = next;
            }
            uintptr_t superClass = 0;
            if (!ReadMem(currentClass + 0x78, &superClass, sizeof(superClass)) ||
                superClass == currentClass) break;
            currentClass = superClass;
        }
    }
    Log("[InteractionAim] IProps complete");
}

void WeaponAimSystem::Shutdown() {
    ShutdownFocusGuard();
    InvalidateDirection();
    m_aimUpdatedMs.store(0, std::memory_order_release);
    m_fireActive.store(false, std::memory_order_release);
    m_vehicleSecondaryFireActive.store(false, std::memory_order_release);
    m_playerInjured.store(false, std::memory_order_release);
    m_phaseWalkActive.store(false, std::memory_order_release);
    SetBallisticOverrideEnabled(false);
    if (m_nativeAimTarget) {
        MH_DisableHook(reinterpret_cast<void*>(m_nativeAimTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_nativeAimTarget));
        m_nativeAimTarget = 0;
        m_originalGetAimRotation = nullptr;
    }
    if (m_scriptInvokeTarget &&
        m_scriptInvokeInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(reinterpret_cast<void*>(m_scriptInvokeTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_scriptInvokeTarget));
    }
    m_scriptInvokeTarget = 0;
    m_originalScriptInvoke = nullptr;
    if (m_tickTargetsTarget &&
        m_interactionHookInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(reinterpret_cast<void*>(m_tickTargetsTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_tickTargetsTarget));
    }
    m_tickTargetsTarget = 0;
    m_originalTickTargets = nullptr;
    m_tickTargetsFunction.store(0, std::memory_order_release);
    if (m_traceTarget &&
        m_traceHookInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(reinterpret_cast<void*>(m_traceTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_traceTarget));
    }
    m_traceTarget = 0;
    m_originalTrace = nullptr;
    m_traceFunction.store(0, std::memory_order_release);
    if (m_traceActorsTarget &&
        m_traceActorsInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(reinterpret_cast<void*>(m_traceActorsTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_traceActorsTarget));
    }
    if (m_fastTraceTarget &&
        m_fastTraceInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(reinterpret_cast<void*>(m_fastTraceTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_fastTraceTarget));
    }
    if (m_viewPointTarget &&
        m_viewPointInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(reinterpret_cast<void*>(m_viewPointTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_viewPointTarget));
    }
    m_traceActorsTarget = m_fastTraceTarget = m_viewPointTarget = 0;
    m_originalTraceActors = m_originalFastTrace = m_originalViewPoint = nullptr;
    m_traceActorsFunction.store(0, std::memory_order_release);
    m_fastTraceFunction.store(0, std::memory_order_release);
    m_viewPointFunction.store(0, std::memory_order_release);
    m_allowUseEventFunction.store(0, std::memory_order_release);
    if (m_isInjuredTarget) {
        MH_DisableHook(reinterpret_cast<void*>(m_isInjuredTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_isInjuredTarget));
    }
    m_isInjuredTarget = 0;
    m_originalIsInjured = nullptr;
    if (m_phaseWalkVisibilityTarget) {
        MH_DisableHook(reinterpret_cast<void*>(m_phaseWalkVisibilityTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_phaseWalkVisibilityTarget));
    }
    m_phaseWalkVisibilityTarget = 0;
    m_originalPhaseWalkVisibility = nullptr;
    m_meleeHooksStopping.store(true, std::memory_order_release);
    if (m_findMeleeTargetTarget &&
        m_findMeleeTargetInstalled.load(std::memory_order_acquire))
        MH_DisableHook(reinterpret_cast<void*>(m_findMeleeTargetTarget));
    if (m_meleeAttackTarget &&
        m_meleeAttackInstalled.load(std::memory_order_acquire))
        MH_DisableHook(reinterpret_cast<void*>(m_meleeAttackTarget));
    while (m_inFlightMeleeHooks.load(std::memory_order_acquire) != 0) Sleep(1);
    if (m_findMeleeTargetTarget &&
        m_findMeleeTargetInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_RemoveHook(reinterpret_cast<void*>(m_findMeleeTargetTarget));
    }
    m_findMeleeTargetFunction = 0;
    m_findMeleeTargetTarget = 0;
    m_originalFindMeleeTarget = nullptr;
    m_maxLungeDistanceParameterOffset = -1;
    if (m_meleeAttackTarget &&
        m_meleeAttackInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_RemoveHook(reinterpret_cast<void*>(m_meleeAttackTarget));
    }
    m_meleeAttackFunction = 0;
    m_meleeAttackTarget = 0;
    m_originalMeleeAttack = nullptr;
    m_meleeDefinitionClass = 0;
    m_meleeObjectClassOffset = -1;
    m_meleeContextParameterOffset = -1;
    m_traceScaleOffset = -1;
    m_radiusScaleOffset = -1;
    m_meleeExecutionThread.store(0, std::memory_order_release);
    if (m_processEventTarget && m_hookInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(reinterpret_cast<void*>(m_processEventTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_processEventTarget));
    }
    m_processEventTarget = 0;
    m_originalProcessEvent = nullptr;
    if (m_callFunctionTarget &&
        m_callFunctionInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(reinterpret_cast<void*>(m_callFunctionTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_callFunctionTarget));
    }
    m_callFunctionTarget = 0;
    m_originalCallFunction = nullptr;
    if (m_usableSelectorTarget &&
        m_usableSelectorInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(reinterpret_cast<void*>(m_usableSelectorTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_usableSelectorTarget));
    }
    m_usableSelectorTarget = 0;
    m_originalUsableSelector = nullptr;
    AcquireSRWLockExclusive(&m_identityLock);
    m_getAdjustedAimName.store(0, std::memory_order_release);
    m_isInjuredName.store(0, std::memory_order_release);
    m_phaseWalkVisibilityName.store(0, std::memory_order_release);
    m_getAdjustedAimFunction.store(0, std::memory_order_release);
    m_getAdjustedAimOwnerClass.store(0, std::memory_order_release);
    m_localController.store(0, std::memory_order_release);
    m_localPawn.store(0, std::memory_order_release);
    m_localWeapon.store(0, std::memory_order_release);
    m_pawnIdentityValid.store(false, std::memory_order_release);
    m_weaponIdentityValid.store(false, std::memory_order_release);
    m_pawnPropertyOffset = -1;
    m_controllerPropertyOffset = -1;
    m_weaponPropertyOffset = -1;
    m_ownerPropertyOffset = -1;
    m_identityGeneration.fetch_add(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&m_identityLock);
    m_initialized.store(false, std::memory_order_release);
}

}} // namespace bl1gotyvr::input
