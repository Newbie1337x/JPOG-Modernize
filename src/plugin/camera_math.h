#pragma once
#include <math.h>

namespace CameraMath {
inline float Clamp(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}
struct OrbitPose {
    float pitch = 0, yaw = 0, distance = 45;
    float velocity[6] = {}; // forward, right, yaw, pitch, distance, height
};
// Exact integration of a first-order velocity response over one frame.
// Inputs are frame displacements from native key/mouse input, not velocities.
inline float IntegrateDisplacement(float input, float dt, float seconds, float& velocity)
{
    if (!(dt > 0.0f) || dt > 0.25f) { velocity = 0; return 0; }
    const float target = input / dt;
    const float decay = expf(-dt / seconds);
    const float distance = target * dt + (velocity - target) * seconds * (1.0f - decay);
    velocity = target + (velocity - target) * decay;
    return distance;
}
inline void StepOrbit(OrbitPose& pose, const float input[6], float dt, float seconds,
    float minPitch, float maxPitch, float minDistance, float maxDistance, float movement[3])
{
    float delta[6];
    for (int i = 0; i < 6; ++i) delta[i] = IntegrateDisplacement(input[i], dt, seconds, pose.velocity[i]);
    movement[0] = delta[0]; movement[1] = delta[1]; movement[2] = delta[5];
    const float pitch = pose.pitch + delta[3];
    pose.pitch = Clamp(pitch, minPitch, maxPitch);
    if (pitch != pose.pitch) pose.velocity[3] = 0;
    pose.yaw += delta[2];
    pose.yaw = fmodf(pose.yaw, 6.28318531f);
    const float distance = pose.distance + delta[4];
    pose.distance = Clamp(distance, minDistance, maxDistance);
    if (distance != pose.distance) pose.velocity[4] = 0;
    // Zoom does not touch pitch/yaw. Height does not touch distance/angles.
}
inline float ResponseDamping(float dt, float seconds)
{
    if (!(dt > 0.0f)) return 1.0f;
    if (dt > 0.25f) dt = 0.25f;
    return expf(-dt / seconds);
}
inline float ZoomRatio(float distance)
{
    float ratio = distance / 45.0f;
    if (!(ratio > 0.35f)) return 0.35f;
    return ratio > 2.0f ? 2.0f : ratio;
}
}
