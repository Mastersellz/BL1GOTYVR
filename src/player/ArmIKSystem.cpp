#include "ArmIKSystem.hpp"

#include "TwoBoneIK.hpp"
#include "../camera/CameraHook.hpp"
#include "../camera/UE3Scanner.hpp"
#include "../config/Config.hpp"
#include "../core/VRMod.hpp"
#include "../input/InputHook.hpp"
#include "../input/WeaponAimSystem.hpp"
#include "../input/XRInput.hpp"
#include "../hook/MinHookWrapper.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace bl1gotyvr::player {

namespace {

struct TArray64 {
    uintptr_t data = 0;
    int32_t count = 0;
    int32_t capacity = 0;
};

struct BoneTransform {
    Quat rotation;
    Vec3 position;
    float padding = 0.0f;
};

struct BoneMatrix {
    float values[16] = {};
};

bool ValidateMatrix(const float matrix[16]);

bool ReadMemory(uintptr_t address, void* output, size_t size) {
    SIZE_T read = 0;
    return address >= 0x10000 &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                          output, size, &read) && read == size;
}

bool WriteMemory(uintptr_t address, const void* input, size_t size) {
    SIZE_T written = 0;
    return address >= 0x10000 &&
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                           input, size, &written) && written == size;
}

bool ReadAtomicWord(uintptr_t address, uint32_t& value) {
    if (address < 0x10000 || (address & 3) != 0) return false;
    __try {
        value = static_cast<uint32_t>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(address), 0, 0));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool UpdateAtomicBit(uintptr_t address, uint32_t mask, bool enabled,
                     uint32_t& before, uint32_t& after) {
    if (!mask || address < 0x10000 || (address & 3) != 0) return false;
    __try {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const LONG current = InterlockedCompareExchange(
                reinterpret_cast<volatile LONG*>(address), 0, 0);
            const LONG desired = enabled ?
                static_cast<LONG>(static_cast<uint32_t>(current) | mask) :
                static_cast<LONG>(static_cast<uint32_t>(current) & ~mask);
            before = static_cast<uint32_t>(current);
            if (desired == current) {
                after = static_cast<uint32_t>(current);
                return true;
            }
            const LONG observed = InterlockedCompareExchange(
                reinterpret_cast<volatile LONG*>(address), desired, current);
            if (observed == current) {
                after = static_cast<uint32_t>(desired);
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

uint64_t HashPose(uintptr_t data, int count, int stride) {
    uint64_t hash = 1469598103934665603ull;
    const int samples = (std::min)(count, 16);
    for (int sample = 0; sample < samples; ++sample) {
        const int index = sample * (count - 1) / (std::max)(1, samples - 1);
        unsigned char bytes[0x40] = {};
        if (!ReadMemory(data + static_cast<uintptr_t>(index) * stride,
                        bytes, sizeof(bytes))) return 0;
        for (unsigned char value : bytes) {
            hash ^= value;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

bool ReadAscii(uintptr_t address, char* output, size_t capacity) {
    if (!output || capacity < 2) return false;
    for (size_t i = 0; i < capacity; ++i) {
        if (!ReadMemory(address + i, output + i, 1)) return false;
        if (output[i] == '\0') return i > 0;
        if (output[i] < 0x20 || output[i] > 0x7e) return false;
    }
    output[capacity - 1] = '\0';
    return false;
}

bool ReadName(const camera::UE3Globals& globals, int32_t index,
              char* output, size_t capacity) {
    TArray64 names = {};
    uintptr_t entry = 0;
    return globals.gNamesValid && index >= 0 &&
        ReadMemory(globals.gNamesAddress, &names, sizeof(names)) &&
        index < names.count &&
        ReadMemory(names.data + static_cast<uintptr_t>(index) * sizeof(uintptr_t),
                   &entry, sizeof(entry)) && entry &&
        ReadAscii(entry + globals.gNameStringOffset, output, capacity);
}

bool ReadObjectName(const camera::UE3Globals& globals, uintptr_t object,
                    char* output, size_t capacity) {
    int32_t index = -1;
    return ReadMemory(object + globals.gObjectNameOffset, &index, sizeof(index)) &&
        ReadName(globals, index, output, capacity);
}

bool ReadClassName(const camera::UE3Globals& globals, uintptr_t object,
                   char* output, size_t capacity) {
    uintptr_t classObject = 0;
    return ReadMemory(object + globals.gObjectClassOffset, &classObject,
                      sizeof(classObject)) && classObject &&
        ReadObjectName(globals, classObject, output, capacity);
}

std::string Lower(const char* input) {
    std::string result = input ? input : "";
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return result;
}

bool OuterChainContains(const camera::UE3Globals& globals, uintptr_t object,
                        uintptr_t target) {
    if (!target || globals.gObjectNameOffset < 8) return false;
    uintptr_t current = object;
    for (int depth = 0; depth < 16 && current >= 0x10000; ++depth) {
        uintptr_t outer = 0;
        if (!ReadMemory(current + globals.gObjectNameOffset - 8, &outer, sizeof(outer)) ||
            !outer || outer == current) return false;
        if (outer == target) return true;
        current = outer;
    }
    return false;
}

bool ValidateVisibilityTarget(const camera::UE3Globals& globals,
                              const ComponentInventoryEntry& entry,
                              uintptr_t pawn, uintptr_t weapon) {
    uintptr_t currentOuter = 0;
    uintptr_t currentClass = 0;
    uintptr_t currentMesh = 0;
    uint64_t currentNameToken = 0;
    char objectName[64] = {};
    char className[64] = {};
    char meshName[64] = {};
    return entry.component && entry.outer && entry.classObject && entry.skeletalMesh &&
        entry.skeletalMeshOffset > 0 && entry.exactPawnOuter && !entry.exactWeaponOuter &&
        ReadMemory(entry.component + globals.gObjectNameOffset - 8,
                   &currentOuter, sizeof(currentOuter)) && currentOuter == entry.outer &&
        ReadMemory(entry.component + globals.gObjectClassOffset,
                   &currentClass, sizeof(currentClass)) && currentClass == entry.classObject &&
        ReadMemory(entry.component + globals.gObjectNameOffset,
                   &currentNameToken, sizeof(currentNameToken)) &&
        currentNameToken == entry.objectNameToken &&
        ReadMemory(entry.component + entry.skeletalMeshOffset,
                   &currentMesh, sizeof(currentMesh)) && currentMesh == entry.skeletalMesh &&
        ReadObjectName(globals, entry.component, objectName, sizeof(objectName)) &&
        strcmp(objectName, entry.objectName) == 0 &&
        ReadClassName(globals, entry.component, className, sizeof(className)) &&
        strcmp(className, entry.className) == 0 &&
        ReadObjectName(globals, currentMesh, meshName, sizeof(meshName)) &&
        strcmp(meshName, entry.meshName) == 0 &&
        OuterChainContains(globals, entry.component, pawn) &&
        !OuterChainContains(globals, entry.component, weapon);
}

const char* RoleName(ComponentRole role) {
    switch (role) {
    case ComponentRole::ProtectedWeapon: return "protected_weapon";
    case ComponentRole::PawnBody: return "pawn_body";
    case ComponentRole::ProbableFirstPersonArms: return "probable_first_person_arms";
    default: return "unknown";
    }
}

int ClassDistance(uintptr_t derivedClass, uintptr_t targetClass) {
    uintptr_t current = derivedClass;
    for (int depth = 0; depth < 64 && current >= 0x10000; ++depth) {
        if (current == targetClass) return depth;
        uintptr_t superClass = 0;
        if (!ReadMemory(current + 0x78, &superClass, sizeof(superClass)) ||
            superClass == current) break;
        current = superClass;
    }
    return -1;
}

int VisibilityPropertyKind(const char* name) {
    const std::string lower = Lower(name);
    if (lower == "hidden" || lower == "bhidden") return 0;
    if (lower == "hiddengame" || lower == "bhiddengame") return 1;
    if (lower == "ownernosee" || lower == "bownernosee") return 2;
    if (lower == "onlyownersee" || lower == "bonlyownersee") return 3;
    return -1;
}

const char* VisibilityPropertyKindName(int kind) {
    static constexpr const char* names[] = {
        "Hidden", "HiddenGame", "OwnerNoSee", "OnlyOwnerSee"
    };
    return kind >= 0 && kind < 4 ? names[kind] : "Unknown";
}

bool IsSingleBit(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool LogVisibilityPropertyInventory(const camera::UE3Globals& globals,
                                    const ComponentInventoryStatus& inventory,
                                    int32_t& hiddenGameOffset,
                                    uint32_t& hiddenGameMask) {
    hiddenGameOffset = -1;
    hiddenGameMask = 0;
    struct Candidate {
        uintptr_t property = 0;
        uintptr_t ownerClass = 0;
        int kind = -1;
        int32_t arrayDim = 0;
        int32_t elementSize = 0;
        int32_t valueOffset = -1;
        uint32_t metadata[16] = {};
        char propertyName[64] = {};
        char ownerName[64] = {};
    };

    TArray64 objects = {};
    if (!ReadMemory(globals.gObjectsAddress, &objects, sizeof(objects)) ||
        !objects.data || objects.count <= 0 || objects.count > objects.capacity ||
        objects.count >= 2000000) {
        Log("[VisibilityProperty] GObjects unavailable for read-only property discovery");
        return false;
    }

    std::vector<Candidate> candidates;
    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t property = 0;
        char propertyName[128] = {};
        char className[128] = {};
        if (!ReadMemory(objects.data + static_cast<uintptr_t>(index) * sizeof(uintptr_t),
                        &property, sizeof(property)) || !property ||
            !ReadObjectName(globals, property, propertyName, sizeof(propertyName)) ||
            VisibilityPropertyKind(propertyName) < 0 ||
            !ReadClassName(globals, property, className, sizeof(className)) ||
            strcmp(className, "BoolProperty") != 0) continue;

        Candidate candidate;
        candidate.property = property;
        candidate.kind = VisibilityPropertyKind(propertyName);
        strcpy_s(candidate.propertyName, propertyName);
        if (!ReadMemory(property + globals.gObjectNameOffset - 8,
                        &candidate.ownerClass, sizeof(candidate.ownerClass)) ||
            !candidate.ownerClass ||
            !ReadObjectName(globals, candidate.ownerClass,
                            candidate.ownerName, sizeof(candidate.ownerName)) ||
            !ReadMemory(property + 0x68, &candidate.arrayDim, sizeof(candidate.arrayDim)) ||
            !ReadMemory(property + 0x6C, &candidate.elementSize,
                        sizeof(candidate.elementSize)) ||
            !ReadMemory(property + 0x8C, &candidate.valueOffset,
                        sizeof(candidate.valueOffset)) ||
            candidate.arrayDim != 1 || candidate.elementSize <= 0 ||
            candidate.elementSize > 8 || candidate.valueOffset < 0x40 ||
            candidate.valueOffset >= 0x10000 ||
            !ReadMemory(property + 0x90, candidate.metadata, sizeof(candidate.metadata)))
            continue;
        candidates.push_back(candidate);
    }

    Log("[VisibilityProperty] candidates=%zu mode=schema-discovery", candidates.size());
    int hiddenGameSchemas = 0;
    int ownerNoSeeSchemas = 0;
    int onlyOwnerSeeSchemas = 0;
    for (const Candidate& candidate : candidates) {
        char masks[256] = {};
        size_t used = 0;
        for (size_t index = 0; index < std::size(candidate.metadata); ++index) {
            if (!IsSingleBit(candidate.metadata[index])) continue;
            const int written = snprintf(masks + used, sizeof(masks) - used,
                "%s+0x%X=0x%08X", used ? "," : "",
                0x90 + static_cast<unsigned int>(index * sizeof(uint32_t)),
                candidate.metadata[index]);
            if (written <= 0 || static_cast<size_t>(written) >= sizeof(masks) - used) break;
            used += static_cast<size_t>(written);
        }
        Log("[VisibilityProperty] schema=%s reflected=%s owner=%s property=%p "
            "valueOffset=+0x%X element=%d metadata90=%08X/%08X/%08X/%08X "
            "metadataA0=%08X/%08X/%08X/%08X bitCandidates=%s",
            VisibilityPropertyKindName(candidate.kind), candidate.propertyName,
            candidate.ownerName, reinterpret_cast<void*>(candidate.property),
            candidate.valueOffset, candidate.elementSize,
            candidate.metadata[0], candidate.metadata[1], candidate.metadata[2],
            candidate.metadata[3], candidate.metadata[4], candidate.metadata[5],
            candidate.metadata[6], candidate.metadata[7], used ? masks : "none");
        if (candidate.kind == 1 && strcmp(candidate.ownerName, "PrimitiveComponent") == 0 &&
            candidate.elementSize == 4 && IsSingleBit(candidate.metadata[10])) {
            hiddenGameOffset = candidate.valueOffset;
            hiddenGameMask = candidate.metadata[10];
            ++hiddenGameSchemas;
        }
        if (candidate.kind == 2 && strcmp(candidate.ownerName, "PrimitiveComponent") == 0 &&
            candidate.valueOffset == 0x1AC && candidate.elementSize == 4 &&
            candidate.metadata[10] == 0x10) ++ownerNoSeeSchemas;
        if (candidate.kind == 3 && strcmp(candidate.ownerName, "PrimitiveComponent") == 0 &&
            candidate.valueOffset == 0x1AC && candidate.elementSize == 4 &&
            candidate.metadata[10] == 0x20) ++onlyOwnerSeeSchemas;
    }

    const input::PlayerIdentitySnapshot currentIdentity =
        input::WeaponAimSystem::Instance().GetPlayerIdentity();
    if (currentIdentity.controller != inventory.controller ||
        currentIdentity.pawn != inventory.pawn ||
        currentIdentity.weapon != inventory.weapon ||
        currentIdentity.pawnValid != inventory.pawnIdentityValid ||
        currentIdentity.weaponValid != inventory.weaponIdentityValid) {
        Log("[VisibilityProperty] Component value dump discarded: player identity changed");
        return false;
    }

    for (size_t entryIndex = 0; entryIndex < inventory.count; ++entryIndex) {
        const ComponentInventoryEntry& entry = inventory.entries[entryIndex];
        if (entry.role == ComponentRole::Unknown || !entry.component) continue;
        uintptr_t componentClass = 0;
        char currentObjectName[64] = {};
        char currentClassName[64] = {};
        if (!ReadMemory(entry.component + globals.gObjectClassOffset,
                        &componentClass, sizeof(componentClass)) || !componentClass ||
            !ReadObjectName(globals, entry.component,
                            currentObjectName, sizeof(currentObjectName)) ||
            strcmp(currentObjectName, entry.objectName) != 0 ||
            !ReadClassName(globals, entry.component,
                           currentClassName, sizeof(currentClassName)) ||
            strcmp(currentClassName, entry.className) != 0) continue;
        const bool expectedOuter = entry.role == ComponentRole::ProtectedWeapon ?
            (inventory.weaponIdentityValid &&
             OuterChainContains(globals, entry.component, inventory.weapon)) :
            (inventory.pawnIdentityValid &&
             OuterChainContains(globals, entry.component, inventory.pawn));
        if (!expectedOuter) continue;

        for (int kind = 0; kind < 4; ++kind) {
            const Candidate* best = nullptr;
            int bestDistance = 65;
            bool ambiguous = false;
            for (const Candidate& candidate : candidates) {
                if (candidate.kind != kind) continue;
                const int distance = ClassDistance(componentClass, candidate.ownerClass);
                if (distance < 0 || distance > bestDistance) continue;
                if (distance < bestDistance) {
                    best = &candidate;
                    bestDistance = distance;
                    ambiguous = false;
                } else if (!best || best->property != candidate.property) {
                    ambiguous = true;
                }
            }
            if (!best || ambiguous) continue;
            uint64_t currentValue = 0;
            const size_t readSize = (std::min)(sizeof(currentValue),
                static_cast<size_t>((std::max)(best->elementSize, 4)));
            if (!ReadMemory(entry.component + best->valueOffset,
                            &currentValue, readSize)) continue;
            Log("[VisibilityProperty] role=%s component=%p class=%s object=%s "
                "property=%s owner=%s valueAddress=%p current=0x%016llX readBytes=%zu",
                RoleName(entry.role), reinterpret_cast<void*>(entry.component),
                entry.className, entry.objectName, VisibilityPropertyKindName(kind),
                best->ownerName,
                reinterpret_cast<void*>(entry.component + best->valueOffset),
                static_cast<unsigned long long>(currentValue), readSize);
        }
    }
    const bool schemaValid = hiddenGameSchemas == 1 && ownerNoSeeSchemas == 1 &&
        onlyOwnerSeeSchemas == 1 && hiddenGameOffset == 0x1AC && hiddenGameMask == 0x4;
    Log("[VisibilityProperty] HiddenGame write schema validated=%d offset=+0x%X mask=0x%08X",
        schemaValid, hiddenGameOffset, hiddenGameMask);
    return schemaValid;
}

bool IsFiniteTransform(const BoneTransform& transform) {
    const float norm = transform.rotation.x * transform.rotation.x +
        transform.rotation.y * transform.rotation.y +
        transform.rotation.z * transform.rotation.z +
        transform.rotation.w * transform.rotation.w;
    return std::isfinite(norm) && norm > 0.5f && norm < 1.5f &&
        std::isfinite(transform.position.x) &&
        std::isfinite(transform.position.y) &&
        std::isfinite(transform.position.z) &&
        std::fabs(transform.position.x) < 100000.0f &&
        std::fabs(transform.position.y) < 100000.0f &&
        std::fabs(transform.position.z) < 100000.0f;
}

bool ValidatePoseArray(const TArray64& array, float& averageTranslation) {
    if (array.count < 6 || array.count > 256 || array.capacity < array.count ||
        array.capacity > 1024 || !array.data) return false;
    const int samples = (std::min)(array.count, 24);
    float total = 0.0f;
    for (int sample = 0; sample < samples; ++sample) {
        const int index = sample * (array.count - 1) / (std::max)(1, samples - 1);
        BoneTransform transform = {};
        if (!ReadMemory(array.data + static_cast<uintptr_t>(index) * 0x20,
                        &transform, sizeof(transform)) || !IsFiniteTransform(transform))
            return false;
        total += transform.position.length();
    }
    averageTranslation = total / samples;
    return true;
}

bool ValidateMatrixPoseArray(const TArray64& array, float& averageTranslation) {
    if (array.count < 6 || array.count > 256 || array.capacity < array.count ||
        array.capacity > 1024 || !array.data) return false;
    const int samples = (std::min)(array.count, 24);
    float total = 0.0f;
    for (int sample = 0; sample < samples; ++sample) {
        const int index = sample * (array.count - 1) / (std::max)(1, samples - 1);
        BoneMatrix matrix = {};
        if (!ReadMemory(array.data + static_cast<uintptr_t>(index) * 0x40,
                        &matrix, sizeof(matrix)) || !ValidateMatrix(matrix.values))
            return false;
        const float x = matrix.values[12];
        const float y = matrix.values[13];
        const float z = matrix.values[14];
        total += std::sqrt(x*x + y*y + z*z);
    }
    averageTranslation = total / samples;
    return true;
}

bool ValidateMatrix(const float matrix[16]) {
    if (!matrix || !std::isfinite(matrix[15]) || std::fabs(matrix[15] - 1.0f) > 0.05f)
        return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(matrix[i]) || std::fabs(matrix[i]) > 10000000.0f) return false;
    const float x = std::sqrt(matrix[0]*matrix[0] + matrix[1]*matrix[1] + matrix[2]*matrix[2]);
    const float y = std::sqrt(matrix[4]*matrix[4] + matrix[5]*matrix[5] + matrix[6]*matrix[6]);
    const float z = std::sqrt(matrix[8]*matrix[8] + matrix[9]*matrix[9] + matrix[10]*matrix[10]);
    const float xy = matrix[0]*matrix[4] + matrix[1]*matrix[5] + matrix[2]*matrix[6];
    const float xz = matrix[0]*matrix[8] + matrix[1]*matrix[9] + matrix[2]*matrix[10];
    const float yz = matrix[4]*matrix[8] + matrix[5]*matrix[9] + matrix[6]*matrix[10];
    return x > 0.8f && x < 1.2f && y > 0.8f && y < 1.2f &&
        z > 0.8f && z < 1.2f && std::fabs(xy) < 0.05f &&
        std::fabs(xz) < 0.05f && std::fabs(yz) < 0.05f;
}

Quat MatrixRotation(const float matrix[16]) {
    const float m00 = matrix[0], m01 = matrix[4], m02 = matrix[8];
    const float m10 = matrix[1], m11 = matrix[5], m12 = matrix[9];
    const float m20 = matrix[2], m21 = matrix[6], m22 = matrix[10];
    const float trace = m00 + m11 + m22;
    Quat result;
    if (trace > 0.0f) {
        const float scale = std::sqrt(trace + 1.0f) * 2.0f;
        result = {(m21 - m12) / scale, (m02 - m20) / scale,
                  (m10 - m01) / scale, 0.25f * scale};
    } else if (m00 > m11 && m00 > m22) {
        const float scale = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        result = {0.25f * scale, (m01 + m10) / scale,
                  (m02 + m20) / scale, (m21 - m12) / scale};
    } else if (m11 > m22) {
        const float scale = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        result = {(m01 + m10) / scale, 0.25f * scale,
                  (m12 + m21) / scale, (m02 - m20) / scale};
    } else {
        const float scale = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        result = {(m02 + m20) / scale, (m12 + m21) / scale,
                  0.25f * scale, (m10 - m01) / scale};
    }
    return result.normalized();
}

void SetMatrixTransform(BoneMatrix& matrix, const BoneTransform& transform) {
    const float scaleX = std::sqrt(matrix.values[0]*matrix.values[0] +
        matrix.values[1]*matrix.values[1] + matrix.values[2]*matrix.values[2]);
    const float scaleY = std::sqrt(matrix.values[4]*matrix.values[4] +
        matrix.values[5]*matrix.values[5] + matrix.values[6]*matrix.values[6]);
    const float scaleZ = std::sqrt(matrix.values[8]*matrix.values[8] +
        matrix.values[9]*matrix.values[9] + matrix.values[10]*matrix.values[10]);
    const Quat rotation = transform.rotation.normalized();
    const float xx = rotation.x * rotation.x, yy = rotation.y * rotation.y;
    const float zz = rotation.z * rotation.z, xy = rotation.x * rotation.y;
    const float xz = rotation.x * rotation.z, yz = rotation.y * rotation.z;
    const float xw = rotation.x * rotation.w, yw = rotation.y * rotation.w;
    const float zw = rotation.z * rotation.w;
    matrix.values[0] = (1.0f - 2.0f * (yy + zz)) * scaleX;
    matrix.values[1] = (2.0f * (xy + zw)) * scaleX;
    matrix.values[2] = (2.0f * (xz - yw)) * scaleX;
    matrix.values[4] = (2.0f * (xy - zw)) * scaleY;
    matrix.values[5] = (1.0f - 2.0f * (xx + zz)) * scaleY;
    matrix.values[6] = (2.0f * (yz + xw)) * scaleY;
    matrix.values[8] = (2.0f * (xz + yw)) * scaleZ;
    matrix.values[9] = (2.0f * (yz - xw)) * scaleZ;
    matrix.values[10] = (1.0f - 2.0f * (xx + yy)) * scaleZ;
    matrix.values[12] = transform.position.x;
    matrix.values[13] = transform.position.y;
    matrix.values[14] = transform.position.z;
}

Quat FromTo(const Vec3& source, const Vec3& destination) {
    const Vec3 from = source.normalized();
    const Vec3 to = destination.normalized();
    const float cosine = std::clamp(from.x*to.x + from.y*to.y + from.z*to.z,
                                    -1.0f, 1.0f);
    if (cosine > 0.99999f) return {};
    Vec3 axis{from.y*to.z - from.z*to.y,
              from.z*to.x - from.x*to.z,
              from.x*to.y - from.y*to.x};
    if (axis.lengthSq() < 1.0e-8f) {
        const Vec3 fallback = std::fabs(from.z) < 0.9f ? Vec3{0,0,1} : Vec3{0,1,0};
        axis = {from.y*fallback.z - from.z*fallback.y,
                from.z*fallback.x - from.x*fallback.z,
                from.x*fallback.y - from.y*fallback.x};
    }
    return QuatFromAxisAngle(axis, std::acos(cosine)).normalized();
}

Vec3 XrToUe(const Vec3& value) { return {-value.z, value.x, value.y}; }

Vec3 RotateYaw(const Vec3& value, float yaw) {
    const float sine = std::sin(yaw);
    const float cosine = std::cos(yaw);
    return {cosine * value.x - sine * value.y,
            sine * value.x + cosine * value.y, value.z};
}

Vec3 RotatePitchYaw(const Vec3& value, float pitch, float yaw) {
    const float pitchSine = std::sin(pitch);
    const float pitchCosine = std::cos(pitch);
    const Vec3 pitched{pitchCosine * value.x - pitchSine * value.z,
                       value.y,
                       pitchSine * value.x + pitchCosine * value.z};
    return RotateYaw(pitched, yaw);
}

Quat XrControllerToWorld(const float rotation[4], const Quat& inverseReference,
                          float gamePitch, float gameYaw) {
    const Quat current{rotation[0], rotation[1], rotation[2], rotation[3]};
    const Quat relative = QuatMultiply(inverseReference, current).normalized();
    const Vec3 xrForward = RotateByQuat(relative, {0, 0, -1});
    const Vec3 xrUp = RotateByQuat(relative, {0, 1, 0});
    const Vec3 ueForward = RotatePitchYaw(XrToUe(xrForward), gamePitch, gameYaw);
    const Vec3 ueUp = RotatePitchYaw(XrToUe(xrUp), gamePitch, gameYaw);
    return QuatLookAt(ueForward, ueUp);
}

} // namespace

struct ArmIKSystem::Rig {
    bool valid = false;
    uintptr_t localController = 0;
    uintptr_t localPawn = 0;
    uintptr_t component = 0;
    uintptr_t skeletalMesh = 0;
    int skeletalMeshOffset = 0;
    uintptr_t componentPose = 0;
    int componentPoseOffset = 0;
    int componentPoseStride = 0x40;
    uintptr_t refSkeleton = 0;
    int refSkeletonOffset = 0;
    int boneCount = 0;
    int refStride = 0;
    int parentOffset = 0;
    int matrixOffset = 0;
    int rightShoulder = -1;
    int rightElbow = -1;
    int rightWrist = -1;
    int leftShoulder = -1;
    int leftElbow = -1;
    int leftWrist = -1;
    char objectName[64] = {};
    char outerName[64] = {};
    float localToWorld[16] = {};
    BoneMatrix backup[256] = {};
    bool backupBone[256] = {};
    Quat wristCalibration[2] = {};
    bool wristCalibrationValid[2] = {};
    Vec3 viewmodelTrackingOrigin;
    bool viewmodelTrackingOriginValid = false;
    bool backupValid = false;
    uint32_t validationObservations = 0;
    uint32_t validationChanges = 0;
    uint32_t validationUpdates = 0;
    uint64_t lastPoseHash = 0;
    uint64_t lastObservedUpdate = 0;
    uint64_t solvedGeneration = 0;
    uint64_t cachedPoseGeneration = 0;
    BoneMatrix cachedPose[256] = {};
    bool cachedPoseBone[256] = {};
    float cachedLocalToWorld[16] = {};
    bool cachedLocalToWorldValid = false;
};

struct ArmIKSystem::TargetSnapshot {
    bool valid[2] = {};
    bool componentSpace = false;
    Vec3 position[2];
    Quat rotation[2];
    Quat cameraRotation;
    Vec3 cameraPosition;
    uint64_t controllerGeneration = 0;
};

constexpr size_t kTargetHistorySize = 32;

ArmIKSystem& ArmIKSystem::Instance() {
    static ArmIKSystem system;
    return system;
}

void ArmIKSystem::StartDiscovery() {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) return;
    m_rig = new Rig();
    m_targets = new TargetSnapshot[kTargetHistorySize]();
    m_stop = false;
    m_shuttingDown = false;
    m_visibilityEnabled.store(
        config::Get().hide_player_body_and_arms, std::memory_order_release);
    m_inventoryRequestGeneration.fetch_add(1, std::memory_order_acq_rel);
    const bool poseHookInstalled = InstallPoseHook();
    if (poseHookInstalled) SetEnabled(true);
    m_thread = CreateThread(nullptr, 0, DiscoveryThreadProc, this, 0, nullptr);
    if (!m_thread) {
        Log("[ArmIK] Discovery thread creation failed: %lu", GetLastError());
        SetEnabled(false);
    }
    Log("[ArmIK] Discovery started; solver self-test=%s",
        RunTwoBoneIKSelfTest() ? "PASS" : "FAIL");
}

bool ArmIKSystem::InstallPoseHook() {
    constexpr uintptr_t kUpdateSkelPoseRva = 0x008EB1C0;
    static constexpr unsigned char signature[] = {
        0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55,
        0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8,
    };
    HMODULE gameModule = GetModuleHandleA("BorderlandsGOTY.exe");
    if (!gameModule) {
        Log("[ArmIK] UpdateSkelPose hook unavailable: game module not found");
        return false;
    }
    const uintptr_t target = reinterpret_cast<uintptr_t>(gameModule) + kUpdateSkelPoseRva;
    if (memcmp(reinterpret_cast<const void*>(target), signature, sizeof(signature)) != 0) {
        Log("[ArmIK] UpdateSkelPose signature mismatch at RVA 0x%llX; IK writes disabled",
            static_cast<unsigned long long>(kUpdateSkelPoseRva));
        return false;
    }
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(target),
        &HookedUpdateSkelPose, reinterpret_cast<void**>(&m_originalUpdateSkelPose));
    if (status == MH_OK) status = MH_EnableHook(reinterpret_cast<void*>(target));
    if (status != MH_OK) {
        m_originalUpdateSkelPose = nullptr;
        Log("[ArmIK] UpdateSkelPose hook failed: %s", MH_StatusToString(status));
        return false;
    }
    m_poseHookTarget = target;
    m_poseHookInstalled = true;
    Log("[ArmIK] Hooked post-animation UpdateSkelPose at RVA 0x%llX",
        static_cast<unsigned long long>(kUpdateSkelPoseRva));
    return true;
}

void __fastcall ArmIKSystem::HookedUpdateSkelPose(void* component, float deltaTime,
                                                  uint32_t tickFaceFx) {
    auto& system = Instance();
    system.m_inFlightPoseHooks.fetch_add(1, std::memory_order_acq_rel);
    if (system.m_originalUpdateSkelPose)
        system.m_originalUpdateSkelPose(component, deltaTime, tickFaceFx);
    if (system.m_shuttingDown.load(std::memory_order_acquire)) {
        system.m_inFlightPoseHooks.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    system.ObserveComponent(component);
    input::InputHook::Instance().ReapplyWeaponPose(component);
    const uint64_t calls = system.m_poseHookCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (calls == 1)
        Log("[ArmIK] UpdateSkelPose hook received its first component");
    if (system.ApplyPostAnimation(component)) {
        const uint64_t applies = system.m_poseHookApplies.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (applies == 1)
            Log("[ArmIK] First component-space solve committed post-animation");
    }
    system.m_inFlightPoseHooks.fetch_sub(1, std::memory_order_acq_rel);
}

void ArmIKSystem::ObserveComponent(void* component) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(component);
    const uint64_t scanEpoch = m_scanEpoch.load(std::memory_order_acquire);
    if (value < 0x10000 || (scanEpoch & 1) != 0) return;
    for (size_t index = 0; index < m_observedComponents.size(); ++index) {
        auto& slot = m_observedComponents[index];
        uintptr_t current = slot.load(std::memory_order_relaxed);
        if (current == value) {
            m_observedComponentUpdates[index].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (current == 0 && slot.compare_exchange_strong(
                current, value, std::memory_order_relaxed)) {
            if (m_scanEpoch.load(std::memory_order_acquire) != scanEpoch) {
                uintptr_t expected = value;
                slot.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
                return;
            }
            m_observedComponentUpdates[index].store(1, std::memory_order_relaxed);
            return;
        }
    }
    if (m_scanEpoch.load(std::memory_order_acquire) == scanEpoch)
        m_observedComponentsTruncated.fetch_add(1, std::memory_order_relaxed);
}

void ArmIKSystem::Shutdown() {
    if (!m_started.exchange(false)) return;
    m_shuttingDown = true;
    m_stop = true;
    m_inventoryRequestGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_rigRequestGeneration.fetch_add(1, std::memory_order_acq_rel);
    SetVisibilityEnabled(false);
    SetEnabled(false);
    if (m_poseHookTarget)
        MH_DisableHook(reinterpret_cast<void*>(m_poseHookTarget));
    while (m_inFlightPoseHooks.load(std::memory_order_acquire) != 0) Sleep(1);
    if (m_thread) {
        WaitForSingleObject(m_thread, INFINITE);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_poseHookTarget) {
        MH_DisableHook(reinterpret_cast<void*>(m_poseHookTarget));
        MH_RemoveHook(reinterpret_cast<void*>(m_poseHookTarget));
        m_poseHookTarget = 0;
        m_originalUpdateSkelPose = nullptr;
        m_poseHookInstalled = false;
    }
    AcquireSRWLockExclusive(&m_rigLock);
    delete m_rig;
    m_rig = nullptr;
    ReleaseSRWLockExclusive(&m_rigLock);
    AcquireSRWLockExclusive(&m_targetLock);
    delete[] m_targets;
    m_targets = nullptr;
    ReleaseSRWLockExclusive(&m_targetLock);
    AcquireSRWLockExclusive(&m_inventoryLock);
    m_inventory = {};
    ReleaseSRWLockExclusive(&m_inventoryLock);
}

void ArmIKSystem::SetEnabled(bool enabled) {
    if (enabled && !m_poseHookInstalled.load(std::memory_order_acquire)) {
        m_enabled = false;
        Log("[ArmIK] Cannot enable: post-animation hook is unavailable");
        return;
    }
    m_enabled = enabled;
    if (enabled) m_discoveryRequested = true;
    if (!enabled) {
        RestoreVisibility();
        Restore();
    }
    Log("[ArmIK] %s", enabled ? "enabled" : "disabled");
}

void ArmIKSystem::SetVisibilityEnabled(bool enabled) {
    const bool previous = m_visibilityEnabled.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled) return;
    Log("[Visibility] Body and arms hiding %s", enabled ? "enabled" : "disabled");
    if (enabled && m_started.load(std::memory_order_acquire))
        RequestInventoryScan();
    else if (!enabled)
        RestoreVisibility();
}

void ArmIKSystem::SetSimulationEnabled(bool enabled) {
    m_simulationEnabled = enabled;
    if (!enabled) m_latestTargetGeneration = 0;
    Log("[ArmIK] Simulated controllers %s", enabled ? "enabled" : "disabled");
}

void ArmIKSystem::RequestRescan() {
    AcquireSRWLockExclusive(&m_scanResetLock);
    m_scanEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_inventoryRequestGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_rigRequestGeneration.fetch_add(1, std::memory_order_acq_rel);
    RestoreVisibility();
    Restore();
    AcquireSRWLockExclusive(&m_rigLock);
    if (m_rig) *m_rig = {};
    ReleaseSRWLockExclusive(&m_rigLock);
    m_observedComponentsTruncated.store(0, std::memory_order_release);
    for (size_t index = 0; index < m_observedComponents.size(); ++index) {
        m_observedComponentUpdates[index].store(0, std::memory_order_relaxed);
        m_observedComponents[index].store(0, std::memory_order_release);
    }
    AcquireSRWLockExclusive(&m_inventoryLock);
    const uint64_t nextInventoryGeneration = m_inventory.generation + 1;
    m_inventory = {};
    m_inventory.generation = nextInventoryGeneration;
    ReleaseSRWLockExclusive(&m_inventoryLock);
    m_scanEpoch.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&m_scanResetLock);
    Log("[ArmIK] Rig rescan requested");
    m_discoveryRequested = true;
}

void ArmIKSystem::RequestInventoryScan() {
    AcquireSRWLockExclusive(&m_scanResetLock);
    m_scanEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_inventoryRequestGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_observedComponentsTruncated.store(0, std::memory_order_release);
    AcquireSRWLockExclusive(&m_inventoryLock);
    const uint64_t nextInventoryGeneration = m_inventory.generation + 1;
    m_inventory = {};
    m_inventory.generation = nextInventoryGeneration;
    ReleaseSRWLockExclusive(&m_inventoryLock);
    m_scanEpoch.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&m_scanResetLock);
    Log("[VisibilityInventory] Component scan requested; writes=%s",
        m_visibilityEnabled.load(std::memory_order_acquire) ? "enabled" : "disabled");
}

