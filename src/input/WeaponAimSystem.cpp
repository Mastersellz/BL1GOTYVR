#include "WeaponAimSystem.hpp"

#include "../camera/UE3Scanner.hpp"
#include "../camera/CameraHook.hpp"
#include "../core/VRMod.hpp"
#include "../hook/MinHookWrapper.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "psapi.lib")

namespace bl1gotyvr { namespace input {

namespace {

constexpr int32_t kRotUnisPerTurn = 65536;
constexpr float kRadiansToUnis = 65536.0f / 6.2831853071795864769f;
constexpr uintptr_t kProcessEventRva = 0x01CB460;
constexpr size_t kProcessEventVtableIndex = 67;

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
    uintptr_t vtable = 0;
    if (!ReadMem(controllerAddress, &vtable, sizeof(vtable)) || vtable < 0x10000)
        return 0;

    const uintptr_t expected = static_cast<uintptr_t>(moduleBase) + kProcessEventRva;
    uintptr_t candidate = 0;
    if (!ReadMem(vtable + kProcessEventVtableIndex * sizeof(uintptr_t),
                 &candidate, sizeof(candidate)) || candidate != expected ||
        candidate < moduleBase || candidate >= moduleBase + moduleSize) {
        Log("[WeaponAim] ProcessEvent validation failed: vtable[%zu]=%p expected=%p",
            kProcessEventVtableIndex, reinterpret_cast<void*>(candidate),
            reinterpret_cast<void*>(expected));
        return 0;
    }

    static constexpr unsigned char expectedPrologue[] = {
        0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x41, 0x54,
        0x41, 0x55, 0x41, 0x56, 0x41, 0x57
    };
    unsigned char observed[sizeof(expectedPrologue)] = {};
    if (!ReadMem(candidate, observed, sizeof(observed)) ||
        memcmp(observed, expectedPrologue, sizeof(observed)) != 0) {
        Log("[WeaponAim] ProcessEvent prologue mismatch at %p", reinterpret_cast<void*>(candidate));
        return 0;
    }

