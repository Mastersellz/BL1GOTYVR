#include "../src/camera/ViewAnchor.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>

using bl1gotyvr::camera::AdjustViewAnchor;

static void Near(float actual, float expected, const char* message) {
    if (!std::isfinite(actual) || std::fabs(actual - expected) > 0.002f)
        throw std::runtime_error(message);
}

int main() {
    try {
        const float handOffset[3] = {40, 24, -18};
        const float eyeOffsets[2][3] = {{0, -3.2f, 0}, {0, 3.2f, 0}};
        for (int pair = 0; pair < 120; ++pair) {
            const float anchor[3] = {2000 + pair*8.0f, -500 + pair*1.5f, 100};
            for (int eye = 0; eye < 2; ++eye) {
                // Native camera drift/bob differs between consecutive AFR ticks.
                const float native[3] = {anchor[0] + (eye ? 4.0f : -1.0f),
                    anchor[1] + (eye ? 0.75f : 0.2f),
                    anchor[2] + std::sin(pair*0.4f + eye)*0.8f};
                const float preView[3] = {-native[0], -native[1], -native[2]};
                const auto result = AdjustViewAnchor(anchor, native, preView, eyeOffsets[eye]);
                for (int axis = 0; axis < 3; ++axis) {
                    Near(result.origin[axis], anchor[axis] + eyeOffsets[eye][axis],
                         "native walking drift leaked into the frozen camera anchor");
                    Near(result.negativeOrigin[axis], -result.origin[axis],
                         "translated and full-view origins disagree");
                    const float handWorld = anchor[axis] + handOffset[axis];
                    Near(handWorld - result.origin[axis],
                         handOffset[axis] - eyeOffsets[eye][axis],
                         "walking changed the hand-to-eye position");
                    Near(handWorld + result.negativeOrigin[axis],
                         handWorld - result.origin[axis],
                         "shader pretranslation retained a different walking origin");
                }
            }
        }
        // Desktop/native fallback: selecting the native anchor changes only
        // tracking offset. Preserve any existing pretranslation residual.
        const float native[3] = {40, 50, 60};
        const float preView[3] = {-39, -48, -57};
        const float offset[3] = {1, 2, 3};
        const auto fallback = AdjustViewAnchor(native, native, preView, offset);
        for (int axis = 0; axis < 3; ++axis)
            Near(fallback.negativeOrigin[axis], preView[axis] - offset[axis],
                 "native fallback pretranslation changed");
        std::puts("PASS: walking stereo view anchor and pretranslation");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
