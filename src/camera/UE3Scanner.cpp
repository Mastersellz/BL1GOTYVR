#include "UE3Scanner.hpp"
#include "SignatureScanner.hpp"
#include "../core/VRMod.hpp"
#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>
#include <unordered_set>

#pragma comment(lib, "psapi.lib")

namespace bl1gotyvr { namespace camera {

// 64-bit UE3 TArray: { pointer data, int32 count, int32 capacity }
struct TArray64 {
    uint64_t data;
    int32_t count;
    int32_t capacity;
};

static bool ReadMem(uintptr_t addr, void* buf, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, buf, size, &read) && read == size;
}

static bool IsPrintableString(uintptr_t addr) {
    char c;
    for (int i = 0; i < 64; i++) {
        if (!ReadMem(addr + i, &c, 1)) return false;
        if (c == 0) return i > 2;
        if (c < 0x20 || c > 0x7E) return false;
    }
    return false;
}

static bool ReadString(uintptr_t addr, char* out, size_t maxLen) {
    for (size_t i = 0; i < maxLen; i++) {
        if (!ReadMem(addr + i, &out[i], 1)) return false;
        if (out[i] == 0) return true;
        if (out[i] < 0x20 || out[i] > 0x7E) return false;
    }
    return false;
}

static bool ReadNameByIndex(const TArray64& names, int stringOffset, int32_t index,
                            char* out, size_t maxLen) {
    if (index < 0 || index >= names.count || stringOffset < 0) return false;
    uint64_t entry = 0;
    return ReadMem(names.data + static_cast<uint64_t>(index) * 8, &entry, sizeof(entry)) && entry &&
           ReadString(entry + stringOffset, out, maxLen);
}

// Check if a pointer looks like a valid UObject vtable
// UObject vtable should point into the .text section of a game module
static bool IsUObjectVtable(uintptr_t vtable, uintptr_t modBase, DWORD modSize) {
    if (!vtable) return false;
    uintptr_t firstMethod = 0;
    if (!ReadMem(vtable, &firstMethod, sizeof(firstMethod)) || !firstMethod) return false;
    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(reinterpret_cast<void*>(firstMethod), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (memory.Protect & executable) != 0;
}

// Score a TArray64 candidate as GNames (entries point to printable strings)
static int ScoreGNamesCandidate(const TArray64* arr, uintptr_t modBase, DWORD modSize) {
    if (arr->count < 1000 || arr->count > 5000000) return 0;
    if (arr->capacity < arr->count || arr->capacity > 8000000) return 0;
    if (!arr->data) return 0;

    static const int offsets[] = { 0, 4, 8, 12, 16, 20, 24 };
    int samples = 0;
    int matches[_countof(offsets)] = {};
    // Test 32 evenly-spaced entries
    for (int slot = 0; slot < 32; slot++) {
        int32_t index = (int32_t)(((int64_t)slot * (arr->count - 1)) / 31);
        uint64_t entry = 0;
        if (!ReadMem(arr->data + (uint64_t)index * 8, &entry, sizeof(entry)) || !entry) continue;
        samples++;
        for (int offsetIndex = 0; offsetIndex < _countof(offsets); ++offsetIndex) {
            if (IsPrintableString(entry + offsets[offsetIndex])) matches[offsetIndex]++;
        }
    }
    int best = 0;
    for (int offsetIndex = 1; offsetIndex < _countof(offsets); ++offsetIndex)
        if (matches[offsetIndex] > matches[best]) best = offsetIndex;
    return samples >= 8 ? matches[best] * 100 / samples : 0;
}

// Detect string offset within FNameEntry for 64-bit UE3
static int DetectFNameStringOffset(const TArray64* arr) {
    static const int offsets[] = { 0, 4, 8, 12, 16, 20, 24 };
    int scores[7] = {0};
    for (int slot = 0; slot < 64; slot++) {
        int32_t index = (int32_t)(((int64_t)slot * (arr->count - 1)) / 63);
        uint64_t entry = 0;
        if (!ReadMem(arr->data + (uint64_t)index * 8, &entry, sizeof(entry)) || !entry) continue;
        for (int o = 0; o < 7; o++) {
            if (IsPrintableString(entry + offsets[o])) scores[o]++;
        }
    }
    int best = 0;
    for (int o = 1; o < 7; o++) {
        if (scores[o] > scores[best]) best = o;
    }
    return scores[best] >= 16 ? offsets[best] : -1;
}

// Validate GNames by checking for known class name at a known index
// BL1 Enhanced uses "WillowPlayerController" or similar
static bool ValidateGNames(const TArray64* arr, int stringOffset) {
    if (stringOffset < 0 || arr->count <= 0) return false;

    // Scan for camera-related class names
    static const char* targets[] = {
        "PlayerController", "PlayerCameraManager", "WillowPlayerController",
        "WillowGameInfo", "WillowPawn", "WillowPlayerPawn"
    };

    for (int32_t i = 0; i < arr->count && i < 500000; i++) {
        uint64_t entry = 0;
        if (!ReadMem(arr->data + (uint64_t)i * 8, &entry, sizeof(entry)) || !entry) continue;
        char name[128];
        if (!ReadString(entry + stringOffset, name, sizeof(name))) continue;
        for (auto* target : targets) {
            if (strstr(name, target)) {
                Log("[UE3Scanner] GNames validated: index=%d name='%s'", i, name);
                return true;
            }
        }
    }
    return false;
}

// Score a TArray64 candidate as GObjects (entries have UObject-like layout)
static int ScoreGObjectsCandidate(const TArray64* arr, uintptr_t modBase, DWORD modSize) {
    if (arr->count < 10000 || arr->count > 5000000) return 0;
    if (arr->capacity < arr->count || arr->capacity > 8000000) return 0;
    if (!arr->data) return 0;

    int samples = 0, matches = 0;
    for (int slot = 0; slot < 32; slot++) {
        int32_t index = (int32_t)(((int64_t)slot * (arr->count - 1)) / 31);
        uint64_t object = 0;
        if (!ReadMem(arr->data + (uint64_t)index * 8, &object, sizeof(object)) || !object) continue;
        samples++;
        uint64_t vtable = 0;
        if (ReadMem(object, &vtable, sizeof(vtable)) && IsUObjectVtable(vtable, modBase, modSize))
            matches++;
    }
    return samples >= 8 ? matches * 100 / samples : 0;
}

static int DetectUObjectNameOffset(const TArray64& objects, const TArray64& names,
                                   int stringOffset, uintptr_t modBase, DWORD modSize) {
    constexpr int kFirstOffset = 8;
    constexpr int kLastOffset = 0x80;
    constexpr int kOffsetCount = (kLastOffset - kFirstOffset) / 4 + 1;
    int scores[kOffsetCount] = {};
    std::unordered_set<int32_t> uniqueNameIndices[kOffsetCount];
    int validObjects = 0;

    for (int slot = 0; slot < 128; ++slot) {
        const int32_t objectIndex = static_cast<int32_t>(
            (static_cast<int64_t>(slot) * (objects.count - 1)) / 127);
        uint64_t object = 0;
        uint64_t vtable = 0;
        unsigned char header[kLastOffset + 8] = {};
        if (!ReadMem(objects.data + static_cast<uint64_t>(objectIndex) * 8,
                     &object, sizeof(object)) || !object ||
            !ReadMem(object, header, sizeof(header))) {
            continue;
        }
        memcpy(&vtable, header, sizeof(vtable));
        if (!IsUObjectVtable(vtable, modBase, modSize)) continue;
        ++validObjects;

        for (int offset = kFirstOffset; offset <= kLastOffset; offset += 4) {
            int32_t nameIndex = -1;
            int32_t nameNumber = -1;
            memcpy(&nameIndex, header + offset, sizeof(nameIndex));
            memcpy(&nameNumber, header + offset + 4, sizeof(nameNumber));
            char name[128] = {};
            if (nameNumber >= 0 && nameNumber < 100000 &&
                ReadNameByIndex(names, stringOffset, nameIndex, name, sizeof(name))) {
                const int offsetIndex = (offset - kFirstOffset) / 4;
                ++scores[offsetIndex];
                uniqueNameIndices[offsetIndex].insert(nameIndex);
            }
        }
    }

    if (validObjects < 16) return -1;
    int bestIndex = 0;
    for (int i = 1; i < kOffsetCount; ++i) {
        if (scores[i] > scores[bestIndex] ||
            (scores[i] == scores[bestIndex] &&
             uniqueNameIndices[i].size() > uniqueNameIndices[bestIndex].size())) {
            bestIndex = i;
        }
    }
    const int bestOffset = kFirstOffset + bestIndex * 4;
    const int confidence = scores[bestIndex] * 100 / validObjects;
    Log("[UE3Scanner] UObject FName consensus: offset=0x%X score=%d/%d (%d%%) unique=%zu",
        bestOffset, scores[bestIndex], validObjects, confidence, uniqueNameIndices[bestIndex].size());
    return confidence >= 60 && uniqueNameIndices[bestIndex].size() >= 8 ? bestOffset : -1;
}

static bool ReadObjectNameAtOffset(const TArray64& names, int stringOffset,
                                   uintptr_t object, int nameOffset,
                                   char* out, size_t outSize, int32_t* nameNumber = nullptr) {
    if (!object || nameOffset < 0) return false;
    int32_t nameIndex = -1;
    int32_t number = 0;
    if (!ReadMem(object + nameOffset, &nameIndex, sizeof(nameIndex)) ||
        !ReadMem(object + nameOffset + 4, &number, sizeof(number)) ||
        number < 0 || number >= 100000 ||
        !ReadNameByIndex(names, stringOffset, nameIndex, out, outSize)) {
        return false;
    }
    if (nameNumber) *nameNumber = number;
    return true;
}

static int DetectUObjectClassOffset(const TArray64& objects, const TArray64& names,
                                    int stringOffset, int nameOffset) {
    constexpr int kFirstOffset = 8;
    constexpr int kLastOffset = 0x80;
    constexpr int kOffsetCount = (kLastOffset - kFirstOffset) / 8 + 1;
    int scores[kOffsetCount] = {};
    int validObjects = 0;

    for (int slot = 0; slot < 128; ++slot) {
        const int32_t objectIndex = static_cast<int32_t>(
            (static_cast<int64_t>(slot) * (objects.count - 1)) / 127);
        uint64_t object = 0;
        char objectName[128] = {};
        if (!ReadMem(objects.data + static_cast<uint64_t>(objectIndex) * 8,
                     &object, sizeof(object)) || !object ||
            !ReadObjectNameAtOffset(names, stringOffset, object, nameOffset,
                                    objectName, sizeof(objectName))) {
            continue;
        }
        ++validObjects;

        for (int offset = kFirstOffset; offset <= kLastOffset; offset += 8) {
            uint64_t classObject = 0;
            uint64_t metaClass = 0;
            char metaClassName[128] = {};
            if (!ReadMem(object + offset, &classObject, sizeof(classObject)) || !classObject ||
                !ReadMem(classObject + offset, &metaClass, sizeof(metaClass)) || !metaClass ||
                !ReadObjectNameAtOffset(names, stringOffset, metaClass, nameOffset,
                                        metaClassName, sizeof(metaClassName))) {
                continue;
            }
            if (strcmp(metaClassName, "Class") == 0) {
                ++scores[(offset - kFirstOffset) / 8];
            }
        }
    }

    if (validObjects < 16) return -1;
    int bestIndex = 0;
    for (int i = 1; i < kOffsetCount; ++i) {
        if (scores[i] > scores[bestIndex]) bestIndex = i;
    }
    const int bestOffset = kFirstOffset + bestIndex * 8;
    const int confidence = scores[bestIndex] * 100 / validObjects;
    Log("[UE3Scanner] UObject Class consensus: offset=0x%X score=%d/%d (%d%%)",
        bestOffset, scores[bestIndex], validObjects, confidence);
    return confidence >= 60 ? bestOffset : -1;
}

// Scan all writable PE sections for TArray64 candidates
static void ScanForUE3GlobalsImpl(UE3Globals* globals, uintptr_t modBase, DWORD modSize) {
    Log("[UE3Scanner] Scanning for GNames and GObjects...");

    // Parse PE sections
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)modBase;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(modBase + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);

    struct ArrayCandidate {
        uintptr_t address;
        TArray64 array;
        int nameScore;
        int objectScore;
    };

    ArrayCandidate bestNames = {};
    ArrayCandidate bestObjects = {};

    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER& sec = sections[i];
        if (!(sec.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;

        uintptr_t secStart = modBase + sec.VirtualAddress;
        DWORD secSize = sec.Misc.VirtualSize;
        if (secSize < sizeof(TArray64)) continue;

        Log("[UE3Scanner] Scanning section %.8s (0x%p, 0x%X bytes, attrs=0x%X)",
            sec.Name, (void*)secStart, secSize, sec.Characteristics);

        // Scan every 8 bytes (aligned for 64-bit pointers)
        for (uintptr_t addr = secStart; addr + sizeof(TArray64) <= secStart + secSize; addr += 8) {
            TArray64 candidate = {};
            if (!ReadMem(addr, &candidate, sizeof(candidate))) continue;

            TArray64 candidates[2] = { candidate, {} };
            uintptr_t candidateAddresses[2] = { addr, 0 };
            uint64_t indirect = 0;
            if (ReadMem(addr, &indirect, sizeof(indirect)) && indirect &&
                ReadMem(indirect, &candidates[1], sizeof(TArray64))) {
                candidateAddresses[1] = static_cast<uintptr_t>(indirect);
            }
            for (int candidateIndex = 0; candidateIndex < 2; ++candidateIndex) {
                const TArray64& current = candidates[candidateIndex];
                if (!candidateAddresses[candidateIndex] || current.count < 1000 ||
                    current.count > 5000000 || current.capacity < current.count ||
                    current.capacity > 8000000 || !current.data) continue;

                int nameScore = ScoreGNamesCandidate(&current, modBase, modSize);
                int objectScore = ScoreGObjectsCandidate(&current, modBase, modSize);
                if (nameScore > bestNames.nameScore)
                    bestNames = { candidateAddresses[candidateIndex], current, nameScore, 0 };
                if (objectScore > bestObjects.objectScore)
                    bestObjects = { candidateAddresses[candidateIndex], current, 0, objectScore };
            }
        }
    }

    // Use GObjects SDK signature as fallback/validation
    Log("[UE3Scanner] Searching for GObjects SDK signature...");
    auto results = ScanPattern(modBase, modSize,
        "48 8B 0D ?? ?? ?? ?? 48 8B 04 ?? 48 8B 40 ?? 25 00020000");
    if (!results.empty()) {
        Log("[UE3Scanner] GObjects SDK signature found at %p (%zu matches)", (void*)results[0].address, results.size());
        // The instruction uses RIP-relative addressing: LEA RCX, [rip+disp32]
        // Read the displacement at offset 2 (after the opcode byte)
        uintptr_t instrAddr = results[0].address;
        // Check if this is a LEA/MOV with RIP-relative addressing
        uint8_t opcode = 0;
        ReadMem(instrAddr, &opcode, 1);
        if (opcode == 0x48) {
            uint8_t modrm = 0;
            ReadMem(instrAddr + 2, &modrm, 1);
            if ((modrm & 0xC7) == 0x0D) { // mod=00, reg=001, r/m=101 (RIP-relative)
                int32_t disp = 0;
                ReadMem(instrAddr + 3, &disp, 4);
                uintptr_t globalAddr = instrAddr + 7 + disp; // 7 = instruction length
                Log("[UE3Scanner] GObjects global at %p (RIP-relative from %p)", (void*)globalAddr, (void*)instrAddr);

                // Read the actual TArray
                TArray64 arr = {};
                if (ReadMem(globalAddr, &arr, sizeof(arr))) {
                    int objScore = ScoreGObjectsCandidate(&arr, modBase, modSize);
                    Log("[UE3Scanner] GObjects TArray: data=%p count=%d capacity=%d score=%d",
                        (void*)arr.data, arr.count, arr.capacity, objScore);
                    if (objScore > 30) {
                        globals->gObjectsAddress = globalAddr;
                        globals->gObjectsValid = true;
                    }
                }
            }
        }
    }

    // Use best candidates found
    if (bestNames.nameScore >= 35 && !globals->gNamesValid) {
        globals->gNamesAddress = bestNames.address;
        globals->gNameCount = bestNames.array.count;
        globals->gNameStringOffset = DetectFNameStringOffset(&bestNames.array);
        globals->gNamesValid = ValidateGNames(&bestNames.array, globals->gNameStringOffset);
        Log("[UE3Scanner] GNames candidate at %p: count=%d stringOffset=0x%X score=%d valid=%d",
            (void*)bestNames.address, bestNames.array.count, globals->gNameStringOffset,
            bestNames.nameScore, globals->gNamesValid);
    }

    if (bestObjects.objectScore >= 35 && !globals->gObjectsValid) {
        globals->gObjectsAddress = bestObjects.address;
        globals->gObjectsValid = true;
        Log("[UE3Scanner] GObjects candidate at %p: count=%d score=%d",
            (void*)bestObjects.address, bestObjects.array.count, bestObjects.objectScore);
    }

    if (globals->gNamesValid && globals->gObjectsValid) {
        TArray64 names = {};
        TArray64 objects = {};
        if (ReadMem(globals->gNamesAddress, &names, sizeof(names)) &&
            ReadMem(globals->gObjectsAddress, &objects, sizeof(objects))) {
            globals->gObjectNameOffset = DetectUObjectNameOffset(
                objects, names, globals->gNameStringOffset, modBase, modSize);
            if (globals->gObjectNameOffset >= 0) {
                globals->gObjectClassOffset = DetectUObjectClassOffset(
                    objects, names, globals->gNameStringOffset, globals->gObjectNameOffset);
            }
        }
    }
}

struct CameraCandidate {
    uintptr_t object = 0;
    int offset = 0;
    int score = 0;
    char objectName[128] = {};
    float location[3] = {};
    int32_t rotation[3] = {};
    float fov = 0.0f;
};

static bool ReadObjectName(const UE3Globals& globals, const TArray64& names,
                           uintptr_t object, char* out, size_t outSize,
                           int32_t* nameNumber = nullptr) {
    return ReadObjectNameAtOffset(names, globals.gNameStringOffset, object,
                                  globals.gObjectNameOffset, out, outSize, nameNumber);
}

static bool IsCameraObjectName(const char* name) {
    return strcmp(name, "WillowPlayerCamera") == 0 ||
           strcmp(name, "PlayerCamera") == 0 ||
           strcmp(name, "WillowPlayerController") == 0;
}

static bool ReadCameraCandidate(uintptr_t object, int offset, CameraCandidate* candidate) {
    float values[7] = {};
    int32_t rotation[3] = {};
    if (!ReadMem(object + offset, values, sizeof(values)) ||
        !ReadMem(object + offset + 12, rotation, sizeof(rotation))) {
        return false;
    }

    const float fov = values[6];
    float maxLocation = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(values[axis]) || fabsf(values[axis]) > 10000000.0f) return false;
        maxLocation = (std::max)(maxLocation, fabsf(values[axis]));
        if (rotation[axis] < -1048576 || rotation[axis] > 1048576) return false;
    }
    if (maxLocation < 1.0f || !std::isfinite(fov) || fov < 30.0f || fov > 170.0f) return false;

