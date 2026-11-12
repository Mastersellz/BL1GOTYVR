#pragma once
#include <cstdint>

namespace bl1gotyvr { namespace camera {

struct UE3Globals {
    uintptr_t gNamesAddress = 0;
    uintptr_t gObjectsAddress = 0;
    int gNameCount = 0;
    int gNameStringOffset = -1;  // offset within FNameEntry to the string
    int gObjectNameOffset = -1;  // offset of FName within UObject
    int gObjectClassOffset = -1; // offset of Class pointer within UObject
    bool gNamesValid = false;
    bool gObjectsValid = false;
};

struct CameraInfo {
    uintptr_t controllerAddress = 0;
    uintptr_t cameraCacheLocation = 0;  // float[3] at controller+offset
    uintptr_t cameraCacheRotation = 0;  // int32[3] at controller+offset
    uintptr_t cameraFov = 0;
    int locationOffset = 0;
    int rotationOffset = 0;
    int fovOffset = 0;
    int controllerRotationOffset = -1; // reflected Actor::Rotation, never a guessed rotator
    int worldInfoOffset = -1;          // Actor::WorldInfo
    int pauserOffset = -1;             // WorldInfo::Pauser
    int cinematicModeOffset = -1;
    uint32_t cinematicModeMask = 0;
    int ignoreLookOffset = -1;
    uint32_t ignoreLookMask = 0;
    bool found = false;
};

// Scan the game module for UE3 globals and camera cache
// Runs in background thread, writes to output structs
bool ScanForUE3Globals(UE3Globals* globals, CameraInfo* camera);
bool RefreshCameraCache(const UE3Globals& globals, CameraInfo* camera);
uintptr_t FindViewportDrawTarget(const UE3Globals& globals);

}} // namespace bl1gotyvr::camera
