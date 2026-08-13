#pragma once
#include <cstdint>
#include <openxr/openxr.h>

namespace bl1gotyvr { namespace camera {

struct UE3Globals;

struct CompletedNativeMultiviewFrame {
    uint64_t generation = 0;
    XrView renderedViews[2] = {};
};

void StartScanner();
bool InstallViewportDrawHook(uintptr_t target);
bool IsCameraFound();
bool IsNativeMultiviewActive();
bool ConsumeCompletedNativeMultiviewFrame(CompletedNativeMultiviewFrame& frame);
bool ConsumeRenderPoseAcknowledgement(uint64_t pairSerial, int eye);
bool GetPrincipalRenderExtent(uint32_t& width, uint32_t& height);
void DiscardCompletedNativeMultiviewFrame();
void SuspendNativeMultiview();
float* GetCameraLocation();   // float[3]
int32_t* GetCameraRotation(); // int[3] in Unis
UE3Globals GetUE3GlobalsSnapshot();
void RequestRecenter();  // Request camera recenter (integer yaw system)
void RequestVisibilityInventoryRefresh();
void RequestPlayerIdentityRefresh();

}} // namespace bl1gotyvr::camera