    candidate->object = object;
    candidate->offset = offset;
    memcpy(candidate->location, values, sizeof(candidate->location));
    memcpy(candidate->rotation, rotation, sizeof(candidate->rotation));
    candidate->fov = fov;
    candidate->score = 4;
    if (fov >= 50.0f && fov <= 120.0f) candidate->score += 2;
    if ((offset & 0xF) == 0xC) candidate->score += 1;
    if (rotation[0] || rotation[1] || rotation[2]) candidate->score += 1;
    return true;
}

static bool ReadReflectedPropertyOffset(const UE3Globals& globals, const TArray64& names,
                                        uintptr_t property, const char* expectedOuter,
                                        int expectedSize, int* propertyOffset) {
    uint64_t outer = 0;
    char outerName[128] = {};
    int32_t arrayDim = 0;
    int32_t elementSize = 0;
    int32_t offset = -1;
    if (!ReadMem(property + globals.gObjectNameOffset - 8, &outer, sizeof(outer)) || !outer ||
        !ReadObjectName(globals, names, outer, outerName, sizeof(outerName)) ||
        strcmp(outerName, expectedOuter) != 0 ||
        !ReadMem(property + 0x68, &arrayDim, sizeof(arrayDim)) || arrayDim != 1 ||
        !ReadMem(property + 0x6C, &elementSize, sizeof(elementSize)) ||
        elementSize != expectedSize ||
        !ReadMem(property + 0x8C, &offset, sizeof(offset)) || offset < 0 || offset > 0x10000) {
        return false;
    }
    *propertyOffset = offset;
    return true;
}

