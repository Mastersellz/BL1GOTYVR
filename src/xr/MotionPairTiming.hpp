#pragma once

#include <cstdint>

namespace bl1gotyvr::xr {

// Animation and Draw may both acquire eye zero. Acquisition must not replace
// the immutable XR/controller sample already used to prepare the mesh.
inline bool NeedsNewStereoPair(int nextEye, bool activePairValid) {
    return nextEye == 0 && !activePairValid;
}

inline bool CanStageMotionPair(bool activeValid, uint64_t activeSerial,
        int nextEye, bool ticketValid, uint64_t ticketSerial, int ticketEye) {
    return activeValid && ticketValid && activeSerial != 0 &&
        activeSerial == ticketSerial && nextEye == 0 && ticketEye == 0;
}

} // namespace bl1gotyvr::xr