DWORD WINAPI ArmIKSystem::DiscoveryThreadProc(void* context) {
    static_cast<ArmIKSystem*>(context)->DiscoveryLoop();
    return 0;
}

void ArmIKSystem::DiscoveryLoop() {
    uint64_t nextWatchdogMs = GetTickCount64() + 500;
    while (!m_stop.load()) {
        const uint64_t nowMs = GetTickCount64();
        if (nowMs >= nextWatchdogMs) {
            CheckVisibilityWatchdog();
            nextWatchdogMs = nowMs + 500;
        }
        const bool needsRig = (m_enabled.load() || m_discoveryRequested.load()) &&
            !GetStatus().rigValid;
        const uint64_t inventoryRequestGeneration =
            m_inventoryRequestGeneration.load(std::memory_order_acquire);
        const bool needsInventory = inventoryRequestGeneration !=
            m_inventoryCompletedGeneration.load(std::memory_order_acquire);
        if (needsRig || needsInventory) {
            if (ProbeRig(needsInventory ? inventoryRequestGeneration : 0) &&
                GetStatus().rigValid)
                m_discoveryRequested = false;
        }
        const bool localIdentityReady =
            input::WeaponAimSystem::Instance().GetPlayerIdentity().pawnValid;
        Sleep(needsRig && localIdentityReady ? 100 : 250);
    }
}

