#include "../src/player/PoseFrame.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

using namespace bl1gotyvr::player;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void Near(const Vec3& actual, const Vec3& expected, const char* message) {
    Check(std::isfinite(actual.lengthSq()) && (actual - expected).length() < 0.005f,
          message);
}

static std::array<float, 16> Frame(const Vec3& position, float yaw, float scale) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {c*scale, s*scale, 0, 0, -s*scale, c*scale, 0, 0,
            0, 0, scale, 0, position.x, position.y, position.z, 1};
}

static void Inverse() {
    for (float scale : {0.864f, 0.918f, 1.0f, 1.15f}) {
        for (float yaw : {0.0f, 0.3f, 1.57f, 3.14f, -2.6f}) {
            auto frame = Frame({1234, -452, 96}, yaw, scale);
            // Include non-uniform scale in a rotated component.
            for (int i = 4; i < 7; ++i) frame[i] *= 1.1f;
            const Vec3 input{48, 24, -16};
            Vec3 output;
            Check(InversePosePoint(frame.data(), TransformPosePoint(frame.data(), input),
                                   output), "invertible component rejected");
            Near(output, input, "scaled component inverse moved the hand target");
        }
    }
}

static void Replay() {
    // Eye zero's solved palette is frozen in world space while native walking
    // moves/rotates/scales the arms component before eye one and later restamps.
    for (float scale : {0.864f, 0.918f, 1.0f}) {
        const auto eyeZero = Frame({2000, -500, 100}, 0.4f, scale);
        const Vec3 cachedLocal{48, 24, -16};
        const Vec3 frozenWorld = TransformPosePoint(eyeZero.data(), cachedLocal);
        for (int step = 0; step < 120; ++step) {
            const auto current = Frame({2000 + step*2.1f, -500 + step*0.4f, 100},
                                       0.4f + step*0.002f, 1.0f);
            Vec3 replay;
            Check(InversePosePoint(current.data(), frozenWorld, replay),
                  "walking replay inverse failed");
            Near(TransformPosePoint(current.data(), replay), frozenWorld,
                 "stereo replay drifted with the moving component");
        }
        // The NEXT pair must use the new camera anchor, not frozenWorld.
        const Vec3 nextAnchorDelta{8, -2, 0};
        const auto next = Frame({2008, -502, 100}, 0.4f, scale);
        Vec3 nextLocal;
        Check(InversePosePoint(next.data(), frozenWorld + nextAnchorDelta, nextLocal),
              "new pair target inverse failed");
        Near(nextLocal, cachedLocal, "new walking pair retained a stale world target");
    }
}

static void Invalid() {
    const Vec3 sentinel{7, 8, 9};
    Vec3 output = sentinel;
    auto matrix = Frame({}, 0, 0);
    Check(!InversePosePoint(matrix.data(), {}, output), "singular component accepted");
    Near(output, sentinel, "failed inverse changed its output");
    for (int element = 0; element < 16; ++element) {
        matrix = Frame({}, 0, 1);
        matrix[element] = std::numeric_limits<float>::quiet_NaN();
        Check(!InversePosePoint(matrix.data(), {}, output), "NaN component accepted");
    }
    matrix = Frame({}, 0, 1);
    Check(!InversePosePoint(matrix.data(),
          {std::numeric_limits<float>::infinity(), 0, 0}, output),
          "nonfinite hand target accepted");
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "specify a test case");
        const std::string test = argv[1];
        if (test == "inverse") Inverse();
        else if (test == "replay") Replay();
        else if (test == "invalid") Invalid();
        else throw std::runtime_error("unknown test case");
        std::printf("PASS: pose_%s\n", argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