// Resolve named camera objects before inspecting their camera cache.
static void FindCameraCache(const UE3Globals* globals, CameraInfo* camera, uintptr_t modBase, DWORD modSize) {
    if (!globals->gNamesValid || !globals->gObjectsValid ||
        globals->gNameStringOffset < 0 || globals->gObjectNameOffset < 0 ||
        globals->gObjectClassOffset < 0) {
        Log("[UE3Scanner] Cannot find camera cache: UObject names are not validated");
        return;
    }

    Log("[UE3Scanner] Scanning GNames for camera-related entries...");

    TArray64 names = {};
    ReadMem(globals->gNamesAddress, &names, sizeof(names));

    // Find camera-related FName indices
    static const char* cameraTargets[] = {
        "PlayerCameraManager", "PlayerCamera", "CalcCamera", "UpdateCamera",
        "CalcViewLocation", "CalcViewRotation", "CameraCache"
    };

    struct NameMatch {
        int32_t index;
        char name[128];
    };
    std::vector<NameMatch> matches;

    for (int32_t i = 0; i < names.count && i < 50000; i++) {
        uint64_t entry = 0;
        if (!ReadMem(names.data + (uint64_t)i * 8, &entry, sizeof(entry)) || !entry) continue;
        char name[128];
        if (!ReadString(entry + globals->gNameStringOffset, name, sizeof(name))) continue;

        for (auto* target : cameraTargets) {
            if (strstr(name, target)) {
                matches.push_back({ i, {} });
                strncpy(matches.back().name, name, sizeof(matches.back().name) - 1);
                break;
            }
        }
    }

    Log("[UE3Scanner] Found %zu camera-related FName entries", matches.size());

    TArray64 objects = {};
    if (!ReadMem(globals->gObjectsAddress, &objects, sizeof(objects))) return;

    std::vector<CameraCandidate> candidates;
    std::unordered_set<uintptr_t> scannedCameraObjects;
    auto scanCameraObject = [&](uintptr_t object, const char* className, int maxOffset) {
        if (!scannedCameraObjects.insert(object).second) return;
        for (int offset = 0x100; offset < maxOffset; offset += 4) {
            CameraCandidate candidate = {};
            if (!ReadCameraCandidate(object, offset, &candidate)) continue;
            strncpy_s(candidate.objectName, className, _TRUNCATE);
            if (strcmp(className, "WillowPlayerCamera") == 0) candidate.score += 4;
            else if (strcmp(className, "PlayerCamera") == 0) candidate.score += 2;
            candidates.push_back(candidate);
        }
    };
    int namedObjects = 0;
    uintptr_t activeController = 0;
    int reflectedLocationOffset = -1;
    int reflectedRotationOffset = -1;
    int reflectedFovOffset = -1;
    for (int32_t i = 0; i < objects.count; ++i) {
        uint64_t object = 0;
        uint64_t classObject = 0;
        char objectName[128] = {};
        char className[128] = {};
        int32_t nameNumber = 0;
        if (!ReadMem(objects.data + static_cast<uint64_t>(i) * 8, &object, sizeof(object)) || !object ||
            !ReadObjectName(*globals, names, object, objectName, sizeof(objectName), &nameNumber) ||
            !ReadMem(object + globals->gObjectClassOffset, &classObject, sizeof(classObject)) || !classObject ||
            !ReadObjectName(*globals, names, classObject, className, sizeof(className))) {
            continue;
        }

        if (strcmp(className, "StructProperty") == 0) {
            if (strcmp(objectName, "CalcViewLocation") == 0 &&
                ReadReflectedPropertyOffset(*globals, names, object,
                                            "WillowPlayerController", 12,
                                            &reflectedLocationOffset)) {
                Log("[UE3Scanner] Reflected CalcViewLocation offset=0x%X",
                    reflectedLocationOffset);
            } else if (strcmp(objectName, "CalcViewRotation") == 0 &&
                       ReadReflectedPropertyOffset(*globals, names, object,
                                                   "WillowPlayerController", 12,
                                                   &reflectedRotationOffset)) {
                Log("[UE3Scanner] Reflected CalcViewRotation offset=0x%X",
                    reflectedRotationOffset);
            }
        } else if (strcmp(className, "FloatProperty") == 0 &&
                   strcmp(objectName, "CachedFOVAngle") == 0 &&
                   ReadReflectedPropertyOffset(*globals, names, object,
                                               "WillowPlayerController", 4,
                                               &reflectedFovOffset)) {
            Log("[UE3Scanner] Reflected CachedFOVAngle offset=0x%X", reflectedFovOffset);
        }

        if (!IsCameraObjectName(className)) continue;

        // Class default objects contain template values, never the live camera.
        if (strncmp(objectName, "Default__", 9) == 0) continue;

        ++namedObjects;
        Log("[UE3Scanner] Camera instance: index=%d object=%p name=%s_%d class=%s",
            i, reinterpret_cast<void*>(object), objectName, nameNumber, className);
        if (strcmp(className, "WillowPlayerController") == 0) activeController = object;

        scanCameraObject(object, className,
                         strcmp(className, "WillowPlayerController") == 0 ? 0x3000 : 0x2000);

        if (strcmp(className, "WillowPlayerController") != 0) continue;
        for (int pointerOffset = 0x80; pointerOffset < 0x3000; pointerOffset += 8) {
            uint64_t pointedObject = 0;
            uint64_t pointedClass = 0;
            char pointedName[128] = {};
            char pointedClassName[128] = {};
            if (!ReadMem(object + pointerOffset, &pointedObject, sizeof(pointedObject)) || !pointedObject ||
                !ReadObjectName(*globals, names, pointedObject, pointedName, sizeof(pointedName)) ||
                !ReadMem(pointedObject + globals->gObjectClassOffset,
                         &pointedClass, sizeof(pointedClass)) || !pointedClass ||
                !ReadObjectName(*globals, names, pointedClass,
                                pointedClassName, sizeof(pointedClassName)) ||
                !IsCameraObjectName(pointedClassName) ||
                strncmp(pointedName, "Default__", 9) == 0) {
                continue;
            }
            Log("[UE3Scanner] Controller camera pointer: controller=%p +0x%X -> %p name=%s class=%s",
                reinterpret_cast<void*>(object), pointerOffset,
                reinterpret_cast<void*>(pointedObject), pointedName, pointedClassName);
            scanCameraObject(pointedObject, pointedClassName, 0x2000);
        }
    }

    Log("[UE3Scanner] Named camera objects=%d structured candidates=%zu",
        namedObjects, candidates.size());
    if (activeController && reflectedLocationOffset >= 0 && reflectedRotationOffset >= 0 &&
        reflectedFovOffset >= 0) {
        float location[3] = {};
        int32_t rotation[3] = {};
        float fov = 0.0f;
        if (ReadMem(activeController + reflectedLocationOffset, location, sizeof(location)) &&
            ReadMem(activeController + reflectedRotationOffset, rotation, sizeof(rotation)) &&
            ReadMem(activeController + reflectedFovOffset, &fov, sizeof(fov)) &&
            std::isfinite(location[0]) && std::isfinite(location[1]) &&
            std::isfinite(location[2]) && std::isfinite(fov) && fov >= 30.0f && fov <= 170.0f) {
            camera->controllerAddress = activeController;
            camera->locationOffset = reflectedLocationOffset;
            camera->rotationOffset = reflectedRotationOffset;
            camera->fovOffset = reflectedFovOffset;
            camera->cameraCacheLocation = activeController + reflectedLocationOffset;
            camera->cameraCacheRotation = activeController + reflectedRotationOffset;
            camera->cameraFov = activeController + reflectedFovOffset;
            camera->found = true;
            Log("[UE3Scanner] Reflected camera validated: controller=%p loc=+0x%X "
                "rot=+0x%X fov=+0x%X values=(%.1f,%.1f,%.1f) (%d,%d,%d) %.1f",
                reinterpret_cast<void*>(activeController), reflectedLocationOffset,
                reflectedRotationOffset, reflectedFovOffset, location[0], location[1], location[2],
                rotation[0], rotation[1], rotation[2], fov);
        }
    }
    CameraCandidate* best = nullptr;
    bool tied = false;
    for (auto& candidate : candidates) {
        Log("[UE3Scanner] Structured camera candidate: %s obj=%p offset=0x%X "
            "loc=(%.1f,%.1f,%.1f) rot=(%d,%d,%d) fov=%.1f score=%d",
            candidate.objectName, reinterpret_cast<void*>(candidate.object), candidate.offset,
            candidate.location[0], candidate.location[1], candidate.location[2],
            candidate.rotation[0], candidate.rotation[1], candidate.rotation[2],
            candidate.fov, candidate.score);
        if (!best || candidate.score > best->score) {
            best = &candidate;
            tied = false;
        } else if (candidate.score == best->score) {
            tied = true;
        }
    }
    if (!camera->found && best && !tied && best->score >= 10) {
        camera->controllerAddress = best->object;
        camera->locationOffset = best->offset;
        camera->rotationOffset = best->offset + 12;
        camera->cameraCacheLocation = best->object + best->offset;
        camera->cameraCacheRotation = best->object + best->offset + 12;
        camera->cameraFov = best->object + best->offset + 24;
        camera->fovOffset = best->offset + 24;
        camera->found = true;
        Log("[UE3Scanner] Camera cache validated: object=%p location=+0x%X rotation=+0x%X",
            reinterpret_cast<void*>(best->object), camera->locationOffset, camera->rotationOffset);
    } else if (!camera->found && best) {
        Log("[UE3Scanner] Camera candidate rejected: bestScore=%d tied=%d", best->score, tied);
    }

    // Log all found FName matches for reference
    for (const auto& m : matches) {
        Log("[UE3Scanner] FName[%d] = '%s'", m.index, m.name);
    }
}