bool ArmIKSystem::ProbeRig(uint64_t inventoryRequestGeneration) {
    const uint64_t scanEpoch = m_scanEpoch.load(std::memory_order_acquire);
    if ((scanEpoch & 1) != 0) return false;
    const uint64_t rigRequestGeneration =
        m_rigRequestGeneration.load(std::memory_order_acquire);
    const camera::UE3Globals globals = camera::GetUE3GlobalsSnapshot();
    if (!globals.gNamesValid || !globals.gObjectsValid ||
        globals.gObjectNameOffset < 0 || globals.gObjectClassOffset < 0) return false;
    std::vector<uintptr_t> observedComponents;
    for (const auto& slot : m_observedComponents) {
        const uintptr_t component = slot.load(std::memory_order_relaxed);
        if (component) observedComponents.push_back(component);
    }
    const input::PlayerIdentitySnapshot identity =
        input::WeaponAimSystem::Instance().GetPlayerIdentity();
    ComponentInventoryStatus inventory;
    inventory.controller = identity.controller;
    inventory.pawn = identity.pawn;
    inventory.weapon = identity.weapon;
    inventory.pawnIdentityValid = identity.pawnValid;
    inventory.weaponIdentityValid = identity.weaponValid;
    auto rolePriority = [](ComponentRole role) {
        switch (role) {
        case ComponentRole::ProtectedWeapon: return 3;
        case ComponentRole::ProbableFirstPersonArms: return 2;
        case ComponentRole::PawnBody: return 1;
        default: return 0;
        }
    };
    auto recordInventoryEntry = [&](const ComponentInventoryEntry& entry) {
        for (size_t index = 0; index < inventory.count; ++index) {
            if (inventory.entries[index].component != entry.component) continue;
            if (rolePriority(entry.role) > rolePriority(inventory.entries[index].role))
                inventory.entries[index] = entry;
            return;
        }
        if (inventory.count < inventory.entries.size())
            inventory.entries[inventory.count++] = entry;
        else
            ++inventory.truncatedComponentCount;
    };

    Rig best = {};
    int bestScore = -1;
    int classMatches = 0;
    int posePairs = 0;
    int meshMatches = 0;
    int skeletonLayouts = 0;
    int armLayouts = 0;
    int weaponGraphComponents = 0;
    for (uintptr_t component : observedComponents) {
        if (m_stop.load(std::memory_order_acquire)) return false;
        char className[128] = {};
        char objectName[128] = {};
        if (!ReadClassName(globals, component, className, sizeof(className)) ||
            Lower(className).find("skeletalmeshcomponent") == std::string::npos ||
            !ReadObjectName(globals, component, objectName, sizeof(objectName)) ||
            strncmp(objectName, "Default__", 9) == 0) continue;
        ++classMatches;

        int score = 0;
        const std::string objectLower = Lower(objectName);
        const bool objectIsViewmodel = objectLower.find("weapon") != std::string::npos ||
            objectLower.find("arm") != std::string::npos ||
            objectLower.find("first") != std::string::npos;
        if (objectLower.find("weapon") != std::string::npos) score += 100;
        if (objectLower.find("arm") != std::string::npos) score += 100;
        if (objectLower.find("first") != std::string::npos) score += 50;
        uintptr_t outer = 0;
        char outerName[128] = {};
        bool outerIsViewmodel = false;
        bool outerIsPlayerPawn = false;
        if (ReadMemory(component + globals.gObjectNameOffset - 8, &outer, sizeof(outer)) && outer) {
            if (ReadObjectName(globals, outer, outerName, sizeof(outerName))) {
                const std::string outerLower = Lower(outerName);
                outerIsPlayerPawn = outerLower.find("willowplayerpawn") != std::string::npos;
                outerIsViewmodel = outerLower.find("weapon") != std::string::npos ||
                    outerLower.find("arm") != std::string::npos ||
                    outerLower.find("first") != std::string::npos ||
                    outerIsPlayerPawn;
                if (outerLower.find("weapon") != std::string::npos) score += 150;
                if (outerLower.find("arm") != std::string::npos) score += 100;
                if (outerIsPlayerPawn) score += 120;
            }
        }
        uint64_t componentUpdates = 0;
        for (size_t observedIndex = 0;
             observedIndex < m_observedComponents.size(); ++observedIndex) {
            if (m_observedComponents[observedIndex].load(std::memory_order_relaxed) == component) {
                componentUpdates = m_observedComponentUpdates[observedIndex].load(
                    std::memory_order_relaxed);
                break;
            }
        }
        const bool exactPawnOuter = identity.pawnValid &&
            OuterChainContains(globals, component, identity.pawn);
        const bool exactWeaponOuter = identity.weaponValid &&
            OuterChainContains(globals, component, identity.weapon);
        constexpr int kSpaceBasesOffset = 0x330;
        constexpr int kLocalAtomsOffset = 0x340;
        struct PoseCandidate { TArray64 array; float average; int offset; };
        PoseCandidate componentPose = {{}, 0.0f, kSpaceBasesOffset};
        PoseCandidate localPose = {{}, 0.0f, kLocalAtomsOffset};
        if (!ReadMemory(component + kSpaceBasesOffset, &componentPose.array,
                        sizeof(componentPose.array)) ||
            !ReadMemory(component + kLocalAtomsOffset, &localPose.array,
                        sizeof(localPose.array)) ||
            !ValidateMatrixPoseArray(componentPose.array, componentPose.average) ||
            !ValidatePoseArray(localPose.array, localPose.average)) continue;
        if (componentPose.array.count != localPose.array.count ||
            componentPose.array.data == localPose.array.data ||
            componentPose.average < localPose.average * 1.2f ||
            componentPose.average - localPose.average < 5.0f) continue;
        ++posePairs;
        const bool genericSkeletalActor = Lower(outerName).find("skeletalmeshactor") !=
            std::string::npos;
        if (!objectIsViewmodel && !outerIsViewmodel && !genericSkeletalActor) {
            static std::atomic<uint32_t> rejectedPoseLogs{0};
            const uint32_t logIndex = rejectedPoseLogs.fetch_add(
                1, std::memory_order_relaxed);
            if (logIndex < 32) {
                Log("[ArmIK] Pose candidate rejected by viewmodel filter: "
                    "object=%s outer=%s class=%s bones=%d component=%p",
                    objectName, outerName, className, componentPose.array.count,
                    reinterpret_cast<void*>(component));
            }
            continue;
        }

        for (int pointerOffset = 0x80; pointerOffset <= 0x700; pointerOffset += 4) {
            uintptr_t mesh = 0;
            char meshClass[128] = {};
            char meshName[128] = {};
            if (!ReadMemory(component + pointerOffset, &mesh, sizeof(mesh)) || !mesh ||
                !ReadClassName(globals, mesh, meshClass, sizeof(meshClass))) continue;
            const std::string meshClassLower = Lower(meshClass);
            if (meshClassLower.find("skeletalmesh") == std::string::npos ||
                meshClassLower.find("component") != std::string::npos) continue;
            ReadObjectName(globals, mesh, meshName, sizeof(meshName));
            ++meshMatches;

            for (int arrayOffset = 0x40; arrayOffset <= 0x800; arrayOffset += 4) {
                TArray64 skeleton = {};
                if (!ReadMemory(mesh + arrayOffset, &skeleton, sizeof(skeleton)) ||
                    skeleton.count != componentPose.array.count || skeleton.count > 256 ||
                    skeleton.capacity < skeleton.count) continue;
                for (int stride : {0x50, 0x58, 0x40, 0x60}) {
                    for (int parentOffset : {8, 12, 0x28, 0x40, 0x44}) {
                        int roots = 0;
                        int named = 0;
                        int rightWrist = -1;
                        int leftWrist = -1;
                        int rightElbow = -1;
                        int leftElbow = -1;
                        int rightShoulder = -1;
                        int leftShoulder = -1;
                        bool hasPelvis = false;
                        bool hasSpine = false;
                        bool hasHead = false;
                        bool hasRightLowerBody = false;
                        bool hasLeftLowerBody = false;
                        bool hierarchy = true;
                        for (int bone = 0; bone < skeleton.count; ++bone) {
                            const uintptr_t entry = skeleton.data +
                                static_cast<uintptr_t>(bone) * stride;
                            int32_t parent = -2;
                            int32_t nameIndex = -1;
                            char boneName[128] = {};
                            if (!ReadMemory(entry + parentOffset, &parent, sizeof(parent)) ||
                                !ReadMemory(entry, &nameIndex, sizeof(nameIndex)) ||
                                (parent != -1 && parent != 0 && (parent < 0 || parent >= bone))) {
                                hierarchy = false;
                                break;
                            }
                            if (parent == -1 || (bone == 0 && parent == 0)) ++roots;
                            if (ReadName(globals, nameIndex, boneName, sizeof(boneName))) {
                                ++named;
                                const std::string lower = Lower(boneName);
                                const bool rightSide = lower.find("right") != std::string::npos ||
                                    lower.find("r_") == 0 || lower.find("_r") != std::string::npos;
                                const bool leftSide = lower.find("left") != std::string::npos ||
                                    lower.find("l_") == 0 || lower.find("_l") != std::string::npos;
                                hasPelvis = hasPelvis || lower.find("pelvis") != std::string::npos ||
                                    lower.find("hip") != std::string::npos;
                                hasSpine = hasSpine || lower.find("spine") != std::string::npos ||
                                    lower.find("chest") != std::string::npos;
                                hasHead = hasHead || lower.find("head") != std::string::npos ||
                                    lower.find("neck") != std::string::npos;
                                const bool lowerBodyBone = lower.find("thigh") != std::string::npos ||
                                    lower.find("calf") != std::string::npos ||
                                    lower.find("leg") != std::string::npos ||
                                    lower.find("foot") != std::string::npos ||
                                    lower.find("toe") != std::string::npos;
                                hasRightLowerBody = hasRightLowerBody || (lowerBodyBone && rightSide);
                                hasLeftLowerBody = hasLeftLowerBody || (lowerBodyBone && leftSide);
                                const bool hand = lower.find("hand") != std::string::npos ||
                                    lower.find("wrist") != std::string::npos;
                                if (hand && (lower.find("right") != std::string::npos ||
                                    lower.find("r_") == 0 || lower.find("_r") != std::string::npos))
                                    rightWrist = bone;
                                if (hand && (lower.find("left") != std::string::npos ||
                                    lower.find("l_") == 0 || lower.find("_l") != std::string::npos))
                                    leftWrist = bone;
                                if ((lower.find("forearm") != std::string::npos ||
                                     lower.find("lowerarm") != std::string::npos) && rightSide)
                                    rightElbow = bone;
                                if ((lower.find("forearm") != std::string::npos ||
                                     lower.find("lowerarm") != std::string::npos) && leftSide)
                                    leftElbow = bone;
                                if (lower.find("upperarm") != std::string::npos && rightSide)
                                    rightShoulder = bone;
                                if (lower.find("upperarm") != std::string::npos && leftSide)
                                    leftShoulder = bone;
                            }
                        }
                        if (!hierarchy || roots < 1 || roots > 4 ||
                            named < skeleton.count / 2) continue;
                        ++skeletonLayouts;

                        auto parentOf = [&](int bone) {
                            int32_t parent = -1;
                            ReadMemory(skeleton.data + static_cast<uintptr_t>(bone) * stride +
                                parentOffset, &parent, sizeof(parent));
                            return parent;
                        };
                        Rig rig = {};
                        rig.localController = identity.controller;
                        rig.localPawn = identity.pawn;
                        rig.component = component;
                        rig.skeletalMesh = mesh;
                        rig.skeletalMeshOffset = pointerOffset;
                        rig.componentPose = componentPose.array.data;
                        rig.componentPoseOffset = componentPose.offset;
                        rig.componentPoseStride = 0x40;
                        rig.refSkeleton = skeleton.data;
                        rig.refSkeletonOffset = arrayOffset;
                        rig.boneCount = skeleton.count;
                        rig.refStride = stride;
                        rig.parentOffset = parentOffset;
                        rig.rightWrist = rightWrist;
                        rig.rightElbow = rightElbow;
                        rig.rightShoulder = rightShoulder;
                        rig.leftWrist = leftWrist;
                        rig.leftElbow = leftElbow;
                        rig.leftShoulder = leftShoulder;
                        strcpy_s(rig.objectName, objectName);
                        strcpy_s(rig.outerName, outerName);
                        const int preferredMatrixOffsets[] = {
                            0xC0, 0x80, 0x84, 0x88, 0x8C, 0x90, 0x94, 0x98, 0x9C,
                            0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC,
                            0xC4, 0xC8, 0xCC, 0xD0, 0xD4, 0xD8, 0xDC, 0xE0,
                            0xE4, 0xE8, 0xEC, 0xF0, 0xF4, 0xF8, 0xFC,
                        };
                        for (int matrixOffset : preferredMatrixOffsets) {
                            float matrix[16] = {};
                            if (ReadMemory(component + matrixOffset, matrix, sizeof(matrix)) &&
                                ValidateMatrix(matrix)) {
                                rig.matrixOffset = matrixOffset;
                                memcpy(rig.localToWorld, matrix, sizeof(matrix));
                                break;
                            }
                        }
                        auto descendsFrom = [&](int bone, int ancestor) {
                            int current = bone;
                            for (int depth = 0; depth <= skeleton.count; ++depth) {
                                if (current == ancestor) return true;
                                if (current < 0 || current >= skeleton.count) return false;
                                current = parentOf(current);
                            }
                            return false;
                        };
                        const bool rightChain = rightWrist >= 0 && rightElbow >= 0 &&
                            rightShoulder >= 0 && rightWrist != rightElbow &&
                            rightElbow != rightShoulder && descendsFrom(rightWrist, rightElbow) &&
                            descendsFrom(rightElbow, rightShoulder);
                        const bool leftChain = leftWrist >= 0 && leftElbow >= 0 &&
                            leftShoulder >= 0 && leftWrist != leftElbow &&
                            leftElbow != leftShoulder && descendsFrom(leftWrist, leftElbow) &&
                            descendsFrom(leftElbow, leftShoulder);
                        rig.valid = rig.matrixOffset != 0 &&
                            (rightChain || leftChain);
                        if (rightChain || leftChain) ++armLayouts;
                        int proximityScore = 0;
                        float cameraDistance = -1.0f;
                        const float* cameraLocation = camera::GetCameraLocation();
                        float cameraPosition[3] = {};
                        bool haveCameraPosition = false;
                        if (cameraLocation && rig.matrixOffset &&
                            ReadMemory(reinterpret_cast<uintptr_t>(cameraLocation),
                                       cameraPosition, sizeof(cameraPosition))) {
                            haveCameraPosition = std::isfinite(cameraPosition[0]) &&
                                std::isfinite(cameraPosition[1]) &&
                                std::isfinite(cameraPosition[2]);
                        }
                        if (!haveCameraPosition) {
                            AcquireSRWLockShared(&m_cameraCacheLock);
                            if (m_hasGoodCameraLocation) {
                                cameraPosition[0] = m_goodCameraLocation[0];
                                cameraPosition[1] = m_goodCameraLocation[1];
                                cameraPosition[2] = m_goodCameraLocation[2];
                                haveCameraPosition = true;
                            }
                            ReleaseSRWLockShared(&m_cameraCacheLock);
                        }
                        if (haveCameraPosition) {
                            const float dx = rig.localToWorld[12] - cameraPosition[0];
                            const float dy = rig.localToWorld[13] - cameraPosition[1];
                            const float dz = rig.localToWorld[14] - cameraPosition[2];
                            const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                            cameraDistance = distance;
                            if (std::isfinite(distance) && distance < 500.0f) proximityScore = 30;
                        }
                        const bool torsoSignature = hasPelvis && hasSpine && hasHead;
                        const bool lowerBodySignature = hasRightLowerBody && hasLeftLowerBody;
                        const std::string meshLower = Lower(meshName);
                        const bool semanticHandMesh =
                            meshLower.find("hands") != std::string::npos ||
                            meshLower.find("arms") != std::string::npos ||
                            meshLower.find("firstperson") != std::string::npos ||
                            meshLower.find("first_person") != std::string::npos;
                        const bool conservativeWeaponEvidence =
                            objectLower.find("weapon") != std::string::npos ||
                            Lower(outerName).find("weapon") != std::string::npos ||
                            meshLower.find("weapon") != std::string::npos ||
                            meshLower.find("gun") != std::string::npos;
                        ComponentInventoryEntry inventoryEntry;
                        inventoryEntry.component = component;
                        inventoryEntry.outer = outer;
                        ReadMemory(component + globals.gObjectClassOffset,
                                   &inventoryEntry.classObject,
                                   sizeof(inventoryEntry.classObject));
                        ReadMemory(component + globals.gObjectNameOffset,
                                   &inventoryEntry.objectNameToken,
                                   sizeof(inventoryEntry.objectNameToken));
                        inventoryEntry.skeletalMesh = mesh;
                        inventoryEntry.updateCount = componentUpdates;
                        inventoryEntry.boneCount = skeleton.count;
                        inventoryEntry.skeletalMeshOffset = pointerOffset;
                        inventoryEntry.localToWorldOffset = rig.matrixOffset;
                        inventoryEntry.cameraDistance = cameraDistance;
                        inventoryEntry.exactPawnOuter = exactPawnOuter;
                        inventoryEntry.exactWeaponOuter = exactWeaponOuter;
                        inventoryEntry.conservativeWeaponEvidence = conservativeWeaponEvidence;
                        inventoryEntry.torsoSignature = torsoSignature;
                        inventoryEntry.lowerBodySignature = lowerBodySignature;
                        inventoryEntry.rightArmChain = rightChain;
                        inventoryEntry.leftArmChain = leftChain;
                        strcpy_s(inventoryEntry.objectName, objectName);
                        strcpy_s(inventoryEntry.className, className);
                        strcpy_s(inventoryEntry.outerName, outerName);
                        strcpy_s(inventoryEntry.meshName, meshName);
                        if (exactWeaponOuter || conservativeWeaponEvidence) {
                            inventoryEntry.role = ComponentRole::ProtectedWeapon;
                        } else if (exactPawnOuter && semanticHandMesh &&
                                   (rightChain || leftChain) && proximityScore) {
                            // BL1 hand meshes retain the full character skeleton,
                            // so topology alone cannot distinguish them from body meshes.
                            inventoryEntry.role = ComponentRole::ProbableFirstPersonArms;
                        } else if (exactPawnOuter && torsoSignature && lowerBodySignature) {
                            inventoryEntry.role = ComponentRole::PawnBody;
                        }
                        recordInventoryEntry(inventoryEntry);
                        if (rightChain || leftChain) {
                            static std::atomic<uint32_t> armLayoutLogs{0};
                            const uint32_t logIndex = armLayoutLogs.fetch_add(
                                1, std::memory_order_relaxed);
                            if (logIndex < 24) {
                                Log("[ArmIK] Arm layout: object=%s outer=%s bones=%d "
                                    "matrix=+0x%X distance=%.1f right=%d/%d/%d left=%d/%d/%d",
                                    objectName, outerName, rig.boneCount, rig.matrixOffset,
                                    cameraDistance, rightShoulder, rightElbow, rightWrist,
                                    leftShoulder, leftElbow, leftWrist);
                            }
                        }
                        // Never drive an NPC, body mesh, or unowned viewmodel.
                        // Visibility inventory evidence is also the write gate.
                        if (!proximityScore ||
                            inventoryEntry.role != ComponentRole::ProbableFirstPersonArms)
                            continue;
                        const int rigScore = score + named + proximityScore +
                            (rig.rightWrist >= 0 ? 20 : 0) + (rig.leftWrist >= 0 ? 20 : 0);
                        if (rig.valid && rigScore > bestScore) {
                            best = rig;
                            bestScore = rigScore;
                        }
                    }
                }
            }
        }
    }

    // Weapon render components do not necessarily execute UpdateSkelPose.
    // Enumerate the validated weapon's UObject outer graph to protect static,
    // skeletal, particle, and attachment components before visibility work.
    if (inventoryRequestGeneration && identity.weaponValid) {
        TArray64 objects = {};
        if (ReadMemory(globals.gObjectsAddress, &objects, sizeof(objects)) &&
            objects.data && objects.count > 0 && objects.count <= objects.capacity &&
            objects.count < 2000000) {
            for (int32_t index = 0; index < objects.count; ++index) {
                if (m_stop.load(std::memory_order_acquire)) return false;
                uintptr_t object = 0;
                if (!ReadMemory(objects.data + static_cast<uintptr_t>(index) * sizeof(uintptr_t),
                                &object, sizeof(object)) || !object || object == identity.weapon ||
                    !OuterChainContains(globals, object, identity.weapon)) continue;

                char className[128] = {};
                char objectName[128] = {};
                if (!ReadClassName(globals, object, className, sizeof(className)) ||
                    Lower(className).find("component") == std::string::npos ||
                    !ReadObjectName(globals, object, objectName, sizeof(objectName)) ||
                    strncmp(objectName, "Default__", 9) == 0) continue;

                ++weaponGraphComponents;
                ComponentInventoryEntry entry;
                entry.role = ComponentRole::ProtectedWeapon;
                entry.component = object;
                ReadMemory(object + globals.gObjectClassOffset,
                           &entry.classObject, sizeof(entry.classObject));
                ReadMemory(object + globals.gObjectNameOffset,
                           &entry.objectNameToken, sizeof(entry.objectNameToken));
                entry.exactWeaponOuter = true;
                strcpy_s(entry.objectName, objectName);
                strcpy_s(entry.className, className);
                if (strstr(className, "SkeletalMeshComponent")) {
                    BoneMatrix localToWorld = {};
                    constexpr int kPrimitiveLocalToWorldOffset = 0xA0;
                    if (ReadMemory(object + kPrimitiveLocalToWorldOffset,
                                   &localToWorld, sizeof(localToWorld)) &&
                        ValidateMatrix(localToWorld.values)) {
                        entry.localToWorldOffset = kPrimitiveLocalToWorldOffset;
                    }
                    for (int pointerOffset = 0x80; pointerOffset <= 0x700;
                         pointerOffset += 4) {
                        uintptr_t mesh = 0;
                        char meshClass[128] = {};
                        if (!ReadMemory(object + pointerOffset, &mesh, sizeof(mesh)) ||
                            !mesh || !ReadClassName(globals, mesh, meshClass,
                                                    sizeof(meshClass))) continue;
                        const std::string meshClassLower = Lower(meshClass);
                        if (meshClassLower.find("skeletalmesh") == std::string::npos ||
                            meshClassLower.find("component") != std::string::npos) continue;
                        entry.skeletalMesh = mesh;
                        entry.skeletalMeshOffset = pointerOffset;
                        ReadObjectName(globals, mesh, entry.meshName,
                                       sizeof(entry.meshName));
                        break;
                    }
                }
                ReadMemory(object + globals.gObjectNameOffset - 8,
                           &entry.outer, sizeof(entry.outer));
                if (entry.outer)
                    ReadObjectName(globals, entry.outer, entry.outerName, sizeof(entry.outerName));
                for (size_t observedIndex = 0;
                     observedIndex < m_observedComponents.size(); ++observedIndex) {
                    if (m_observedComponents[observedIndex].load(
                            std::memory_order_relaxed) == object) {
                        entry.updateCount = m_observedComponentUpdates[observedIndex].load(
                            std::memory_order_relaxed);
                        break;
                    }
                }
                recordInventoryEntry(entry);
            }
        }
    }

    if (inventoryRequestGeneration) {
        const input::PlayerIdentitySnapshot latestIdentity =
            input::WeaponAimSystem::Instance().GetPlayerIdentity();
        const bool identityChanged = latestIdentity.generation != identity.generation ||
            latestIdentity.controller != identity.controller || latestIdentity.pawn != identity.pawn ||
            latestIdentity.weapon != identity.weapon ||
            latestIdentity.pawnValid != identity.pawnValid ||
            latestIdentity.weaponValid != identity.weaponValid;
        if (identityChanged || m_scanEpoch.load(std::memory_order_acquire) != scanEpoch ||
            m_inventoryRequestGeneration.load(
                std::memory_order_acquire) != inventoryRequestGeneration) {
            Log("[VisibilityInventory] Scan discarded: identity or request changed during scan");
            return false;
        }
        AcquireSRWLockExclusive(&m_inventoryLock);
        if (m_scanEpoch.load(std::memory_order_acquire) != scanEpoch ||
            m_inventoryRequestGeneration.load(std::memory_order_acquire) !=
                inventoryRequestGeneration) {
            ReleaseSRWLockExclusive(&m_inventoryLock);
            Log("[VisibilityInventory] Scan discarded: request changed before publication");
            return false;
        }
        inventory.generation = m_inventory.generation + 1;
        inventory.weaponComponentCount = weaponGraphComponents;
        inventory.truncatedComponentCount += static_cast<int>(
            m_observedComponentsTruncated.load(std::memory_order_acquire));
        m_inventory = inventory;
        ReleaseSRWLockExclusive(&m_inventoryLock);
        m_inventoryCompletedGeneration.store(
            inventoryRequestGeneration, std::memory_order_release);

        int32_t hiddenGameOffset = -1;
        uint32_t hiddenGameMask = 0;
        if (LogVisibilityPropertyInventory(
                globals, inventory, hiddenGameOffset, hiddenGameMask))
            ApplyVisibility(inventory, hiddenGameOffset, hiddenGameMask,
                            inventoryRequestGeneration);

        Log("[VisibilityInventory] generation=%llu entries=%zu pawnValid=%d "
            "weaponValid=%d controller=%p pawn=%p weapon=%p weaponComponents=%d "
            "truncated=%d visibilityWrites=%s",
            static_cast<unsigned long long>(inventory.generation), inventory.count,
            inventory.pawnIdentityValid, inventory.weaponIdentityValid,
            reinterpret_cast<void*>(inventory.controller),
            reinterpret_cast<void*>(inventory.pawn),
            reinterpret_cast<void*>(inventory.weapon), weaponGraphComponents,
            inventory.truncatedComponentCount,
            m_visibilityEnabled.load(std::memory_order_acquire) ? "enabled" : "disabled");
        for (size_t index = 0; index < inventory.count; ++index) {
            const ComponentInventoryEntry& entry = inventory.entries[index];
            Log("[VisibilityInventory] [%zu] role=%s component=%p class=%s object=%s "
                "outer=%p/%s mesh=%p/%s bones=%d updates=%llu matrix=+0x%X distance=%.1f "
                "pawnOuter=%d weaponOuter=%d weaponHint=%d torso=%d lower=%d arms=%d/%d",
                index, RoleName(entry.role), reinterpret_cast<void*>(entry.component),
                entry.className, entry.objectName, reinterpret_cast<void*>(entry.outer),
                entry.outerName, reinterpret_cast<void*>(entry.skeletalMesh), entry.meshName,
                entry.boneCount, static_cast<unsigned long long>(entry.updateCount),
                entry.localToWorldOffset, entry.cameraDistance,
                entry.exactPawnOuter, entry.exactWeaponOuter,
                entry.conservativeWeaponEvidence, entry.torsoSignature,
                entry.lowerBodySignature, entry.rightArmChain, entry.leftArmChain);
        }
    }

    if (!best.valid) {
        static std::atomic<uint32_t> probeLogs{0};
        const uint32_t logIndex = probeLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 10) {
            Log("[ArmIK] Probe %u: observed=%zu class=%d posePairs=%d mesh=%d "
                "skeleton=%d arms=%d", logIndex + 1, observedComponents.size(),
                classMatches, posePairs, meshMatches, skeletonLayouts, armLayouts);
        }
        return false;
    }
    uint64_t observedUpdate = 0;
    for (size_t index = 0; index < m_observedComponents.size(); ++index) {
        if (m_observedComponents[index].load(std::memory_order_relaxed) == best.component) {
            observedUpdate = m_observedComponentUpdates[index].load(std::memory_order_relaxed);
            break;
        }
    }
    const uint64_t poseHash = HashPose(
        best.componentPose, best.boneCount, best.componentPoseStride);
    if (!poseHash) return false;
    if (m_stop.load(std::memory_order_acquire) ||
        m_scanEpoch.load(std::memory_order_acquire) != scanEpoch ||
        m_rigRequestGeneration.load(std::memory_order_acquire) != rigRequestGeneration)
        return false;
    AcquireSRWLockExclusive(&m_rigLock);
    if (m_stop.load(std::memory_order_acquire) ||
        m_scanEpoch.load(std::memory_order_acquire) != scanEpoch ||
        m_rigRequestGeneration.load(std::memory_order_acquire) != rigRequestGeneration) {
        ReleaseSRWLockExclusive(&m_rigLock);
        return false;
    }
    if (m_rig && m_rig->localController == best.localController &&
        m_rig->localPawn == best.localPawn &&
        m_rig->component == best.component &&
        m_rig->skeletalMesh == best.skeletalMesh &&
        m_rig->skeletalMeshOffset == best.skeletalMeshOffset &&
        m_rig->componentPose == best.componentPose &&
        m_rig->componentPoseOffset == best.componentPoseOffset &&
        m_rig->componentPoseStride == best.componentPoseStride &&
        m_rig->boneCount == best.boneCount &&
        m_rig->refSkeleton == best.refSkeleton &&
        m_rig->refSkeletonOffset == best.refSkeletonOffset &&
        m_rig->refStride == best.refStride &&
        m_rig->parentOffset == best.parentOffset &&
        m_rig->matrixOffset == best.matrixOffset &&
        m_rig->rightShoulder == best.rightShoulder &&
        m_rig->rightElbow == best.rightElbow &&
        m_rig->rightWrist == best.rightWrist &&
        m_rig->leftShoulder == best.leftShoulder &&
        m_rig->leftElbow == best.leftElbow &&
        m_rig->leftWrist == best.leftWrist) {
        best.validationObservations = m_rig->validationObservations + 1;
        best.validationChanges = m_rig->validationChanges +
            (m_rig->lastPoseHash != poseHash ? 1u : 0u);
        best.validationUpdates = m_rig->validationUpdates +
            (m_rig->lastObservedUpdate != observedUpdate ? 1u : 0u);
        best.wristCalibration[0] = m_rig->wristCalibration[0];
        best.wristCalibration[1] = m_rig->wristCalibration[1];
        best.wristCalibrationValid[0] = m_rig->wristCalibrationValid[0];
        best.wristCalibrationValid[1] = m_rig->wristCalibrationValid[1];
        best.viewmodelTrackingOrigin = m_rig->viewmodelTrackingOrigin;
        best.viewmodelTrackingOriginValid = m_rig->viewmodelTrackingOriginValid;
        memcpy(best.cachedLocalToWorld, m_rig->cachedLocalToWorld,
               sizeof(best.cachedLocalToWorld));
        best.cachedLocalToWorldValid = m_rig->cachedLocalToWorldValid;
    } else {
        best.validationObservations = 1;
    }
    best.lastPoseHash = poseHash;
    best.lastObservedUpdate = observedUpdate;
    best.valid = best.validationObservations >= 3 &&
        (best.validationChanges >= 1 || best.validationUpdates >= 2);
    *m_rig = best;
    ReleaseSRWLockExclusive(&m_rigLock);
    Log("[ArmIK] Rig candidate: validated=%d observations=%u changes=%u updates=%u "
        "object=%s outer=%s component=%p mesh=%p bones=%d pose=+0x%X/%p matrix=+0x%X "
        "right=%d/%d/%d left=%d/%d/%d", best.valid,
        best.validationObservations, best.validationChanges, best.validationUpdates,
        best.objectName, best.outerName, reinterpret_cast<void*>(best.component),
        reinterpret_cast<void*>(best.skeletalMesh), best.boneCount,
        best.componentPoseOffset, reinterpret_cast<void*>(best.componentPose), best.matrixOffset,
        best.rightShoulder, best.rightElbow, best.rightWrist,
        best.leftShoulder, best.leftElbow, best.leftWrist);
    return true;
}

