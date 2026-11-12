#include "../src/xr/MotionPairTiming.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace bl1gotyvr::xr;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void Acquire() {
    uint64_t serial = 0;
    int snapshots = 0;
    bool valid = false;
    int nextEye = 0;
    auto acquire = [&]() {
        if (NeedsNewStereoPair(nextEye, valid)) {
            ++snapshots;
            ++serial;
            valid = true;
        }
        return serial;
    };
    for (int pair = 1; pair <= 120; ++pair) {
        const auto animationSerial = acquire();
        // Native component processing can ask more than once before Draw.
        Check(acquire() == animationSerial, "animation acquisition replaced its ticket");
        const auto drawSerial = acquire();
        Check(drawSerial == animationSerial, "Draw replaced the prepared XR/controller snapshot");
        Check(snapshots == pair, "first eye captured more than one tracking snapshot");
        nextEye = 1;
        Check(acquire() == animationSerial, "right eye did not reuse the left-eye ticket");
        nextEye = 0;
        valid = false; // Complete pair, or AbortStereoPair.
    }
    Check(acquire() == 121, "new pair reused a completed/aborted ticket");
    Check(!NeedsNewStereoPair(1, false), "orphan right eye created a new pair");
}

static void Stage() {
    Check(CanStageMotionPair(true, 7, 0, true, 7, 0), "matching animation ticket rejected");
    Check(!CanStageMotionPair(false, 7, 0, true, 7, 0), "inactive pair accepted");
    Check(!CanStageMotionPair(true, 7, 0, false, 7, 0), "invalid ticket accepted");
    Check(!CanStageMotionPair(true, 0, 0, true, 0, 0), "zero serial accepted");
    Check(!CanStageMotionPair(true, 8, 0, true, 7, 0), "old pair overwrote a new pair");
    Check(!CanStageMotionPair(true, 7, 1, true, 7, 0), "late preparation changed the second eye");
    Check(!CanStageMotionPair(true, 7, 0, true, 7, 1), "right eye published left-eye preparation");
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "specify a test case");
        const std::string test = argv[1];
        if (test == "acquire") Acquire();
        else if (test == "stage") Stage();
        else throw std::runtime_error("unknown test case");
        std::printf("PASS: motion_pair_%s\n", argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
