#pragma once

#include "TwoBoneIK.hpp"

#include <cstdint>

namespace bl1gotyvr::player {

enum class PhysicalPose : uint8_t { Standing, Crouching, Prone };

struct TrackedBodyInput {
    Vec3 bodyAnchor;
    Vec3 headPosition;
    Quat headRotation;
    Vec3 leftHandPosition;
    Quat leftHandRotation;
    Vec3 rightHandPosition;
    Quat rightHandRotation;
    bool leftHandValid = false;
    bool rightHandValid = false;
};

struct RoomScaleBodySettings {
    bool enabled = true;
    bool allowHorizontal = true;
    bool allowVertical = true;
    float followStrength = 10.0f;
    float calibratedHeight = 170.0f;
    float headToChest = 45.0f;
    float headToPelvis = 85.0f;
    float shoulderWidth = 42.0f;
    float standingThreshold = 0.72f;
    float proneThreshold = 0.35f;
    float poseHysteresis = 0.04f;
};

struct RoomScaleBodyPose {
    Vec3 virtualRoot;
    Vec3 head;
    Vec3 pelvis;
    Vec3 chest;
    Vec3 leftShoulder;
    Vec3 rightShoulder;
    Vec3 forward{1.0f, 0.0f, 0.0f};
    Vec3 right{0.0f, 1.0f, 0.0f};
    Vec3 up{0.0f, 0.0f, 1.0f};
    Vec3 physicalOffset;
    float heightRatio = 1.0f;
    PhysicalPose physicalPose = PhysicalPose::Standing;
    bool valid = false;
};

class RoomScaleBody {
public:
    void Reset(const Vec3& bodyAnchor, const Vec3& headPosition,
               float calibratedHeight);
    RoomScaleBodyPose Update(const TrackedBodyInput& input, float deltaTime,
                             const RoomScaleBodySettings& settings);

private:
    Vec3 m_calibratedPhysicalOffset;
    Vec3 m_bodyOffset;
    float m_calibratedHeight = 170.0f;
    PhysicalPose m_physicalPose = PhysicalPose::Standing;
    bool m_calibrated = false;
};

bool RunRoomScaleBodySelfTest();

} // namespace bl1gotyvr::player
