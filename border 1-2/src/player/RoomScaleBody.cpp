#include "RoomScaleBody.hpp"

#include <algorithm>
#include <cmath>

namespace bl1gotyvr::player {
namespace {

bool IsFinite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

Vec3 Lerp(const Vec3& from, const Vec3& to, float alpha) {
    return from + (to - from) * alpha;
}

} // namespace

void RoomScaleBody::Reset(const Vec3& bodyAnchor, const Vec3& headPosition,
                          float calibratedHeight) {
    m_calibratedPhysicalOffset = headPosition - bodyAnchor;
    m_bodyOffset = {};
    m_calibratedHeight = std::max(50.0f, calibratedHeight);
    m_physicalPose = PhysicalPose::Standing;
    m_calibrated = IsFinite(bodyAnchor) && IsFinite(headPosition) &&
        std::isfinite(calibratedHeight);
}

RoomScaleBodyPose RoomScaleBody::Update(const TrackedBodyInput& input,
                                        float deltaTime,
                                        const RoomScaleBodySettings& settings) {
    RoomScaleBodyPose result;
    if (!IsFinite(input.bodyAnchor) || !IsFinite(input.headPosition)) return result;
    if (!m_calibrated)
        Reset(input.bodyAnchor, input.headPosition, settings.calibratedHeight);
    if (!m_calibrated) return result;

    Vec3 physicalOffset = input.headPosition - input.bodyAnchor -
        m_calibratedPhysicalOffset;
    if (!settings.enabled) physicalOffset = {};
    if (!settings.allowHorizontal) {
        physicalOffset.x = 0.0f;
        physicalOffset.y = 0.0f;
    }
    if (!settings.allowVertical) physicalOffset.z = 0.0f;

    const float safeDelta = std::clamp(
        std::isfinite(deltaTime) ? deltaTime : 0.0f, 0.0f, 0.1f);
    const float response = std::max(0.0f, settings.followStrength);
    const float alpha = response > 0.0f
        ? 1.0f - std::exp(-response * safeDelta) : 1.0f;
    m_bodyOffset = Lerp(m_bodyOffset, physicalOffset, alpha);

    result.virtualRoot = input.bodyAnchor + m_bodyOffset;
    result.head = result.virtualRoot;
    result.physicalOffset = physicalOffset;
    result.heightRatio = std::max(0.0f,
        (m_calibratedHeight + physicalOffset.z) / m_calibratedHeight);
    const Vec3 headForward = RotateByQuat(input.headRotation, {1.0f, 0.0f, 0.0f});
    result.forward = Vec3{headForward.x, headForward.y, 0.0f}.normalized();
    if (result.forward.lengthSq() < 1.0e-6f) result.forward = {1.0f, 0.0f, 0.0f};
    result.right = {-result.forward.y, result.forward.x, 0.0f};
    result.chest = result.head + Vec3{0.0f, 0.0f, -settings.headToChest};
    result.pelvis = result.head + Vec3{0.0f, 0.0f, -settings.headToPelvis};
    const float halfShoulderWidth = settings.shoulderWidth * 0.5f;
    result.leftShoulder = result.chest - result.right * halfShoulderWidth;
    result.rightShoulder = result.chest + result.right * halfShoulderWidth;

    const float standingThreshold = std::max(
        settings.proneThreshold, settings.standingThreshold);
    const float hysteresis = std::max(0.0f, settings.poseHysteresis);
    if (m_physicalPose == PhysicalPose::Standing) {
        if (result.heightRatio <= settings.proneThreshold - hysteresis)
            m_physicalPose = PhysicalPose::Prone;
        else if (result.heightRatio <= standingThreshold - hysteresis)
            m_physicalPose = PhysicalPose::Crouching;
    } else if (m_physicalPose == PhysicalPose::Crouching) {
        if (result.heightRatio >= standingThreshold + hysteresis)
            m_physicalPose = PhysicalPose::Standing;
        else if (result.heightRatio <= settings.proneThreshold - hysteresis)
            m_physicalPose = PhysicalPose::Prone;
    } else {
        if (result.heightRatio >= standingThreshold + hysteresis)
            m_physicalPose = PhysicalPose::Standing;
        else if (result.heightRatio >= settings.proneThreshold + hysteresis)
            m_physicalPose = PhysicalPose::Crouching;
    }
    result.physicalPose = m_physicalPose;
    result.valid = IsFinite(result.virtualRoot) && IsFinite(result.chest) &&
        IsFinite(result.pelvis) && IsFinite(result.leftShoulder) &&
        IsFinite(result.rightShoulder) && std::isfinite(result.heightRatio);
    return result;
}

bool RunRoomScaleBodySelfTest() {
    RoomScaleBody body;
    RoomScaleBodySettings settings;
    settings.followStrength = 100.0f;
    const Vec3 anchor{100.0f, 200.0f, 300.0f};
    body.Reset(anchor, anchor, 170.0f);
    TrackedBodyInput input;
    input.bodyAnchor = anchor;
    input.headPosition = anchor + Vec3{30.0f, -20.0f, -85.0f};
    const RoomScaleBodyPose pose = body.Update(input, 1.0f, settings);
    return pose.valid && pose.physicalPose == PhysicalPose::Crouching;
}

} // namespace bl1gotyvr::player
