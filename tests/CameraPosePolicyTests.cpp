#include "../src/camera/CameraPosePolicy.hpp"

#include <array>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace bl1gotyvr::camera;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void Near(float actual, float expected, const char* message) {
    Check(std::isfinite(actual) && std::fabs(actual - expected) < 0.0001f, message);
}

static void Reference() {
    for (int32_t native : {0, 100, -5817, 16000, 65535, -1047551562, INT_MAX, INT_MIN})
        Check(SelectGamePitchReference(true, native) == 0,
              "on-foot reference absorbed native/HMD/recoil pitch");
    Check(SelectGamePitchReference(false, -5817) == -5817, "vehicle/native pitch changed");
    Check(SelectGamePitchReference(false, 65535) == -1, "wrapped native pitch not normalized");
    Check(SelectGamePitchReference(false, -65536) == 0, "whole-turn native pitch not normalized");
    Check(SelectGamePitchReference(false, -1047551562) == 0,
          "invalid startup pitch became a permanent reference");
    Check(SelectGamePitchReference(false, INT_MAX) == 0, "invalid native pitch accepted");
    // Repeated unarmed resets/recenters must not accumulate restored HMD pitch.
    int32_t base = 0;
    for (int i = 0; i < 100; ++i) {
        base = SelectGamePitchReference(true, base - 5817);
        Check(base == 0, "interaction pitch compounded after a reference reset");
    }
}

static void Recoil() {
    RecoilPitchHold hold;
    constexpr uintptr_t controller = 0x100000, pawn = 0x200000;
    Check(!hold.Update(true, false, controller, pawn, 1000), "idle camera was held");
    Check(hold.Update(true, true, controller, pawn, 1000), "first shot did not start hold");
    Check(hold.Update(true, true, controller, pawn, 2000), "continuous fire lost hold");
    Check(hold.Update(true, false, controller, pawn, 2399), "release cooldown ended early");
    Check(!hold.Update(true, false, controller, pawn, 2400), "release cooldown did not expire");
    hold.Update(true, true, controller, pawn, 3000);
    Check(!hold.Update(true, false, controller, pawn + 1, 3001), "hold crossed a pawn change");
    hold.Update(true, true, controller, pawn, 4000);
    Check(!hold.Update(true, false, controller + 1, pawn, 4001), "hold crossed a controller change");
    hold.Update(true, true, controller, pawn, 5000);
    Check(!hold.Update(false, true, controller, pawn, 5001), "disabled/vehicle context held camera");
    Check(!hold.Update(true, false, controller, pawn, 5002), "old hold survived context exit");
    hold.Update(true, true, controller, pawn, 6000);
    Check(!hold.Update(true, false, controller, pawn, 5999), "backward clock kept a stale hold");
    Check(!hold.Update(true, true, 0, pawn, 7000), "missing controller accepted");
    Check(!hold.Update(true, true, controller, 0, 7000), "missing pawn accepted");
    Check(hold.Update(true, true, controller, pawn, 0), "time zero cannot begin a shot");
    Check(hold.Update(true, false, controller, pawn, 399), "time-zero shot lost cooldown");
    Check(!hold.Update(true, false, controller, pawn, 400), "time-zero cooldown did not expire");
}

static void Thread() {
    Check(!IsViewportPoseThread(0, 0), "uninitialized thread ownership accepted");
    Check(!IsViewportPoseThread(71, 0), "input wrote before viewport discovery");
    Check(!IsViewportPoseThread(72, 71), "Present/input worker became a camera writer");
    Check(IsViewportPoseThread(71, 71), "viewport owner rejected");
    Check(!IsViewportPoseThread(71, 72), "old owner survived a viewport thread change");
}

static std::array<float, 16> Rotation(int32_t pitch, int32_t yaw, int32_t roll) {
    const int32_t rotation[3] = {pitch, yaw, roll};
    std::array<float, 16> result{};
    BuildStableViewRotation(rotation, result.data());
    return result;
}

static void RotationMath() {
    const auto neutral = Rotation(0, 0, 0);
    Near(neutral[2], 1, "neutral forward axis changed");
    Near(neutral[4], 1, "neutral right axis changed");
    Near(neutral[9], 1, "neutral up axis changed");
    for (int32_t pitch : {-16000, -5817, 0, 9000, 16000}) {
        for (int32_t yaw : {-100000, -16384, 0, 16384, INT_MAX}) {
            for (int32_t roll : {-20000, 0, 16384, INT_MIN}) {
                const auto matrix = Rotation(pitch, yaw, roll);
                for (float value : matrix) Check(std::isfinite(value), "nonfinite view basis");
                for (int a = 0; a < 3; ++a) {
                    for (int b = 0; b < 3; ++b) {
                        float dot = 0;
                        for (int row = 0; row < 3; ++row)
                            dot += matrix[row*4+a] * matrix[row*4+b];
                        Near(dot, a == b ? 1.0f : 0.0f, "view basis is not orthonormal");
                    }
                }
                Near(matrix[12], 0, "rotation contains world translation");
                Near(matrix[15], 1, "invalid homogeneous view basis");
            }
        }
    }
    const auto yawTurn = Rotation(0, 16384, 0);
    Near(yawTurn[6], 1, "quarter-turn yaw did not rotate forward");
    const auto wrapped = Rotation(0, 16384 + 65536*100, 0);
    for (int i = 0; i < 16; ++i) Near(wrapped[i], yawTurn[i], "whole turns changed basis");
}

static void Stereo() {
    Check(UseFrozenViewRotation(true, 7, true, false, false), "on-foot stereo retained native shake");
    Check(!UseFrozenViewRotation(true, 7, true, true, false), "vehicle basis overridden");
    Check(!UseFrozenViewRotation(false, 7, true, false, false), "desktop/native basis overridden");
    Check(!UseFrozenViewRotation(true, 0, true, false, false), "unpaired native view overridden");
    Check(!UseFrozenViewRotation(true, 7, false, false, false), "title/menu basis overridden");
    Check(UseFrozenViewRotation(false, 0, false, false, true), "first-person recovery lost its basis");
    const auto frozen = Rotation(0, 4187, 0);
    for (int32_t kick : {-5817, -1000, 0, 1000, 5817}) {
        for (int eye = 0; eye < 2; ++eye) {
            const auto native = Rotation(kick, 4187 + (eye ? 300 : -300), eye ? 200 : -200);
            const auto finalBasis = UseFrozenViewRotation(true, 7, true, false, false)
                ? frozen : native;
            for (int i = 0; i < 16; ++i)
                Near(finalBasis[i], frozen[i], "native firing shake leaked into one stereo eye");
        }
    }
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "specify a test case");
        const std::string test = argv[1];
        if (test == "reference") Reference();
        else if (test == "recoil") Recoil();
        else if (test == "thread") Thread();
        else if (test == "rotation") RotationMath();
        else if (test == "stereo") Stereo();
        else throw std::runtime_error("unknown test case");
        std::printf("PASS: camera_pose_%s\n", argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