bool ArmIKSystem::ApplyVisibility(const ComponentInventoryStatus& inventory,
                                   int32_t visibilityOffset,
                                   uint32_t hiddenGameMask,
                                   uint64_t inventoryRequestGeneration) {
    if (m_shuttingDown.load(std::memory_order_acquire) || m_stop.load() ||
        !m_enabled.load(std::memory_order_acquire) ||
        !m_visibilityEnabled.load(std::memory_order_acquire) ||
        !m_poseHookInstalled.load(std::memory_order_acquire)) {
        RestoreVisibility();
        return false;
    }
    if (m_inventoryRequestGeneration.load(std::memory_order_acquire) !=
            inventoryRequestGeneration ||
        !inventory.pawnIdentityValid || !inventory.weaponIdentityValid ||
        inventory.truncatedComponentCount != 0 ||
        visibilityOffset != 0x1AC || hiddenGameMask != 0x4) {
        return false;
    }

    const ComponentInventoryEntry* targets[2] = {};
    int bodyCount = 0;
    int armsCount = 0;
    for (size_t index = 0; index < inventory.count; ++index) {
        const ComponentInventoryEntry& entry = inventory.entries[index];
        if (entry.role == ComponentRole::PawnBody) {
            ++bodyCount;
            targets[0] = &entry;
        } else if (entry.role == ComponentRole::ProbableFirstPersonArms) {
            ++armsCount;
            targets[1] = &entry;
        }
    }
    if (bodyCount != 1 || armsCount != 1 || !targets[0] || !targets[1] ||
        targets[0]->component == targets[1]->component ||
        targets[1]->localToWorldOffset <= 0) {
        Log("[Visibility] Hide deferred: bodyCount=%d armsCount=%d", bodyCount, armsCount);
        return false;
    }
    for (size_t index = 0; index < inventory.count; ++index) {
        if (inventory.entries[index].role == ComponentRole::ProtectedWeapon &&
            (inventory.entries[index].component == targets[0]->component ||
             inventory.entries[index].component == targets[1]->component)) {
            Log("[Visibility] Hide rejected: target overlaps protected weapon component");
            return false;
        }
    }

    const camera::UE3Globals globals = camera::GetUE3GlobalsSnapshot();
    AcquireSRWLockShared(&m_visibilityLock);
    const bool alreadyApplied = m_visibilityActive &&
        m_visibilityController == inventory.controller &&
        m_visibilityPawn == inventory.pawn &&
        m_visibilityWeapon == inventory.weapon &&
        m_visibilityComponents[0] == targets[0]->component &&
        m_visibilityComponents[1] == targets[1]->component &&
        m_visibilityWordOffset == visibilityOffset &&
        m_visibilityHiddenMask == hiddenGameMask;
    const bool replacing = m_visibilityActive && !alreadyApplied;
    ReleaseSRWLockShared(&m_visibilityLock);
    bool liveApplied = alreadyApplied;
    if (liveApplied) {
        for (int targetIndex = 0; targetIndex < 2; ++targetIndex) {
            uint32_t currentWord = 0;
            liveApplied = ValidateVisibilityTarget(globals, *targets[targetIndex],
                              inventory.pawn, inventory.weapon) &&
                ReadAtomicWord(targets[targetIndex]->component + visibilityOffset,
                               currentWord) &&
                (currentWord & hiddenGameMask) != 0;
            if (!liveApplied) break;
        }
    }
    if (liveApplied) return true;
    if (alreadyApplied || replacing) {
        AcquireSRWLockShared(&m_scanResetLock);
        const bool requestStillCurrent =
            m_inventoryRequestGeneration.load(std::memory_order_acquire) ==
                inventoryRequestGeneration;
        if (requestStillCurrent) RestoreVisibility();
        ReleaseSRWLockShared(&m_scanResetLock);
        if (!requestStillCurrent) return false;
        AcquireSRWLockShared(&m_visibilityLock);
        const bool restorePending = m_visibilityActive;
        ReleaseSRWLockShared(&m_visibilityLock);
        if (restorePending) return false;
    }

    const input::PlayerIdentitySnapshot identity =
        input::WeaponAimSystem::Instance().GetPlayerIdentity();
    if (!identity.pawnValid || !identity.weaponValid ||
        identity.controller != inventory.controller || identity.pawn != inventory.pawn ||
        identity.weapon != inventory.weapon) return false;

    uint32_t originalWords[2] = {};
    for (int targetIndex = 0; targetIndex < 2; ++targetIndex) {
        const ComponentInventoryEntry& entry = *targets[targetIndex];
        if (!ValidateVisibilityTarget(globals, entry, inventory.pawn, inventory.weapon) ||
            !ReadAtomicWord(entry.component + visibilityOffset,
                            originalWords[targetIndex])) {
            Log("[Visibility] Hide rejected: target %d failed final identity validation",
                targetIndex);
            return false;
        }
    }

    AcquireSRWLockExclusive(&m_visibilityLock);
    const input::PlayerIdentitySnapshot finalIdentity =
        input::WeaponAimSystem::Instance().GetPlayerIdentity();
    if (m_shuttingDown.load(std::memory_order_acquire) || m_stop.load() ||
        !m_enabled.load(std::memory_order_acquire) ||
        !m_visibilityEnabled.load(std::memory_order_acquire) ||
        m_inventoryRequestGeneration.load(std::memory_order_acquire) !=
            inventoryRequestGeneration ||
        !finalIdentity.pawnValid || !finalIdentity.weaponValid ||
        finalIdentity.controller != inventory.controller ||
        finalIdentity.pawn != inventory.pawn || finalIdentity.weapon != inventory.weapon) {
        ReleaseSRWLockExclusive(&m_visibilityLock);
        return false;
    }

    bool touchedTargets[2] = {};
    int writtenTargets = 0;
    for (int targetIndex = 0; targetIndex < 2; ++targetIndex) {
        if (!ValidateVisibilityTarget(globals, *targets[targetIndex],
                                      inventory.pawn, inventory.weapon)) break;
        const uintptr_t address = targets[targetIndex]->component + visibilityOffset;
        uint32_t before = 0;
        uint32_t after = 0;
        if (!UpdateAtomicBit(address, hiddenGameMask, true, before, after)) break;
        touchedTargets[targetIndex] = true;
        originalWords[targetIndex] = before;
        if ((after & hiddenGameMask) == 0) break;
        ++writtenTargets;
    }
    bool transactionValid = writtenTargets == 2;
    if (transactionValid) {
        const input::PlayerIdentitySnapshot committedIdentity =
            input::WeaponAimSystem::Instance().GetPlayerIdentity();
        transactionValid = !m_shuttingDown.load(std::memory_order_acquire) &&
            !m_stop.load(std::memory_order_acquire) &&
            m_enabled.load(std::memory_order_acquire) &&
            m_visibilityEnabled.load(std::memory_order_acquire) &&
            m_inventoryRequestGeneration.load(std::memory_order_acquire) ==
                inventoryRequestGeneration &&
            committedIdentity.pawnValid && committedIdentity.weaponValid &&
            committedIdentity.controller == inventory.controller &&
            committedIdentity.pawn == inventory.pawn &&
            committedIdentity.weapon == inventory.weapon;
        for (int targetIndex = 0; transactionValid && targetIndex < 2; ++targetIndex) {
            uint32_t committedWord = 0;
            transactionValid = ValidateVisibilityTarget(
                    globals, *targets[targetIndex], inventory.pawn, inventory.weapon) &&
                ReadAtomicWord(targets[targetIndex]->component + visibilityOffset,
                               committedWord) &&
                (committedWord & hiddenGameMask) != 0;
        }
    }
    if (!transactionValid) {
        bool rollbackFailed = false;
        for (int targetIndex = 0; targetIndex < 2; ++targetIndex) {
            if (!touchedTargets[targetIndex]) continue;
            if (!ValidateVisibilityTarget(globals, *targets[targetIndex],
                                          inventory.pawn, inventory.weapon)) {
                rollbackFailed = true;
                continue;
            }
            const uintptr_t address = targets[targetIndex]->component + visibilityOffset;
            uint32_t before = 0;
            uint32_t after = 0;
            if (!UpdateAtomicBit(address, hiddenGameMask,
                    (originalWords[targetIndex] & hiddenGameMask) != 0, before, after) ||
                (after & hiddenGameMask) !=
                    (originalWords[targetIndex] & hiddenGameMask)) rollbackFailed = true;
        }
        if (rollbackFailed) {
            m_visibilityActive = true;
            m_visibilityController = inventory.controller;
            m_visibilityPawn = inventory.pawn;
            m_visibilityWeapon = inventory.weapon;
            for (int targetIndex = 0; targetIndex < 2; ++targetIndex) {
                m_visibilityComponents[targetIndex] = touchedTargets[targetIndex] ?
                    targets[targetIndex]->component : 0;
                m_visibilityOriginalWords[targetIndex] = originalWords[targetIndex];
                m_visibilityOuters[targetIndex] = targets[targetIndex]->outer;
                m_visibilityClassObjects[targetIndex] = targets[targetIndex]->classObject;
                m_visibilityNameTokens[targetIndex] = targets[targetIndex]->objectNameToken;
                strcpy_s(m_visibilityObjectNames[targetIndex], targets[targetIndex]->objectName);
                strcpy_s(m_visibilityClassNames[targetIndex], targets[targetIndex]->className);
            }
            m_visibilityWordOffset = visibilityOffset;
            m_visibilityHiddenMask = hiddenGameMask;
            m_visibilityArmsMatrixOffset = targets[1]->localToWorldOffset;
            m_hiddenArmsComponent.store(targets[1]->component, std::memory_order_release);
        }
        ReleaseSRWLockExclusive(&m_visibilityLock);
        Log("[Visibility] Hide transaction rejected after %d/2 writes; rollback=%s",
            writtenTargets, rollbackFailed ? "INCOMPLETE (state retained)" : "complete");
        return false;
    }

    m_visibilityActive = true;
    m_visibilityController = inventory.controller;
    m_visibilityPawn = inventory.pawn;
    m_visibilityWeapon = inventory.weapon;
    m_visibilityComponents[0] = targets[0]->component;
    m_visibilityComponents[1] = targets[1]->component;
    m_visibilityOriginalWords[0] = originalWords[0];
    m_visibilityOriginalWords[1] = originalWords[1];
    for (int targetIndex = 0; targetIndex < 2; ++targetIndex) {
        m_visibilityOuters[targetIndex] = targets[targetIndex]->outer;
        m_visibilityClassObjects[targetIndex] = targets[targetIndex]->classObject;
        m_visibilityNameTokens[targetIndex] = targets[targetIndex]->objectNameToken;
        strcpy_s(m_visibilityObjectNames[targetIndex], targets[targetIndex]->objectName);
        strcpy_s(m_visibilityClassNames[targetIndex], targets[targetIndex]->className);
    }
    m_visibilityWordOffset = visibilityOffset;
    m_visibilityHiddenMask = hiddenGameMask;
    m_visibilityArmsMatrixOffset = targets[1]->localToWorldOffset;
    m_hiddenArmsComponent.store(targets[1]->component, std::memory_order_release);
    ReleaseSRWLockExclusive(&m_visibilityLock);
    Log("[Visibility] HiddenGame transaction committed: pawn=%p body=%p 0x%08X->0x%08X "
        "arms=%p 0x%08X->0x%08X weaponComponents=%d",
        reinterpret_cast<void*>(inventory.pawn),
        reinterpret_cast<void*>(targets[0]->component), originalWords[0],
        originalWords[0] | hiddenGameMask,
        reinterpret_cast<void*>(targets[1]->component), originalWords[1],
        originalWords[1] | hiddenGameMask, inventory.weaponComponentCount);
    return true;
}

