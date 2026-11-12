#include "../src/input/MagazinePose.hpp"
#include "../src/input/WeaponPosePolicy.hpp"
#include "../src/player/TrackingSpace.hpp"
#include "../src/render/ReloadHighlight.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

using namespace bl1gotyvr;
using namespace bl1gotyvr::player;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void Near(float actual, float expected, const char* message) {
    Check(std::isfinite(actual) && std::fabs(actual - expected) < 0.003f, message);
}

static std::array<float, 16> Frame(Vec3 origin, float yaw, float scale) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {c*scale, s*scale, 0, 0, -s*scale, c*scale, 0, 0,
            0, 0, scale, 0, origin.x, origin.y, origin.z, 1};
}

static void Affine() {
    // A stationary carried mag must survive a moving/scaled weapon component.
    const auto desired = Frame({2010, -420, 91}, -0.6f, 0.918f);
    for (float scale : {0.864f, 0.918f, 1.0f, 1.2f}) {
        for (int step = 0; step < 100; ++step) {
            auto component = Frame({2000 + step*1.7f, -500 + step*0.2f, 100},
                                   0.4f + step*0.01f, scale);
            // Nonuniform scale and shear must not be treated as rigid rotation.
            for (int i = 4; i < 7; ++i) component[i] *= 1.15f;
            component[8] += 0.08f;
            float local[16] = {};
            Check(input::MagazineWorldToComponent(component.data(), desired.data(), local),
                  "valid affine magazine rejected");
            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 4; ++column) {
                    float result = 0;
                    for (int k = 0; k < 4; ++k)
                        result += local[row*4+k] * component[k*4+column];
                    Near(result, desired[row*4+column], "weapon stamp moved the carried mag");
                }
            }
        }
    }
    auto invalid = Frame({}, 0, 0);
    float output[16];
    for (float& value : output) value = 7;
    Check(!input::MagazineWorldToComponent(invalid.data(), desired.data(), output),
          "singular component accepted");
    for (float value : output) Near(value, 7, "failed inverse modified output");
    invalid = Frame({}, 0, 1);
    invalid[8] = std::numeric_limits<float>::quiet_NaN();
    Check(!input::MagazineWorldToComponent(invalid.data(), desired.data(), output),
          "nonfinite matrix accepted");
    invalid = Frame({}, 0, 1);
    invalid[4] = 1; invalid[5] = 0; // Nonzero axes but determinant zero.
    Check(!input::MagazineWorldToComponent(invalid.data(), desired.data(), output),
          "linearly dependent axes accepted");
}

static void Tracking() {
    const Vec3 stage{0.3f, 1.1f, -0.4f};
    for (float scale : {50.0f, 100.0f, 140.0f}) {
        for (float yaw : {0.0f, 0.7f, -2.1f}) {
            for (float referenceYaw : {0.0f, 0.6f, -1.3f}) {
                TrackingSpaceFrame frame{{1234, -555, 97}, {0.1f, 1.5f, 0.2f},
                    QuatFromAxisAngle({0, 1, 0}, referenceYaw), yaw, scale, true};
                // Independent forward mapping used by UpdateTargets.
                const Vec3 xr = RotateByQuat(frame.referenceRotation.conjugate(),
                    stage - frame.referencePosition);
                const Vec3 ue{-xr.z, xr.x, xr.y};
                const float c = std::cos(yaw), s = std::sin(yaw);
                const Vec3 world = frame.anchor +
                    Vec3{c*ue.x - s*ue.y, s*ue.x + c*ue.y, ue.z} * scale;
                Vec3 actual;
                Check(TrackingWorldToStage(frame, world, actual), "valid tracking frame rejected");
                Near(actual.x, stage.x, "stage x mismatched rendered anchor");
                Near(actual.y, stage.y, "stage y mismatched rendered anchor");
                Near(actual.z, stage.z, "stage z mismatched rendered anchor");
                // Live camera motion must not change a previously frozen frame.
                auto later = frame;
                later.anchor.x += 200;
                Vec3 laterStage;
                Check(TrackingWorldToStage(later, world, laterStage) &&
                      (laterStage - stage).length() > 0.25f, "test camera movement too small");
                Check(TrackingWorldToStage(frame, world, actual), "frozen frame lost");
                Near(actual.x, stage.x, "later camera changed frozen conversion");
            }
        }
    }
    TrackingSpaceFrame invalid;
    Vec3 output{7, 8, 9};
    Check(!TrackingWorldToStage(invalid, {}, output), "unpublished tracking accepted");
    invalid.valid = true; invalid.scale = 0;
    Check(!TrackingWorldToStage(invalid, {}, output), "zero tracking scale accepted");
    invalid.scale = 100;
    invalid.referenceRotation.x = std::numeric_limits<float>::quiet_NaN();
    Check(!TrackingWorldToStage(invalid, {}, output), "nonfinite tracking quaternion accepted");
    Near(output.x, 7, "failed tracking conversion modified output");
}

static void Identity() {
    const input::MagazinePoseKey key{0x10000, 0x20000, 0x30000, 0x40000, 1, 4};
    Check(key.Valid() && key == key, "valid magazine key rejected");
    auto changed = key; changed.weapon++;
    Check(!(changed == key), "weapon swap reused seated pose");
    changed = key; changed.component++;
    Check(!(changed == key), "component swap reused seated pose");
    changed = key; changed.mesh++;
    Check(!(changed == key), "mesh swap reused seated pose");
    changed = key; changed.poseData++;
    Check(!(changed == key), "palette relocation reused write address");
    changed = key; changed.bone++;
    Check(!(changed == key), "bone swap reused seated pose");
    changed = key; changed.count++;
    Check(!(changed == key), "bone count mismatch reused seated pose");
    changed = key; changed.bone = 4;
    Check(!changed.Valid(), "out-of-bounds bone accepted");
    changed = key; changed.poseData = 0;
    Check(!changed.Valid(), "null palette accepted");
}

