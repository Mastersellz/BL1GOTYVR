#pragma once
#include <cstdint>

namespace bl1gotyvr { namespace camera {

struct UE3Globals;

void StartScanner();
bool InstallViewportDrawHook(uintptr_t target);
bool IsCameraFound();
bool IsNativeMultiviewActive();
uint64_t GetNativeMultiviewGeneration();
float* GetCameraLocation();   // float[3]
int32_t* GetCameraRotation(); // int[3] in Unis
const UE3Globals& GetUE3Globals();

}} // namespace bl1gotyvr::camera
