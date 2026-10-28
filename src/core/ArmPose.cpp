#include "core/ArmPose.h"

#include <algorithm>
#include <cmath>

namespace lo::core::armpose {

namespace {
constexpr float kPi = 3.14159265358979323846f;

char const* sideName(Side side) { return side == Side::Right ? "right" : "left"; }
char        axisName(Axis axis) { return axis == Axis::X ? 'x' : axis == Axis::Y ? 'y' : 'z'; }

ArmPose armPose(Side side, float x, float offset, ArmOverride const& o) {
    ArmPose a;
    if (!o.render) return a;
    float sign = side == Side::Right ? 1.0f : -1.0f;
    a.active   = true;
    a.rotation = {-x, sign * offset, -sign * kSpreadZ};
    a.crouchX  = true;
    for (int i = 0; i < 3; ++i) {
        if (!o.rotation[i]) continue;
        a.rotation[i] = *o.rotation[i];
        if (i == 0) a.crouchX = false;
    }
    return a;
}
} // namespace

float blockRenderWidth(float shapeWidth, bool itemOverride) { return itemOverride ? 0.8f : shapeWidth; }

float entityRenderWidth(float width, bool sideways) { return sideways ? width - 0.35f * width * width : width * 0.9f; }

float armOffset(float renderWidth) { return std::min((renderWidth - 1.0f) / 1.5f, kMaxOffset); }

Pose computePose(bool entityOrPlayer, float renderWidth, ArmOverride const& right, ArmOverride const& left, bool renderArms) {
    Pose pose;
    if (!renderArms) return pose;
    float x      = kBaseX + (entityOrPlayer ? kEntityX : 0.0f);
    float offset = armOffset(renderWidth);
    pose.right   = armPose(Side::Right, x, offset, right);
    pose.left    = armPose(Side::Left, x, offset, left);
    return pose;
}

std::array<float, 3> javaRotation(ArmPose const& arm, bool crouching) {
    auto r = arm.rotation;
    if (arm.crouchX && crouching) r[0] -= kCrouchX;
    return r;
}

float toDegrees(float radians) { return radians * 180.0f / kPi; }

int quantize(float radians) {
    if (!std::isfinite(radians)) return 0;
    long i = std::lround(radians / kStep);
    return static_cast<int>(std::clamp<long>(i, -kMaxIndex, kMaxIndex));
}

float dequantize(int index) { return static_cast<float>(index) * kStep; }

std::string animationName(Side side, Axis axis, int index, bool crouchX) {
    std::string name = "animation.liftoff.carry.";
    name += sideName(side);
    name += '.';
    name += axisName(axis);
    if (crouchX && axis == Axis::X) name += 'c';
    name += '.';
    if (index < 0) name += 'm' + std::to_string(-index);
    else if (index > 0) name += 'p' + std::to_string(index);
    else name += '0';
    return name;
}

std::string controllerName(Side side, Axis axis) {
    std::string name = "liftoff.carry.";
    name += sideName(side);
    name += '.';
    name += axisName(axis);
    return name;
}

std::string nativeVariable(Side side, char const* suffix) {
    std::string name = "variable.liftoff_carry_";
    name += side == Side::Right ? 'r' : 'l';
    name += suffix;
    return name;
}

std::vector<SlotAnimation> vanillaAnimations(Pose const& pose) {
    std::vector<SlotAnimation> out;
    for (Side side : {Side::Right, Side::Left}) {
        ArmPose const& arm = pose.arm(side);
        for (Axis axis : {Axis::X, Axis::Y, Axis::Z}) {
            auto i = static_cast<std::size_t>(axis);
            out.push_back({
                controllerName(side, axis),
                arm.active ? animationName(side, axis, quantize(arm.rotation[i]), axis == Axis::X && arm.crouchX)
                           : std::string(kNoneAnimation),
            });
        }
    }
    out.push_back({kFirstPersonController, pose.right.active ? kFirstPersonAnimation : kNoneAnimation});
    return out;
}

std::vector<std::string> allControllers() {
    std::vector<std::string> out;
    for (auto const& s : vanillaAnimations(Pose{})) out.push_back(s.controller);
    return out;
}

} // namespace lo::core::armpose