static void Grip() {
    Check(!input::CanAcquireMagazine(false, 0.1f, 0.26f), "unpressed trigger grabbed mag");
    Check(!input::CanAcquireMagazine(true, 0.4f, 0.26f), "distant mag acquired");
    Check(input::CanAcquireMagazine(true, 0.2f, 0.26f),
          "fresh trigger edge did not acquire inside zone");
    Check(!input::CanAcquireMagazine(false, 0.2f, 0.26f),
          "held trigger without a fresh edge acquired on entering zone");
    Check(input::CanAcquireMagazine(true, 0.26f, 0.26f), "zone boundary excluded");
    Check(!input::CanAcquireMagazine(true, std::numeric_limits<float>::quiet_NaN(), 0.26f),
          "invalid distance acquired mag");
    Check(!input::CanAcquireMagazine(true, 0.1f, std::numeric_limits<float>::infinity()),
          "invalid radius acquired mag");
}

static void Projection() {
    float u = 0, v = 0, radius = 0;
    const float center[3] = {0, 0, -1};
    Check(render::ProjectReloadHighlight(center, 0.1f, -1, 1, -1, 1, u, v, radius),
          "centered cue rejected");
    Near(u, 0.5f, "center u"); Near(v, 0.5f, "center v");
    Near(radius, 0.05f, "cue radius");
    // Original v0.5.7.3: asymmetric X, centered Y, per destination eye.
    Check(render::ProjectReloadHighlight(center, 0.1f, -0.8f, 1.2f, -0.6f, 1.4f,
                                        u, v, radius), "cropped cue rejected");
    Near(u, 0.4f, "asymmetric horizontal projection");
    Near(v, 0.5f, "cue must match centered source horizon, not raw asymmetric FOV");
    const float point[3] = {0.2f, 0.3f, -1};
    Check(render::ProjectReloadHighlight(point, 0.1f, -0.8f, 1.2f, -0.6f, 1.4f, u, v, radius),
          "cropped point rejected");
    Near(u, 0.5f, "original horizontal mapping changed");
    Near(v, 0.35f, "original centered vertical mapping changed");
    // Cross-check the original formulas over unequal left/right eye FOVs.
    for (float tanLeft : {-1.2f, -0.8f}) {
        for (float x : {-0.2f, 0.0f, 0.3f}) {
            for (float y : {-0.3f, 0.1f, 0.2f}) {
                const float local[3] = {x, y, -0.7f};
                Check(render::ProjectReloadHighlight(local, 0.11f, tanLeft, 1.3f,
                        -1.1f, 0.9f, u, v, radius), "original reference point rejected");
                Near(u, (x/0.7f - tanLeft)/(1.3f - tanLeft), "original eye X formula changed");
                Near(v, (1.0f - y/0.7f)/2.0f, "original eye Y formula changed");
            }
        }
    }
    const float behind[3] = {0, 0, 1};
    Check(!render::ProjectReloadHighlight(behind, 0.1f, -1, 1, -1, 1, u, v, radius),
          "cue behind eye accepted");
    const float offscreen[3] = {3, 0, -1};
    Check(!render::ProjectReloadHighlight(offscreen, 0.1f, -1, 1, -1, 1, u, v, radius),
          "offscreen cue accepted");
    Check(!render::ProjectReloadHighlight(center, 0.1f, -1, 1, 1, -1, u, v, radius),
          "inverted FOV accepted");
}

static void Capture() {
    Check(!input::NeedsOriginalMagazineCapture(0, false), "idle equip pose must not be cached");
    Check(input::NeedsOriginalMagazineCapture(1, false), "first grab must capture seated pose");
    Check(!input::NeedsOriginalMagazineCapture(1, true), "fresh pouch mag must reuse grabbed orientation");
    Check(!input::NeedsOriginalMagazineCapture(2, true), "discard must retain grabbed orientation");
    Check(input::NeedsOriginalMagazineCapture(3, false), "first cylinder release must capture seated pose");
    Check(input::NeedsOriginalMagazineCapture(3, true), "cylinder release must refresh its basis");
}

static void Owner() {
    Check(!input::UseNativeIkAttachment(false, false, false), "bootstrap lost canonical fallback");
    Check(!input::UseNativeIkAttachment(false, true, false), "invalid rig took ownership");
    Check(!input::UseNativeIkAttachment(false, false, true), "rig without hook took ownership");
    bool owner = input::UseNativeIkAttachment(false, true, true);
    Check(owner, "original native IK attachment never acquired ownership");
    Check(input::UseNativeIkAttachment(owner, true, false), "transient discovery reenabled competing canonical writer");
    Check(input::UseNativeIkAttachment(owner, false, false), "latched ownership was lost between eyes");
    owner = false; // Weapon/component change or calibration reset.
    Check(!input::UseNativeIkAttachment(owner, true, false), "new invalid component inherited old ownership");
    Check(input::UseNativeIkAttachment(owner, true, true), "new valid rig failed to acquire attachment");
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "specify a test case");
        const std::string test = argv[1];
        if (test == "affine") Affine();
        else if (test == "tracking") Tracking();
        else if (test == "identity") Identity();
        else if (test == "grip") Grip();
        else if (test == "projection") Projection();
        else if (test == "capture") Capture();
        else if (test == "owner") Owner();
        else throw std::runtime_error("unknown test case");
        std::printf("PASS: manual_reload_%s\n", argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
