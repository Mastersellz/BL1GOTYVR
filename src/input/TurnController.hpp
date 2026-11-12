#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bl1gotyvr::input {

enum class TurnContext { Gameplay, Native, Blocked };

struct TurnSettings {
    int mode = 1; // 0=snap, 1=smooth, 2=off (on foot only)
    float snapDegrees = 30.0f;
    float smoothDegreesPerSecond = 90.0f;
    float deadzone = 0.2f;
};

struct TurnOutput {
    double yawDegrees = 0.0;
    float nativeAxis = 0.0f;
};

// No OS calls or game writes. One snap per neutral -> deflected gesture;
// neither frame rate nor mouse/gamepad sensitivity determines the snap angle.
class TurnController {
public:
    void Reset() { m_armed = false; m_mode = -1; }

    TurnOutput Update(float stick, double seconds, const TurnSettings& settings,
                      TurnContext context) {
        if (!std::isfinite(stick)) { Reset(); return {}; }
        stick = std::clamp(stick, -1.0f, 1.0f);
        if (context != TurnContext::Gameplay) {
            Reset();
            return {0.0, context == TurnContext::Native ? stick : 0.0f};
        }
        if (settings.mode != m_mode) {
            m_armed = false;
            m_mode = settings.mode;
        }
        if (!std::isfinite(settings.deadzone) || settings.deadzone < 0.0f ||
            settings.deadzone > 0.5f || settings.mode < 0 || settings.mode > 1 ||
            !std::isfinite(seconds) || seconds <= 0.0 || seconds > 0.1) {
            m_armed = false;
            return {};
        }
        const float magnitude = std::fabs(stick);
        if (magnitude <= settings.deadzone) { m_armed = true; return {}; }
        if (!m_armed) return {};
        if (settings.mode == 0) {
            if (!std::isfinite(settings.snapDegrees) || settings.snapDegrees < 5.0f ||
                settings.snapDegrees > 90.0f || magnitude < 0.65f) return {};
            m_armed = false;
            return {std::copysign(static_cast<double>(settings.snapDegrees), stick), 0.0f};
        }
        if (!std::isfinite(settings.smoothDegreesPerSecond) ||
            settings.smoothDegreesPerSecond < 10.0f ||
            settings.smoothDegreesPerSecond > 360.0f) return {};
        const double axis = (magnitude - settings.deadzone) / (1.0 - settings.deadzone);
        return {std::copysign(axis * settings.smoothDegreesPerSecond * seconds, stick), 0.0f};
    }

private:
    bool m_armed = false;
    int m_mode = -1;
};

// Externally synchronized mailbox between Present/input and the game thread.
// Expire the oldest queued sample rather than refreshing old turn debt forever.
class TurnMailbox {
public:
    void Clear() { m_degrees = 0; m_controller = m_pawn = m_firstMs = 0; }
    void Add(double degrees, uintptr_t controller, uintptr_t pawn, uint64_t nowMs) {
        if (!std::isfinite(degrees) || std::fabs(degrees) > 90.0 ||
            controller < 0x10000 || pawn < 0x10000 || !nowMs) { Clear(); return; }
        if (degrees == 0.0) return;
        if (controller != m_controller || pawn != m_pawn ||
            nowMs < m_firstMs || nowMs - m_firstMs > 100) Clear();
        if (!m_firstMs) {
            m_controller = controller;
            m_pawn = pawn;
            m_firstMs = nowMs;
        }
        m_degrees += degrees;
        if (std::fabs(m_degrees) > 180.0) Clear();
    }
    double Consume(uintptr_t controller, uintptr_t pawn, uint64_t nowMs) {
        const double result = controller == m_controller && pawn == m_pawn &&
            nowMs >= m_firstMs && nowMs - m_firstMs <= 100 ? m_degrees : 0.0;
        Clear();
        return result;
    }

private:
    double m_degrees = 0.0;
    uintptr_t m_controller = 0, m_pawn = 0;
    uint64_t m_firstMs = 0;
};

// Keep fractional UE3 rotator units across smooth-turn samples. Modulo through
// int64 avoids signed overflow at +/-INT32_MAX without changing pitch or roll.
inline int32_t QuantizeTurn(double degrees, double& remainder) {
    if (!std::isfinite(degrees) || std::fabs(degrees) > 180.0 ||
        !std::isfinite(remainder) || std::fabs(remainder) > 0.5) {
        remainder = 0.0;
        return 0;
    }
    const double units = degrees * (65536.0 / 360.0) + remainder;
    const auto rounded = static_cast<int32_t>(std::llround(units));
    remainder = units - rounded;
    return rounded;
}

inline int32_t AddYaw(int32_t yaw, int32_t delta) {
    const int64_t wrapped = (static_cast<int64_t>(yaw) + delta) % 65536;
    return static_cast<int32_t>(wrapped < 0 ? wrapped + 65536 : wrapped);
}

} // namespace bl1gotyvr::input