bool ScanForUE3Globals(UE3Globals* globals, CameraInfo* camera) {
    HMODULE gameModule = GetModuleHandleA("BorderlandsGOTY.exe");
    if (!gameModule) {
        Log("[UE3Scanner] ERROR: BorderlandsGOTY.exe module not found");
        return false;
    }

    MODULEINFO modInfo = {};
    GetModuleInformation(GetCurrentProcess(), gameModule, &modInfo, sizeof(modInfo));

    uintptr_t modBase = (uintptr_t)modInfo.lpBaseOfDll;
    DWORD modSize = modInfo.SizeOfImage;

    Log("[UE3Scanner] Module: %p, size: 0x%X", (void*)modBase, modSize);

    // Phase 1: Find GNames and GObjects
    ScanForUE3GlobalsImpl(globals, modBase, modSize);

    if (!globals->gNamesValid && !globals->gObjectsValid) {
        Log("[UE3Scanner] WARNING: Neither GNames nor GObjects found reliably");
        Log("[UE3Scanner] Camera discovery will rely on pattern scanning only");
    }

    // Phase 2: Find camera cache
    FindCameraCache(globals, camera, modBase, modSize);

    Log("[UE3Scanner] Scan complete: GNames=%s GObjects=%s Camera=%s",
        globals->gNamesValid ? "VALID" : "not found",
        globals->gObjectsValid ? "VALID" : "not found",
        camera->found ? "FOUND" : "not found");

    return globals->gNamesValid || globals->gObjectsValid;
}

