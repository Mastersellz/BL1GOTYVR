#pragma once

#include <algorithm>
#include <cmath>

namespace bl1gotyvr::render {

// Match the headset-validated original v0.5.7.3 indicator. In particular,
// vertical span is centered; do not substitute a raw asymmetric-FOV center.
inline bool ProjectReloadHighlight(const float local[3], float radiusMeters,
        float tanLeft, float tanRight, float tanDown, float tanUp,
        float& u, float& v, float& radius) {
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(local[axis])) return false;
    const float values[] = {tanLeft, tanRight, tanDown, tanUp};
    for (float value : values) if (!std::isfinite(value)) return false;
    if (!std::isfinite(radiusMeters) || radiusMeters <= 0.0f ||
        tanRight <= tanLeft || tanUp <= tanDown || local[2] >= -0.001f) return false;
    const float depth = -local[2];
    const float x = (local[0] / depth - tanLeft) / (tanRight - tanLeft);
    const float verticalHalfSpan = (tanUp - tanDown)*0.5f;
    const float y = (verticalHalfSpan - local[1]/depth) / (verticalHalfSpan*2.0f);
    if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return false;
    u = x;
    v = y;
    radius = std::clamp(radiusMeters / depth /
        (tanUp - tanDown), 0.01f, 0.5f);
    return true;
}

} // namespace bl1gotyvr::render
