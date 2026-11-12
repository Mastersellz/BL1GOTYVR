#pragma once

namespace bl1gotyvr::input {

// Original v0.5.7.3 ownership: bootstrap with a canonical component stamp,
// then let the engine's IK attachment own placement. Ownership remains latched
// until a weapon/component change or calibration reset, not per-eye discovery.
inline bool UseNativeIkAttachment(bool latched, bool poseHookInstalled, bool rigValid) {
    return latched || (poseHookInstalled && rigValid);
}

} // namespace bl1gotyvr::input
