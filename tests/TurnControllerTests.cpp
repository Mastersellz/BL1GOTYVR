#include "../src/input/TurnController.hpp"

#include <climits>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

using namespace bl1gotyvr::input;

static void Check(bool passed, const char* message) {
    if (!passed) throw std::runtime_error(message);
}

static void Near(double actual, double expected, const char* message) {
    Check(std::isfinite(actual) && std::fabs(actual - expected) < 1.0e-4, message);
}

static void Smooth() {
    const TurnSettings settings;
    for (int fps : {30, 60, 72, 90, 120, 144, 240}) {
        for (float axis : {-1.0f, -0.6f, 0.6f, 1.0f}) {
            TurnController turn;
            turn.Update(0, 1.0 / fps, settings, TurnContext::Gameplay);
            double degrees = 0;
            for (int frame = 0; frame < fps; ++frame) {
                const auto output = turn.Update(axis, 1.0 / fps, settings, TurnContext::Gameplay);
                Check(output.nativeAxis == 0, "smooth turning also leaked a native axis");
                degrees += output.yawDegrees;
            }
            Near(degrees, std::copysign(std::fabs(axis) == 1 ? 90.0 : 45.0, axis),
                 "smooth speed depends on frame rate or direction");
        }
    }
    TurnController turn;
    turn.Update(0, 0.01, settings, TurnContext::Gameplay);
    for (float axis : {-0.2f, -0.01f, 0.0f, 0.2f})
        Near(turn.Update(axis, 0.01, settings, TurnContext::Gameplay).yawDegrees,
             0, "deadzone jitter causes rotation");
    auto fast = settings;
    fast.smoothDegreesPerSecond = 180;
    Near(turn.Update(1, 0.05, fast, TurnContext::Gameplay).yawDegrees, 9,
         "live smooth speed was ignored");
}