uintptr_t FindViewportDrawTarget(const UE3Globals& globals) {
    if (!globals.gNamesValid || !globals.gObjectsValid || globals.gObjectNameOffset < 0) return 0;

    TArray64 names = {};
    TArray64 objects = {};
    if (!ReadMem(globals.gNamesAddress, &names, sizeof(names)) ||
        !ReadMem(globals.gObjectsAddress, &objects, sizeof(objects))) {
        return 0;
    }

    std::unordered_set<int32_t> viewportNameIndices;
    for (int32_t i = 0; i < names.count; ++i) {
        uint64_t entry = 0;
        char name[128] = {};
        if (!ReadMem(names.data + static_cast<uint64_t>(i) * 8, &entry, sizeof(entry)) || !entry ||
            !ReadString(entry + globals.gNameStringOffset, name, sizeof(name))) {
            continue;
        }
        if (strstr(name, "GameViewportClient") && !strstr(name, "exec")) {
            viewportNameIndices.insert(i);
            Log("[UE3Scanner] Viewport FName[%d] = '%s'", i, name);
        }
    }
    if (viewportNameIndices.empty()) return 0;

    HMODULE module = GetModuleHandleA("BorderlandsGOTY.exe");
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    uintptr_t textBegin = 0;
    uintptr_t textEnd = 0;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            textBegin = reinterpret_cast<uintptr_t>(base) + sections[i].VirtualAddress;
            textEnd = textBegin + sections[i].Misc.VirtualSize;
            break;
        }
    }
    if (!textBegin) return 0;

    for (int32_t i = 0; i < objects.count; ++i) {
        uint64_t object = 0;
        if (!ReadMem(objects.data + static_cast<uint64_t>(i) * 8, &object, sizeof(object)) || !object) {
            continue;
        }

        int32_t nameIndex = -1;
        int32_t nameNumber = 0;
        char objectName[128] = {};
        if (!ReadMem(object + globals.gObjectNameOffset, &nameIndex, sizeof(nameIndex)) ||
            !viewportNameIndices.count(nameIndex) ||
            !ReadObjectName(globals, names, object, objectName, sizeof(objectName), &nameNumber) ||
            strncmp(objectName, "Default__", 9) == 0) continue;

        uint64_t secondaryVtable = 0;
        uint64_t slots[3] = {};
        if (!ReadMem(object + 0x60, &secondaryVtable, sizeof(secondaryVtable)) ||
            !ReadMem(secondaryVtable, slots, sizeof(slots))) {
            continue;
        }
        if (slots[0] < textBegin || slots[0] >= textEnd ||
            slots[1] < textBegin || slots[1] >= textEnd ||
            slots[2] < textBegin || slots[2] >= textEnd) {
            continue;
        }

        Log("[UE3Scanner] GameViewportClient object=%p name=%s_%d secondaryVtable=%p Draw=%p",
            reinterpret_cast<void*>(object), objectName, nameNumber,
            reinterpret_cast<void*>(secondaryVtable), reinterpret_cast<void*>(slots[2]));
        return static_cast<uintptr_t>(slots[2]);
    }

    Log("[UE3Scanner] No validated GameViewportClient secondary vtable found");
    return 0;
}

}} // namespace bl1gotyvr::camera