    Log("[WeaponAim] ProcessEvent validated at vtable[%zu]: %p (RVA 0x%llX)",
        kProcessEventVtableIndex, reinterpret_cast<void*>(candidate),
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

void __fastcall WeaponAimSystem::HookedGetAimRotation(void* object, void* frame,
                                                       void* result) {
    auto& system = Instance();
    if (system.m_originalGetAimRotation)
        system.m_originalGetAimRotation(object, frame, result);

    const uint64_t count = system.m_nativeAimCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    const bool firing = system.m_fireActive.load(std::memory_order_acquire);
    const bool periodicSample = firing ? (count <= 16 || count % 120 == 0) :
        (count <= 8 || count % 600 == 0);
    if (!result || !periodicSample) return;

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
        ReadDirect(reinterpret_cast<uintptr_t>(frame) + 0x14,
                   &function, sizeof(function));
    }
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
    if (overrideEnabled && firing && desiredValid && resultReadable) {
        written = WriteDirect(reinterpret_cast<uintptr_t>(result), desired, sizeof(desired));
    }
    ReleaseSRWLockShared(&system.m_ballisticOverrideLock);
    if (written) system.m_overrideCount.fetch_add(1, std::memory_order_relaxed);
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

void WeaponAimSystem::Discover(const void* globalsAddress, uint64_t controllerAddress,
                                 uint64_t moduleBase, uint32_t moduleSize) {
    const auto& globals = *static_cast<const camera::UE3Globals*>(globalsAddress);
    if (!globals.gNamesValid || !globals.gObjectsValid || controllerAddress < 0x10000)
        return;

    TArray64 names = {};
    TArray64 objects = {};
    if (!ReadMem(globals.gNamesAddress, &names, sizeof(names)) ||
        !ReadMem(globals.gObjectsAddress, &objects, sizeof(objects)))
        return;

    struct AimFunctionCandidate {
        uintptr_t function = 0;
        uintptr_t owner = 0;
        uint64_t nameToken = 0;
    };
    AimFunctionCandidate aimCandidates[128] = {};
    size_t aimCandidateCount = 0;
    size_t totalAimCandidateCount = 0;
    uintptr_t getAdjustedAim = 0;
    uint64_t aimNameToken = 0;

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
    if (weaponValid && !aimCandidatesTruncated) {
        for (size_t index = 0; index < aimCandidateCount; ++index) {
            const int distance = ClassDistance(weaponClass, aimCandidates[index].owner);
            if (distance < 0 || distance > bestAimOwnerDistance) continue;
            if (distance < bestAimOwnerDistance) {
                bestAimOwnerDistance = distance;
                getAdjustedAim = aimCandidates[index].function;
                aimOwner = aimCandidates[index].owner;
                aimNameToken = aimCandidates[index].nameToken;
                aimSelectionAmbiguous = false;
            } else if (aimCandidates[index].function != getAdjustedAim) {
                aimSelectionAmbiguous = true;
            }
        }
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
        m_localController.load(std::memory_order_acquire) !=
            (pawnValid ? controllerAddress : 0) ||
        m_localPawn.load(std::memory_order_acquire) != (pawnValid ? pawn : 0) ||
        m_localWeapon.load(std::memory_order_acquire) != (weaponValid ? weapon : 0);
    m_getAdjustedAimName.store(aimNameToken, std::memory_order_release);
    m_getAdjustedAimFunction.store(getAdjustedAim, std::memory_order_release);
    m_getAdjustedAimOwnerClass.store(aimOwner, std::memory_order_release);
    m_localController.store(pawnValid ? controllerAddress : 0, std::memory_order_release);
    m_localPawn.store(pawnValid ? pawn : 0, std::memory_order_release);
    m_localWeapon.store(weaponValid ? weapon : 0, std::memory_order_release);
    m_pawnIdentityValid.store(pawnValid, std::memory_order_release);
    m_weaponIdentityValid.store(weaponValid, std::memory_order_release);
    m_pawnPropertyOffset = pawnValid ? pawnOffset : -1;
    m_controllerPropertyOffset = pawnValid ? controllerOffset : -1;
    m_weaponPropertyOffset = weaponOffsetsValid ? weaponOffset : -1;
    m_ownerPropertyOffset = weaponOffsetsValid ? ownerOffset : -1;
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

    InstallNativeAimProbe(moduleBase, moduleSize);
    if (getAdjustedAim && weaponValid) {
        InstallScriptInvokeProbe(getAdjustedAim, moduleBase, moduleSize);
    }
    Log("[WeaponAim] Ballistic path: script=%d override=%d aimFunction=%p",
        m_scriptInvokeInstalled.load(std::memory_order_acquire),
        m_ballisticOverrideEnabled.load(std::memory_order_acquire),
        reinterpret_cast<void*>(getAdjustedAim));
    m_initialized.store(true, std::memory_order_release);
}

int32_t __fastcall WeaponAimSystem::HookedProcessEvent(
        void* object, uint64_t functionName, void* params, void* result) {
    auto& system = Instance();
    const bool isLocalAdjustedAim =
        functionName != 0 &&
        functionName == system.m_getAdjustedAimName.load(std::memory_order_acquire) &&
        system.m_weaponIdentityValid.load(std::memory_order_acquire) &&
        (reinterpret_cast<uintptr_t>(object) ==
            system.m_localWeapon.load(std::memory_order_acquire) ||
         system.m_vehicleSecondaryFireActive.load(std::memory_order_acquire));

    const int32_t status = system.m_originalProcessEvent ?
        system.m_originalProcessEvent(object, functionName, params, result) : 0;

    if (!isLocalAdjustedAim) return status;

    const uint64_t count = system.m_processEventAimCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    const bool firing = system.m_fireActive.load(std::memory_order_acquire);
    const bool periodicSample = firing ? (count <= 8 || count % 120 == 0) :
        (count <= 8 || count % 600 == 0);

    float origin[3] = {};
    int32_t rotation[3] = {};
    const bool originReadable = params && ReadMem(
        reinterpret_cast<uintptr_t>(params), origin, sizeof(origin));
    const bool resultReadable = result && ReadMem(
        reinterpret_cast<uintptr_t>(result), rotation, sizeof(rotation));
    if (!periodicSample) return status;
    Log("[WeaponAim] ProcessEvent GetAdjustedAim probe call=%llu firing=%d "
        "object=%p name=0x%llX params=%p originReadable=%d origin=(%.3f,%.3f,%.3f) "
        "result=%p resultReadable=%d rot=(%d,%d,%d)",
        static_cast<unsigned long long>(count), firing, object,
        static_cast<unsigned long long>(functionName), params, originReadable,
        origin[0], origin[1], origin[2], result, resultReadable,
        rotation[0], rotation[1], rotation[2]);
    return status;
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
    if (pawnOffset <= 0 || controllerOffset <= 0 || weaponOffset <= 0 || ownerOffset <= 0)
        return false;

    uintptr_t publishedPawn = 0;
    uintptr_t publishedController = 0;
    uintptr_t weapon = 0;
    uintptr_t owner = 0;
    if (!ReadMem(controller + static_cast<uintptr_t>(pawnOffset),
                 &publishedPawn, sizeof(publishedPawn)) || publishedPawn != pawn ||
        !ReadMem(pawn + static_cast<uintptr_t>(controllerOffset),
                 &publishedController, sizeof(publishedController)) ||
        publishedController != controller ||
        !ReadMem(pawn + static_cast<uintptr_t>(weaponOffset), &weapon, sizeof(weapon)) ||
        weapon < 0x10000 ||
        !ReadMem(weapon + static_cast<uintptr_t>(ownerOffset), &owner, sizeof(owner)) ||
        owner != pawn) return false;

    const camera::UE3Globals globals = camera::GetUE3GlobalsSnapshot();
    TArray64 names = {};
    uintptr_t weaponClass = 0;
    char weaponName[128] = {};
    char weaponClassName[128] = {};
    if (!globals.gNamesValid ||
        !ReadMem(globals.gNamesAddress, &names, sizeof(names)) ||
        !ValidateRuntimeObject(globals, names, weapon, "Weapon", weaponClass,
                               weaponName, sizeof(weaponName), weaponClassName,
                               sizeof(weaponClassName))) return false;

    bool changed = false;
    const uintptr_t selectedAimFunction =
        m_getAdjustedAimFunction.load(std::memory_order_acquire);
    const uintptr_t selectedAimOwner =
        m_getAdjustedAimOwnerClass.load(std::memory_order_acquire);
    const bool selectedAimCompatible = selectedAimFunction == 0 ||
        ClassDistance(weaponClass, selectedAimOwner) >= 0;
    AcquireSRWLockExclusive(&m_identityLock);
    changed = m_localController.load(std::memory_order_acquire) != controller ||
        m_localPawn.load(std::memory_order_acquire) != pawn ||
        m_localWeapon.load(std::memory_order_acquire) != weapon ||
        !m_pawnIdentityValid.load(std::memory_order_acquire) ||
        !m_weaponIdentityValid.load(std::memory_order_acquire);
    m_localController.store(controller, std::memory_order_release);
    m_localPawn.store(pawn, std::memory_order_release);
    m_localWeapon.store(weapon, std::memory_order_release);
    m_pawnIdentityValid.store(true, std::memory_order_release);
    m_weaponIdentityValid.store(true, std::memory_order_release);
    if (changed) m_identityGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (changed) m_scriptInvokeAimCalls.store(0, std::memory_order_release);
    if (!selectedAimCompatible) {
        m_getAdjustedAimName.store(0, std::memory_order_release);
        m_getAdjustedAimFunction.store(0, std::memory_order_release);
        m_getAdjustedAimOwnerClass.store(0, std::memory_order_release);
    }
    const uint64_t generation = m_identityGeneration.load(std::memory_order_acquire);
    ReleaseSRWLockExclusive(&m_identityLock);
    Log("[WeaponAim] Fast foot identity restored: controller=%p pawn=%p weapon=%p(%s/%s) "
        "generation=%llu aimCompatible=%d", reinterpret_cast<void*>(controller),
        reinterpret_cast<void*>(pawn), reinterpret_cast<void*>(weapon), weaponName,
        weaponClassName, static_cast<unsigned long long>(generation),
        selectedAimCompatible);
    return true;
}

void WeaponAimSystem::Shutdown() {
    InvalidateDirection();
    m_aimUpdatedMs.store(0, std::memory_order_release);
    m_fireActive.store(false, std::memory_order_release);
    m_vehicleSecondaryFireActive.store(false, std::memory_order_release);
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
    if (m_processEventTarget && m_hookInstalled.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(reinterpret_cast<void*>(m_processEventTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_processEventTarget));
    }
    m_processEventTarget = 0;
    m_originalProcessEvent = nullptr;
    AcquireSRWLockExclusive(&m_identityLock);
    m_getAdjustedAimName.store(0, std::memory_order_release);
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