void ArmIKSystem::RestoreVisibility() {
    AcquireSRWLockExclusive(&m_visibilityLock);
    if (!m_visibilityActive) {
        ReleaseSRWLockExclusive(&m_visibilityLock);
        return;
    }

    const camera::UE3Globals globals = camera::GetUE3GlobalsSnapshot();
    bool restoreFailed = false;
    int restored = 0;
    int dropped = 0;
    for (int targetIndex = 0; targetIndex < 2; ++targetIndex) {
        const uintptr_t component = m_visibilityComponents[targetIndex];
        uintptr_t currentOuter = 0;
        uintptr_t currentClass = 0;
        uint64_t currentNameToken = 0;
        char objectName[64] = {};
        char className[64] = {};
        if (!component) continue;
        if (!ReadMemory(component + globals.gObjectNameOffset - 8,
                        &currentOuter, sizeof(currentOuter)) ||
            !ReadMemory(component + globals.gObjectClassOffset,
                        &currentClass, sizeof(currentClass)) ||
            !ReadMemory(component + globals.gObjectNameOffset,
                        &currentNameToken, sizeof(currentNameToken)) ||
            !ReadObjectName(globals, component, objectName, sizeof(objectName)) ||
            !ReadClassName(globals, component, className, sizeof(className))) {
            ++dropped;
            continue;
        }
        if (currentOuter != m_visibilityOuters[targetIndex] ||
            currentClass != m_visibilityClassObjects[targetIndex] ||
            currentNameToken != m_visibilityNameTokens[targetIndex] ||
            strcmp(objectName, m_visibilityObjectNames[targetIndex]) != 0 ||
            strcmp(className, m_visibilityClassNames[targetIndex]) != 0) {
            ++dropped;
            continue;
        }
        if (!OuterChainContains(globals, component, m_visibilityPawn)) {
            ++dropped;
            continue;
        }
        const uintptr_t address = component + m_visibilityWordOffset;
        uint32_t before = 0;
        uint32_t after = 0;
        if (!UpdateAtomicBit(address, m_visibilityHiddenMask,
                (m_visibilityOriginalWords[targetIndex] & m_visibilityHiddenMask) != 0,
                before, after) ||
            (after & m_visibilityHiddenMask) !=
                (m_visibilityOriginalWords[targetIndex] & m_visibilityHiddenMask)) {
            restoreFailed = true;
            continue;
        }
        ++restored;
    }
    if (!restoreFailed) {
        m_visibilityActive = false;
        m_visibilityController = 0;
        m_visibilityPawn = 0;
        m_visibilityWeapon = 0;
        m_visibilityComponents[0] = 0;
        m_visibilityComponents[1] = 0;
        m_visibilityOriginalWords[0] = 0;
        m_visibilityOriginalWords[1] = 0;
        memset(m_visibilityOuters, 0, sizeof(m_visibilityOuters));
        memset(m_visibilityClassObjects, 0, sizeof(m_visibilityClassObjects));
        memset(m_visibilityNameTokens, 0, sizeof(m_visibilityNameTokens));
        memset(m_visibilityObjectNames, 0, sizeof(m_visibilityObjectNames));
        memset(m_visibilityClassNames, 0, sizeof(m_visibilityClassNames));
        m_visibilityWordOffset = -1;
        m_visibilityHiddenMask = 0;
        m_visibilityArmsMatrixOffset = 0;
        m_hiddenArmsComponent.store(0, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&m_visibilityLock);
    Log("[Visibility] Restore %s: restored=%d dropped=%d",
        restoreFailed ? "INCOMPLETE" : "complete", restored, dropped);
}

void ArmIKSystem::CheckVisibilityWatchdog() {
    uintptr_t controller = 0;
    uintptr_t pawn = 0;
    uintptr_t weapon = 0;
    uintptr_t body = 0;
    uintptr_t arms = 0;
    int matrixOffset = 0;
    int32_t wordOffset = -1;
    uint32_t hiddenMask = 0;
    AcquireSRWLockShared(&m_visibilityLock);
    if (m_visibilityActive) {
        controller = m_visibilityController;
        pawn = m_visibilityPawn;
        weapon = m_visibilityWeapon;
        body = m_visibilityComponents[0];
        arms = m_visibilityComponents[1];
        matrixOffset = m_visibilityArmsMatrixOffset;
        wordOffset = m_visibilityWordOffset;
        hiddenMask = m_visibilityHiddenMask;
    }
    ReleaseSRWLockShared(&m_visibilityLock);
    if (!body && !arms) return;

    const input::PlayerIdentitySnapshot identity =
        input::WeaponAimSystem::Instance().GetPlayerIdentity();
    const camera::UE3Globals globals = camera::GetUE3GlobalsSnapshot();
    bool shouldRestore = !identity.pawnValid ||
        identity.controller != controller || identity.pawn != pawn;
    const char* reason = shouldRestore ? "player identity changed" : nullptr;

    if (!shouldRestore && !identity.weaponValid) return;
    if (!shouldRestore && identity.weapon != weapon) {
        const bool targetsRemainPawnOnly =
            OuterChainContains(globals, body, pawn) &&
            OuterChainContains(globals, arms, pawn) &&
            !OuterChainContains(globals, body, identity.weapon) &&
            !OuterChainContains(globals, arms, identity.weapon);
        if (targetsRemainPawnOnly) {
            AcquireSRWLockExclusive(&m_visibilityLock);
            const bool transactionUnchanged = m_visibilityActive &&
                m_visibilityController == controller && m_visibilityPawn == pawn &&
                m_visibilityWeapon == weapon;
            if (transactionUnchanged) m_visibilityWeapon = identity.weapon;
            ReleaseSRWLockExclusive(&m_visibilityLock);
            if (transactionUnchanged) {
                Log("[Visibility] Preserving hidden pawn components across weapon change: %p -> %p",
                    reinterpret_cast<void*>(weapon), reinterpret_cast<void*>(identity.weapon));
                camera::RequestPlayerIdentityRefresh();
            }
            return;
        }
        shouldRestore = true;
        reason = "hidden target overlaps new weapon ownership";
    }

    if (!shouldRestore && (!body || !arms)) {
        shouldRestore = true;
        reason = "partial visibility transaction pending rollback";
    }

    if (!shouldRestore &&
        (!OuterChainContains(globals, body, pawn) ||
         !OuterChainContains(globals, arms, pawn))) {
        shouldRestore = true;
        reason = "hidden component ownership changed";
    }
    if (!shouldRestore) {
        uint32_t bodyWord = 0;
        uint32_t armsWord = 0;
        if (wordOffset < 0 || !hiddenMask ||
            !ReadAtomicWord(body + wordOffset, bodyWord) ||
            !ReadAtomicWord(arms + wordOffset, armsWord) ||
            (bodyWord & hiddenMask) == 0 || (armsWord & hiddenMask) == 0) {
            shouldRestore = true;
            reason = "HiddenGame state changed";
        }
    }

    float cameraPosition[3] = {};
    bool haveCamera = false;
    AcquireSRWLockShared(&m_cameraCacheLock);
    if (m_hasGoodCameraLocation) {
        memcpy(cameraPosition, m_goodCameraLocation, sizeof(cameraPosition));
        haveCamera = true;
    }
    ReleaseSRWLockShared(&m_cameraCacheLock);
    float localToWorld[16] = {};
    if (!shouldRestore && (!haveCamera || matrixOffset <= 0 ||
        !ReadMemory(arms + matrixOffset, localToWorld, sizeof(localToWorld)) ||
        !ValidateMatrix(localToWorld))) {
        shouldRestore = true;
        reason = "first-person arms transform unavailable";
    }
    if (!shouldRestore) {
        const float dx = localToWorld[12] - cameraPosition[0];
        const float dy = localToWorld[13] - cameraPosition[1];
        const float dz = localToWorld[14] - cameraPosition[2];
        const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (!std::isfinite(distance) || distance > 300.0f) {
            shouldRestore = true;
            reason = "first-person arms left camera range";
        }
    }
    if (!shouldRestore) return;
    Log("[Visibility] Watchdog requesting restore: %s", reason);
    RequestRescan();
}

uint64_t ArmIKSystem::UpdateTargets(const float cameraLocation[3], float gamePitchRadians,
                                    float gameYawRadians,
                                    const float trackingReferencePosition[3],
                                    const float trackingReferenceRotation[4]) {
    if (cameraLocation && std::isfinite(cameraLocation[0]) &&
        std::isfinite(cameraLocation[1]) && std::isfinite(cameraLocation[2])) {
        AcquireSRWLockExclusive(&m_cameraCacheLock);
        m_hasGoodCameraLocation = true;
        m_goodCameraLocation[0] = cameraLocation[0];
        m_goodCameraLocation[1] = cameraLocation[1];
        m_goodCameraLocation[2] = cameraLocation[2];
        ReleaseSRWLockExclusive(&m_cameraCacheLock);
    }
    input::ControllerState controllers[2] = {};
    uint64_t controllerGeneration = 0;
    input::XRInput::Instance().GetControllerSnapshot(controllers, &controllerGeneration);
    const Quat inverseReference{-trackingReferenceRotation[0],
        -trackingReferenceRotation[1], -trackingReferenceRotation[2],
        trackingReferenceRotation[3]};
    TargetSnapshot snapshot = {};
    if (m_simulationEnabled.load(std::memory_order_acquire)) {
        controllerGeneration = m_simulationGeneration.fetch_add(
            1, std::memory_order_relaxed) + 1;
        snapshot.controllerGeneration = controllerGeneration;
        snapshot.componentSpace = true;
        const float phase = static_cast<float>(GetTickCount64() % 100000) * 0.001f;
        const Vec3 localTargets[2] = {
            {42.0f, -24.0f + std::sin(phase * 1.3f) * 6.0f,
             -18.0f + std::cos(phase * 0.9f) * 5.0f},
            {48.0f, 24.0f + std::sin(phase) * 8.0f,
             -16.0f + std::cos(phase * 1.1f) * 6.0f},
        };
        for (int hand = 0; hand < 2; ++hand) {
            snapshot.position[hand] = localTargets[hand];
            const float handYaw = (hand == 0 ? -0.12f : 0.12f) +
                std::sin(phase * 0.7f) * 0.08f;
            const Vec3 forward{std::cos(handYaw), std::sin(handYaw),
                std::sin(phase * 0.8f) * 0.12f};
            snapshot.rotation[hand] = QuatLookAt(forward, {0.0f, 0.0f, 1.0f});
            snapshot.valid[hand] = true;
        }
    } else {
        snapshot.controllerGeneration = controllerGeneration;
        snapshot.cameraPosition = {cameraLocation[0], cameraLocation[1], cameraLocation[2]};
        snapshot.cameraRotation = QuatLookAt(
            RotatePitchYaw({1.0f, 0.0f, 0.0f}, gamePitchRadians, gameYawRadians),
            RotatePitchYaw({0.0f, 0.0f, 1.0f}, gamePitchRadians, gameYawRadians));
        for (int hand = 0; hand < 2; ++hand) {
            if (!controllers[hand].valid) continue;
            const Vec3 trackingDelta{
                controllers[hand].position[0] - trackingReferencePosition[0],
                controllers[hand].position[1] - trackingReferencePosition[1],
                controllers[hand].position[2] - trackingReferencePosition[2]};
            const Vec3 localXr = RotateByQuat(inverseReference, trackingDelta);
            const float worldScale = 100.0f * config::Get().positional_scale;
            const Vec3 worldOffset = RotateYaw(XrToUe(localXr), gameYawRadians) * worldScale;
            snapshot.position[hand] = {cameraLocation[0] + worldOffset.x,
                cameraLocation[1] + worldOffset.y, cameraLocation[2] + worldOffset.z};
            const float* handRotation = hand == 1 && controllers[hand].aimValid
                ? controllers[hand].aimRotation : controllers[hand].rotation;
            snapshot.rotation[hand] = XrControllerToWorld(
                handRotation, inverseReference,
                gamePitchRadians, gameYawRadians);
            snapshot.valid[hand] = true;
        }
    }
    AcquireSRWLockExclusive(&m_targetLock);
    if (!m_targets) {
        ReleaseSRWLockExclusive(&m_targetLock);
        return 0;
    }
    m_targets[controllerGeneration % kTargetHistorySize] = snapshot;
    ReleaseSRWLockExclusive(&m_targetLock);
    m_latestTargetGeneration.store(controllerGeneration, std::memory_order_release);
    return controllerGeneration;
}

void ArmIKSystem::SetRenderContext(uint64_t renderGeneration,
                                   uint64_t targetGeneration) {
    m_renderTargetGeneration.store(targetGeneration, std::memory_order_release);
    m_renderGeneration.store(renderGeneration, std::memory_order_release);
}

bool ArmIKSystem::ApplyPostAnimation(void* component) {
    if (!component || !m_poseHookInstalled.load(std::memory_order_acquire)) return false;
    uintptr_t targetComponent = 0;
    AcquireSRWLockShared(&m_rigLock);
    if (m_rig && m_rig->valid) targetComponent = m_rig->component;
    ReleaseSRWLockShared(&m_rigLock);
    if (reinterpret_cast<uintptr_t>(component) != targetComponent) return false;
    const uint64_t renderGeneration = m_renderGeneration.load(std::memory_order_acquire);
    const uint64_t targetGeneration = m_renderTargetGeneration.load(std::memory_order_acquire);
    return renderGeneration != 0 && targetGeneration != 0 &&
        Apply(renderGeneration, targetGeneration, false);
}

bool ArmIKSystem::Apply(uint64_t renderGeneration, uint64_t targetGeneration,
                         bool restoreAfterRender) {
    if (!m_enabled.load()) return false;
    Rig rig = {};
    TargetSnapshot targets = {};
    AcquireSRWLockShared(&m_rigLock);
    if (m_rig) rig = *m_rig;
    ReleaseSRWLockShared(&m_rigLock);
    AcquireSRWLockShared(&m_targetLock);
    if (m_targets) targets = m_targets[targetGeneration % kTargetHistorySize];
    ReleaseSRWLockShared(&m_targetLock);
    if (!rig.valid || targets.controllerGeneration != targetGeneration ||
        (!targets.valid[0] && !targets.valid[1]) ||
        rig.boneCount <= 0 || rig.boneCount > 256) return false;
    const input::PlayerIdentitySnapshot identity =
        input::WeaponAimSystem::Instance().GetPlayerIdentity();
    if (!identity.pawnValid || identity.controller != rig.localController ||
        identity.pawn != rig.localPawn) return false;
    TArray64 livePose = {};
    const bool poseStillOwned = ReadMemory(rig.component + rig.componentPoseOffset,
        &livePose, sizeof(livePose)) && livePose.data == rig.componentPose &&
        livePose.count == rig.boneCount && livePose.capacity >= livePose.count;
    float localToWorld[16] = {};
    if (!poseStillOwned || !ReadMemory(rig.component + rig.matrixOffset,
        localToWorld, sizeof(localToWorld)) || !ValidateMatrix(localToWorld)) return false;

    // AER renders the two eyes on consecutive game frames. Re-stamp the exact
    // eye-zero palette so animation/controller updates cannot ghost between eyes.
    if (rig.cachedPoseGeneration == renderGeneration) {
        const bool compensateComponentMotion = rig.cachedLocalToWorldValid &&
            ValidateMatrix(rig.cachedLocalToWorld);
        const Quat componentDelta = compensateComponentMotion
            ? QuatMultiply(MatrixRotation(localToWorld).conjugate(),
                           MatrixRotation(rig.cachedLocalToWorld)).normalized()
            : Quat{};
        for (int bone = 0; bone < rig.boneCount; ++bone) {
            if (!rig.cachedPoseBone[bone]) continue;
            BoneMatrix replay = rig.cachedPose[bone];
            if (compensateComponentMotion) {
                const Vec3 cachedPosition{replay.values[12], replay.values[13],
                    replay.values[14]};
                const Vec3 cachedWorldPosition{
                    rig.cachedLocalToWorld[0]*cachedPosition.x +
                        rig.cachedLocalToWorld[4]*cachedPosition.y +
                        rig.cachedLocalToWorld[8]*cachedPosition.z +
                        rig.cachedLocalToWorld[12],
                    rig.cachedLocalToWorld[1]*cachedPosition.x +
                        rig.cachedLocalToWorld[5]*cachedPosition.y +
                        rig.cachedLocalToWorld[9]*cachedPosition.z +
                        rig.cachedLocalToWorld[13],
                    rig.cachedLocalToWorld[2]*cachedPosition.x +
                        rig.cachedLocalToWorld[6]*cachedPosition.y +
                        rig.cachedLocalToWorld[10]*cachedPosition.z +
                        rig.cachedLocalToWorld[14]};
                const Vec3 currentWorldDelta = cachedWorldPosition -
                    Vec3{localToWorld[12], localToWorld[13], localToWorld[14]};
                BoneTransform compensated;
                compensated.position = {
                    localToWorld[0]*currentWorldDelta.x +
                        localToWorld[1]*currentWorldDelta.y +
                        localToWorld[2]*currentWorldDelta.z,
                    localToWorld[4]*currentWorldDelta.x +
                        localToWorld[5]*currentWorldDelta.y +
                        localToWorld[6]*currentWorldDelta.z,
                    localToWorld[8]*currentWorldDelta.x +
                        localToWorld[9]*currentWorldDelta.y +
                        localToWorld[10]*currentWorldDelta.z};
                compensated.rotation = QuatMultiply(componentDelta,
                    MatrixRotation(replay.values)).normalized();
                SetMatrixTransform(replay, compensated);
            }
            if (!WriteMemory(rig.componentPose + static_cast<uintptr_t>(bone) *
                                 rig.componentPoseStride,
                             &replay, sizeof(BoneMatrix))) return false;
        }
        return true;
    }

    std::array<BoneTransform, 256> original = {};
    std::array<BoneTransform, 256> solved = {};
    std::array<BoneMatrix, 256> originalMatrices = {};
    std::array<int32_t, 256> parents = {};
    for (int bone = 0; bone < rig.boneCount; ++bone) {
        if (!ReadMemory(rig.componentPose + static_cast<uintptr_t>(bone) *
                            rig.componentPoseStride,
                        &originalMatrices[bone], sizeof(BoneMatrix)) ||
            !ValidateMatrix(originalMatrices[bone].values) ||
            !ReadMemory(rig.refSkeleton + static_cast<uintptr_t>(bone) * rig.refStride +
                rig.parentOffset, &parents[bone], sizeof(parents[bone]))) return false;
        original[bone].rotation = MatrixRotation(originalMatrices[bone].values);
        original[bone].position = {originalMatrices[bone].values[12],
            originalMatrices[bone].values[13], originalMatrices[bone].values[14]};
        solved[bone] = original[bone];
    }

    auto solveArm = [&](int hand, int shoulder, int elbow, int wrist) {
        if (!targets.valid[hand] || shoulder < 0 || elbow < 0 || wrist < 0 ||
            shoulder >= rig.boneCount || elbow >= rig.boneCount || wrist >= rig.boneCount)
            return false;
        Vec3 targetComponent = targets.position[hand];
        Quat targetRotation = targets.rotation[hand];
        Quat calibrationReferenceRotation = targetRotation;
        if (!targets.componentSpace) {
            const Vec3 worldDelta = targets.position[hand] -
                Vec3{localToWorld[12], localToWorld[13], localToWorld[14]};
            targetComponent = {
                localToWorld[0]*worldDelta.x + localToWorld[1]*worldDelta.y + localToWorld[2]*worldDelta.z,
                localToWorld[4]*worldDelta.x + localToWorld[5]*worldDelta.y + localToWorld[6]*worldDelta.z,
                localToWorld[8]*worldDelta.x + localToWorld[9]*worldDelta.y + localToWorld[10]*worldDelta.z};
            const Quat componentWorld = MatrixRotation(localToWorld);
            targetRotation = QuatMultiply(componentWorld.conjugate(),
                targets.rotation[hand]).normalized();
            calibrationReferenceRotation = QuatMultiply(componentWorld.conjugate(),
                targets.cameraRotation).normalized();
        }
        const Vec3 oldShoulder = original[shoulder].position;
        const Vec3 oldElbow = original[elbow].position;
        const Vec3 oldWrist = original[wrist].position;
        const float armReach = (oldElbow - oldShoulder).length() +
            (oldWrist - oldElbow).length();
        if (!targets.componentSpace &&
            (targetComponent - oldShoulder).length() > armReach * 3.0f) {
            const Vec3 worldOffset = targets.position[hand] - targets.cameraPosition;
            const Vec3 componentOffset{
                -(localToWorld[0]*worldOffset.x + localToWorld[1]*worldOffset.y +
                    localToWorld[2]*worldOffset.z),
                localToWorld[4]*worldOffset.x + localToWorld[5]*worldOffset.y +
                    localToWorld[6]*worldOffset.z,
                localToWorld[8]*worldOffset.x + localToWorld[9]*worldOffset.y +
                    localToWorld[10]*worldOffset.z};
            const int oppositeShoulder = hand == 0 ? rig.rightShoulder : rig.leftShoulder;
            Vec3 shoulderCenter = oldShoulder;
            if (oppositeShoulder >= 0 && oppositeShoulder < rig.boneCount)
                shoulderCenter = (oldShoulder + original[oppositeShoulder].position) * 0.5f;
            Vec3 trackingOrigin = rig.viewmodelTrackingOriginValid
                ? rig.viewmodelTrackingOrigin
                : shoulderCenter + Vec3{0.0f, 0.0f, 22.0f};
            if (!rig.viewmodelTrackingOriginValid) {
                rig.viewmodelTrackingOrigin = trackingOrigin;
                rig.viewmodelTrackingOriginValid = true;
                AcquireSRWLockExclusive(&m_rigLock);
                if (m_rig && m_rig->component == rig.component &&
                    !m_rig->viewmodelTrackingOriginValid) {
                    m_rig->viewmodelTrackingOrigin = trackingOrigin;
                    m_rig->viewmodelTrackingOriginValid = true;
                }
                ReleaseSRWLockExclusive(&m_rigLock);
            }
            targetComponent = trackingOrigin + componentOffset;
            static std::atomic<uint32_t> anchoredTargetLogs{0};
            if (anchoredTargetLogs.fetch_add(1, std::memory_order_relaxed) < 2) {
                Log("[ArmIK] Controller target anchored to viewmodel shoulders: hand=%d "
                    "origin=(%.1f,%.1f,%.1f) offset=(%.1f,%.1f,%.1f)", hand,
                    trackingOrigin.x, trackingOrigin.y, trackingOrigin.z,
                    componentOffset.x, componentOffset.y, componentOffset.z);
            }
        }
        TwoBoneIKInput input;
        input.shoulder = oldShoulder;
        input.handTarget = targetComponent;
        input.handTargetRotation = targetRotation;
        input.upperArmLength = (oldElbow - oldShoulder).length();
        input.forearmLength = (oldWrist - oldElbow).length();
        // Preserve the animation's natural bend plane. A fixed component-axis
        // pole twists rigs whose local handedness differs between characters.
        input.poleHint = oldElbow;
        input.useHandRotation = true;
        const TwoBoneIKResult result = SolveTwoBoneIK(input);
        if (!result.valid) return false;

        static std::atomic<uint32_t> solveLogs{0};
        if (solveLogs.fetch_add(1, std::memory_order_relaxed) < 2) {
            Log("[ArmIK] Natural-pole solve: hand=%d lengths=%.2f+%.2f "
                "shoulder=(%.1f,%.1f,%.1f) elbow=(%.1f,%.1f,%.1f) "
                "target=(%.1f,%.1f,%.1f)", hand,
                input.upperArmLength, input.forearmLength,
                oldShoulder.x, oldShoulder.y, oldShoulder.z,
                oldElbow.x, oldElbow.y, oldElbow.z,
                targetComponent.x, targetComponent.y, targetComponent.z);
        }

        auto descendantOf = [&](int bone, int ancestor) {
            int current = bone;
            for (int depth = 0; depth <= rig.boneCount; ++depth) {
                if (current == ancestor) return true;
                if (current < 0 || current >= rig.boneCount) return false;
                const int parent = parents[current];
                if (parent == current || (current == 0 && parent == 0)) return false;
                current = parent;
            }
            return false;
        };

        const Quat upperDelta = FromTo(oldElbow - oldShoulder,
            result.elbow - oldShoulder);
        for (int bone = 0; bone < rig.boneCount; ++bone) {
            if (!descendantOf(bone, shoulder)) continue;
            solved[bone].position = oldShoulder + RotateByQuat(
                upperDelta, original[bone].position - oldShoulder);
            solved[bone].rotation = QuatMultiply(
                upperDelta, original[bone].rotation).normalized();
        }

        const Quat forearmDelta = FromTo(solved[wrist].position - result.elbow,
            targetComponent - result.elbow);
        for (int bone = 0; bone < rig.boneCount; ++bone) {
            if (!descendantOf(bone, elbow)) continue;
            solved[bone].position = result.elbow + RotateByQuat(
                forearmDelta, solved[bone].position - result.elbow);
            solved[bone].rotation = QuatMultiply(
                forearmDelta, solved[bone].rotation).normalized();
        }
        Quat wristCalibration = rig.wristCalibration[hand];
        if (!rig.wristCalibrationValid[hand]) {
            wristCalibration = QuatMultiply(calibrationReferenceRotation.conjugate(),
                original[wrist].rotation).normalized();
            rig.wristCalibration[hand] = wristCalibration;
            rig.wristCalibrationValid[hand] = true;
            AcquireSRWLockExclusive(&m_rigLock);
            if (m_rig && m_rig->component == rig.component) {
                m_rig->wristCalibration[hand] = wristCalibration;
                m_rig->wristCalibrationValid[hand] = true;
            }
            ReleaseSRWLockExclusive(&m_rigLock);
        }
        const Quat desiredWristRotation = QuatMultiply(targetRotation,
            wristCalibration).normalized();
        const Quat wristDelta = QuatMultiply(desiredWristRotation,
            solved[wrist].rotation.conjugate()).normalized();
        const Vec3 solvedWrist = solved[wrist].position;
        for (int bone = 0; bone < rig.boneCount; ++bone) {
            if (!descendantOf(bone, wrist)) continue;
            solved[bone].position = solvedWrist +
                RotateByQuat(wristDelta, solved[bone].position - solvedWrist);
            solved[bone].rotation = QuatMultiply(wristDelta,
                solved[bone].rotation).normalized();
        }
        return true;
    };

    const bool rightSolved = solveArm(1, rig.rightShoulder, rig.rightElbow, rig.rightWrist);
    const bool leftSolved = solveArm(0, rig.leftShoulder, rig.leftElbow, rig.leftWrist);
    if (!rightSolved && !leftSolved) return false;

    auto armBone = [&](int bone) {
        auto descendant = [&](int ancestor) {
            int current = bone;
            for (int depth = 0; depth <= rig.boneCount; ++depth) {
                if (current == ancestor) return true;
                if (current < 0 || current >= rig.boneCount) return false;
                const int parent = parents[current];
                if (parent == current || (current == 0 && parent == 0)) return false;
                current = parent;
            }
            return false;
        };
        return (rightSolved && descendant(rig.rightShoulder)) ||
            (leftSolved && descendant(rig.leftShoulder));
    };

    const input::PlayerIdentitySnapshot finalIdentity =
        input::WeaponAimSystem::Instance().GetPlayerIdentity();
    AcquireSRWLockExclusive(&m_rigLock);
    TArray64 finalPose = {};
    TArray64 finalSkeleton = {};
    uintptr_t finalMesh = 0;
    const bool ownershipStillValid =
        ReadMemory(rig.component + rig.componentPoseOffset, &finalPose, sizeof(finalPose)) &&
        ReadMemory(rig.component + rig.skeletalMeshOffset, &finalMesh, sizeof(finalMesh)) &&
        finalMesh == rig.skeletalMesh &&
        ReadMemory(finalMesh + rig.refSkeletonOffset, &finalSkeleton, sizeof(finalSkeleton)) &&
        finalPose.data == rig.componentPose && finalPose.count == rig.boneCount &&
        finalPose.capacity >= finalPose.count &&
        finalSkeleton.data == rig.refSkeleton && finalSkeleton.count == rig.boneCount &&
        finalSkeleton.capacity >= finalSkeleton.count;
    if (!finalIdentity.pawnValid || finalIdentity.controller != rig.localController ||
        finalIdentity.pawn != rig.localPawn ||
        !m_enabled.load(std::memory_order_acquire) || !m_rig ||
        m_rig->component != rig.component || m_rig->componentPose != rig.componentPose ||
        m_rig->skeletalMesh != rig.skeletalMesh || m_rig->refSkeleton != rig.refSkeleton ||
        m_rig->backupValid || !ownershipStillValid) {
        ReleaseSRWLockExclusive(&m_rigLock);
        return false;
    }
    for (int bone = 0; bone < rig.boneCount; ++bone) {
        if (!armBone(bone)) continue;
        m_rig->backup[bone] = originalMatrices[bone];
        m_rig->backupBone[bone] = true;
    }
    m_rig->backupValid = true;
    memset(m_rig->cachedPoseBone, 0, sizeof(m_rig->cachedPoseBone));
    for (int bone = 0; bone < rig.boneCount; ++bone) {
        if (!armBone(bone)) continue;
        BoneMatrix output = originalMatrices[bone];
        SetMatrixTransform(output, solved[bone]);
        if (!WriteMemory(rig.componentPose + static_cast<uintptr_t>(bone) *
                             rig.componentPoseStride,
                         &output, sizeof(output))) {
            ReleaseSRWLockExclusive(&m_rigLock);
            Restore();
            return false;
        }
        m_rig->cachedPose[bone] = output;
        m_rig->cachedPoseBone[bone] = true;
    }
    m_rig->solvedGeneration = renderGeneration;
    m_rig->cachedPoseGeneration = renderGeneration;
    memcpy(m_rig->cachedLocalToWorld, localToWorld,
           sizeof(m_rig->cachedLocalToWorld));
    m_rig->cachedLocalToWorldValid = true;
    if (!restoreAfterRender) {
        memset(m_rig->backupBone, 0, sizeof(m_rig->backupBone));
        m_rig->backupValid = false;
    }
    ReleaseSRWLockExclusive(&m_rigLock);
    return true;
}

void ArmIKSystem::Restore() {
    AcquireSRWLockExclusive(&m_rigLock);
    if (!m_rig || !m_rig->backupValid) {
        ReleaseSRWLockExclusive(&m_rigLock);
        return;
    }
    TArray64 livePose = {};
    const bool stillOwned = ReadMemory(
        m_rig->component + m_rig->componentPoseOffset, &livePose, sizeof(livePose)) &&
        livePose.data == m_rig->componentPose && livePose.count == m_rig->boneCount;
    if (!stillOwned) {
        memset(m_rig->backupBone, 0, sizeof(m_rig->backupBone));
        m_rig->backupValid = false;
        m_rig->valid = false;
        ReleaseSRWLockExclusive(&m_rigLock);
        Log("[ArmIK] Pose ownership changed during render; restore discarded and rig invalidated");
        return;
    }
    bool restoreFailed = false;
    for (int bone = 0; bone < m_rig->boneCount; ++bone) {
        if (!m_rig->backupBone[bone]) continue;
        const BoneMatrix& backup = m_rig->backup[bone];
        if (!ValidateMatrix(backup.values)) continue;
        if (WriteMemory(m_rig->componentPose + static_cast<uintptr_t>(bone) *
                            m_rig->componentPoseStride,
                        &backup, sizeof(backup))) {
            m_rig->backupBone[bone] = false;
        } else {
            restoreFailed = true;
        }
    }
    m_rig->backupValid = restoreFailed;
    if (restoreFailed) {
        m_rig->valid = false;
        m_enabled = false;
    }
    ReleaseSRWLockExclusive(&m_rigLock);
    if (restoreFailed) Log("[ArmIK] Restore failed; IK disabled and rig invalidated");
}

ArmRigStatus ArmIKSystem::GetStatus() const {
    ArmRigStatus status;
    status.poseHookInstalled = m_poseHookInstalled.load(std::memory_order_acquire);
    status.poseHookCalls = m_poseHookCalls.load(std::memory_order_relaxed);
    status.poseHookApplies = m_poseHookApplies.load(std::memory_order_relaxed);
    AcquireSRWLockShared(&m_rigLock);
    if (m_rig) {
        status.rigValid = m_rig->valid;
        status.component = m_rig->component;
        status.skeletalMesh = m_rig->skeletalMesh;
        status.componentPose = m_rig->componentPose;
        status.boneCount = m_rig->boneCount;
        status.rightShoulder = m_rig->rightShoulder;
        status.rightElbow = m_rig->rightElbow;
        status.rightWrist = m_rig->rightWrist;
        status.leftShoulder = m_rig->leftShoulder;
        status.leftElbow = m_rig->leftElbow;
        status.leftWrist = m_rig->leftWrist;
        status.solvedGeneration = m_rig->solvedGeneration;
    }
    ReleaseSRWLockShared(&m_rigLock);
    return status;
}

ComponentInventoryStatus ArmIKSystem::GetComponentInventory() const {
    ComponentInventoryStatus inventory;
    AcquireSRWLockShared(&m_inventoryLock);
    inventory = m_inventory;
    ReleaseSRWLockShared(&m_inventoryLock);
    return inventory;
}

} // namespace bl1gotyvr::player
