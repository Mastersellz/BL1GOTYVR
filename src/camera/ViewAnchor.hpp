#pragma once

namespace bl1gotyvr::camera {

struct ViewAnchorAdjustment {
    float origin[3] = {};
    float negativeOrigin[3] = {};
    float nativeDelta[3] = {};
};

// ViewProjection uses the full world origin; translated shader coordinates use
// PreViewTranslation. Both must receive the same anchor correction, not just the
// HMD/IPD offset, when the native source moved after the stereo ticket was made.
inline ViewAnchorAdjustment AdjustViewAnchor(
        const float anchor[3], const float nativeOrigin[3],
        const float nativeNegativeOrigin[3], const float trackingOffset[3]) {
    ViewAnchorAdjustment result;
    for (int axis = 0; axis < 3; ++axis) {
        result.origin[axis] = anchor[axis] + trackingOffset[axis];
        result.nativeDelta[axis] = result.origin[axis] - nativeOrigin[axis];
        result.negativeOrigin[axis] = nativeNegativeOrigin[axis] - result.nativeDelta[axis];
    }
    return result;
}

} // namespace bl1gotyvr::camera
