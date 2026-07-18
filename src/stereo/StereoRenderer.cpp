#include "StereoRenderer.hpp"
#include "../core/VRMod.hpp"
#include "../core/globals.hpp"

namespace bl1gotyvr { namespace stereo {

bool Initialize() {
    Log("[Stereo] Initialized (stub)");
    return true;
}

void Shutdown() {
    Log("[Stereo] Shutdown");
}

void OnPresent() {
    // TODO: Phase 5 — double-render or AFR stereo
}

}} // namespace bl1gotyvr::stereo