static void Snap() {
    for (float angle : {5.0f, 30.0f, 45.0f, 90.0f}) {
        TurnSettings settings;
        settings.mode = 0;
        settings.snapDegrees = angle;
        TurnController turn;
        Near(turn.Update(1, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
             "held stick on activation caused snap");
        turn.Update(0, 0.01, settings, TurnContext::Gameplay);
        Near(turn.Update(0.64f, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
             "snap threshold is too low");
        Near(turn.Update(0.65f, 0.01, settings, TurnContext::Gameplay).yawDegrees, angle,
             "snap angle is not the configured angle");
        for (int frame = 0; frame < 240; ++frame)
            Near(turn.Update(1, 1.0 / 240, settings, TurnContext::Gameplay).yawDegrees,
                 0, "holding stick repeats snap");
        Near(turn.Update(-1, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
             "direction reversal without centering repeats snap");
        turn.Update(0.2f, 0.01, settings, TurnContext::Gameplay);
        Near(turn.Update(-1, 0.01, settings, TurnContext::Gameplay).yawDegrees, -angle,
             "neutral did not rearm opposite snap");
    }
}

static void Safety() {
    TurnSettings settings;
    TurnController turn;
    turn.Update(0, 0.01, settings, TurnContext::Gameplay);
    const auto native = turn.Update(-0.7f, 0.01, settings, TurnContext::Native);
    Near(native.nativeAxis, -0.7, "vehicle/menu native axis changed");
    Near(native.yawDegrees, 0, "vehicle/menu generated a native yaw write");
    Near(turn.Update(1, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
         "return from native controls did not require neutral");
    turn.Update(0, 0.01, settings, TurnContext::Gameplay);
    const auto blocked = turn.Update(1, 0.01, settings, TurnContext::Blocked);
    Near(blocked.yawDegrees, 0, "blocked input generated yaw");
    Near(blocked.nativeAxis, 0, "blocked input leaked native steering");
    Near(turn.Update(1, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
         "resuming tracking/focus caused turn without neutral");
    settings.mode = 2;
    turn.Update(0, 0.01, settings, TurnContext::Gameplay);
    Near(turn.Update(1, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
         "off mode turned the player");
    Near(turn.Update(1, 0.01, settings, TurnContext::Native).nativeAxis, 1,
         "off foot mode disabled vehicle controls");
    settings.mode = 0;
    Near(turn.Update(1, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
         "live mode change snapped with held stick");
    for (double dt : {0.0, -1.0, 0.101, 10.0, std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::quiet_NaN()}) {
        turn.Update(0, 0.01, settings, TurnContext::Gameplay);
        Near(turn.Update(1, dt, settings, TurnContext::Gameplay).yawDegrees, 0,
             "invalid/stalled frame interval caused snap");
    }
    turn.Update(0, 0.01, settings, TurnContext::Gameplay);
    Near(turn.Update(std::numeric_limits<float>::quiet_NaN(), 0.01,
                     settings, TurnContext::Gameplay).yawDegrees, 0, "NaN stick caused turn");
    settings.mode = 1;
    settings.deadzone = 0;
    turn.Update(0, 0.01, settings, TurnContext::Gameplay);
    Check(turn.Update(1, 0.01, settings, TurnContext::Gameplay).yawDegrees > 0,
          "zero deadzone cannot rearm turning");
    turn.Reset();
    Near(turn.Update(1, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
         "reset retained armed state");
    settings.deadzone = 1;
    Near(turn.Update(0, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
         "invalid deadzone accepted");
    settings.deadzone = 0.2f;
    settings.smoothDegreesPerSecond = std::numeric_limits<float>::infinity();
    turn.Update(0, 0.01, settings, TurnContext::Gameplay);
    Near(turn.Update(1, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
         "nonfinite turn speed accepted");
    settings.mode = 0;
    settings.snapDegrees = std::numeric_limits<float>::quiet_NaN();
    turn.Update(0, 0.01, settings, TurnContext::Gameplay);
    Near(turn.Update(1, 0.01, settings, TurnContext::Gameplay).yawDegrees, 0,
         "nonfinite snap angle accepted");
}

static void Queue() {
    TurnMailbox mailbox;
    constexpr uintptr_t controller = 0x100000, pawn = 0x200000;
    mailbox.Add(1, controller, pawn, 1000);
    mailbox.Add(2, controller, pawn, 1010);
    Near(mailbox.Consume(controller, pawn, 1020), 3, "pair-boundary accumulation lost turns");
    Near(mailbox.Consume(controller, pawn, 1020), 0, "same request applied to both eyes");
    mailbox.Add(30, controller, pawn, 2000);
    Near(mailbox.Consume(controller, pawn + 1, 2010), 0, "turn reached a different pawn");
    mailbox.Add(30, controller, pawn, 2000);
    Near(mailbox.Consume(controller + 1, pawn, 2010), 0, "turn reached a different controller");
    mailbox.Add(30, controller, pawn, 2000);
    Near(mailbox.Consume(controller, pawn, 2101), 0, "stale turn survived");
    mailbox.Add(30, controller, pawn, 2000);
    Near(mailbox.Consume(controller, pawn, 1999), 0, "backward clock accepted");
    mailbox.Add(30, controller, pawn, 2000);
    mailbox.Add(1, controller, pawn, 2101);
    Near(mailbox.Consume(controller, pawn, 2102), 1, "new samples kept stale turn debt");
    mailbox.Add(30, controller, pawn, 3000);
    mailbox.Clear();
    Near(mailbox.Consume(controller, pawn, 3001), 0, "focus loss did not cancel pending turn");
    mailbox.Add(90, controller, pawn, 4000);
    mailbox.Add(90, controller, pawn, 4001);
    mailbox.Add(90, controller, pawn, 4002);
    Near(mailbox.Consume(controller, pawn, 4003), 0, "unbounded accumulated turn accepted");
    mailbox.Add(30, controller, pawn, 5000);
    mailbox.Add(2, controller, pawn + 1, 5001);
    Near(mailbox.Consume(controller, pawn + 1, 5002), 2, "identity transition retained old debt");
    mailbox.Add(std::numeric_limits<double>::quiet_NaN(), controller, pawn, 5000);
    Near(mailbox.Consume(controller, pawn, 5001), 0, "NaN entered mailbox");
    mailbox.Add(30, 0, pawn, 6000);
    Near(mailbox.Consume(0, pawn, 6001), 0, "invalid controller entered mailbox");
}

static void Rotator() {
    for (int fps : {30, 60, 72, 90, 144, 240}) {
        double remainder = 0;
        int total = 0;
        for (int frame = 0; frame < fps; ++frame) total += QuantizeTurn(90.0 / fps, remainder);
        Check(total == 16384, "smooth rotator quantization depends on FPS");
    }
    double remainder = 0;
    int total = 0;
    for (int i = 0; i < 10000; ++i) total += QuantizeTurn(0.0001, remainder);
    Check(total == 182, "sub-unit turn samples were discarded");
    for (int i = 0; i < 10000; ++i) total += QuantizeTurn(-0.0001, remainder);
    Check(total == 0, "symmetric turns accumulated quantization drift");
    int32_t yaw = 0;
    remainder = 0;
    for (int i = 0; i < 12; ++i) yaw = AddYaw(yaw, QuantizeTurn(30, remainder));
    Check(yaw == 0, "twelve 30-degree snaps did not complete a circle");
    Check(AddYaw(65530, 10) == 4, "positive yaw wrap");
    Check(AddYaw(4, -10) == 65530, "negative yaw wrap");
    Check(AddYaw(INT_MAX, 1) == 0 && AddYaw(INT_MIN, -1) == 65535, "signed yaw overflow");
    remainder = std::numeric_limits<double>::infinity();
    Check(QuantizeTurn(1, remainder) == 0 && remainder == 0, "invalid fractional remainder accepted");
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "specify a test case");
        const std::string test = argv[1];
        if (test == "smooth") Smooth();
        else if (test == "snap") Snap();
        else if (test == "safety") Safety();
        else if (test == "queue") Queue();
        else if (test == "rotator") Rotator();
        else throw std::runtime_error("unknown test case");
        std::printf("PASS: turn_%s\n", argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
