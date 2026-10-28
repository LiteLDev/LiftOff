#pragma once

// Carry On's arm pose while carrying (docs/SPEC.md §4.5), without the game: the Java values, their
// quantisation for vanilla clients and the names of the resource-pack animations that show them.
//
// Java (radians, model axes = Bedrock animation axes, Bedrock degrees = radians * 180 / pi):
//   x = 1.0 + (crouching ? 0.2 : 0) + (entity or player carried ? 0.3 : 0)
//   offset = min((renderWidth - 1) / 1.5, 0.2)
//   right arm (-x, +offset, -0.05), left arm (-x, -offset, +0.05)
// Script `rotation_*_arm` values replace single axes; `render_*_arm` false leaves that arm vanilla.
//
// Bedrock (resource pack `animations/liftoff_carry.animation.json`, written by
// `tools/armpose/animations.ps1`): one animation per arm, axis and value, each setting only its own
// axis absolutely (`-this + value`), so the six slots can play in any order. Every client gets them
// from the server (`AnimateEntityPacket`, one runtime controller per slot); crouching is read on the
// client (`query.is_sneaking`), so the server only re-sends on carry changes.

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace lo::core::armpose {

inline constexpr float kBaseX   = 1.0f;  ///< arm x rotation (radians) while carrying
inline constexpr float kCrouchX = 0.2f;  ///< added while crouching
inline constexpr float kEntityX = 0.3f;  ///< added for entities and players
inline constexpr float kSpreadZ = 0.05f; ///< arm z rotation (right -, left +)
inline constexpr float kMaxOffset = 0.2f;

/// Quantisation of the vanilla animations: 0.05 rad steps, indices -63..63 (about ±pi).
inline constexpr float kStep     = 0.05f;
inline constexpr int   kMaxIndex = 63;

/// Bedrock's sneaking animation tilts the player's root (and with it the arms) 28° forward; Java's
/// crouch does not move the arms, so the animations subtract it while sneaking.
inline constexpr float kSneakRootDegrees = 28.0f;

enum class Side { Right, Left };
enum class Axis { X, Y, Z };

/// Script values for one arm (`rotation_*_arm`, radians; nullopt = computed) and `render_*_arm`.
struct ArmOverride {
    std::array<std::optional<float>, 3> rotation;
    bool                                render = true;
};

/// The pose of one arm.
struct ArmPose {
    bool                 active = false;        ///< false: the vanilla pose (no carry or render_*_arm false)
    std::array<float, 3> rotation{0.f, 0.f, 0.f}; ///< radians, without the crouch term
    bool                 crouchX = false;       ///< x is computed: -0.2 more while crouching

    friend bool operator==(ArmPose const&, ArmPose const&) = default;
};

struct Pose {
    ArmPose right, left;
    [[nodiscard]] ArmPose const& arm(Side side) const { return side == Side::Right ? right : left; }
    friend bool                  operator==(Pose const&, Pose const&) = default;
};

/// `renderWidth` of a carried block: the shape width, 0.8 with an item model override.
[[nodiscard]] float blockRenderWidth(float shapeWidth, bool itemOverride);
/// `renderWidth` of a carried entity: width * 0.9, or `w - 0.35 w^2` with `rotateEntitiesSideways`.
[[nodiscard]] float entityRenderWidth(float width, bool sideways);
/// `min((renderWidth - 1) / 1.5, 0.2)`.
[[nodiscard]] float armOffset(float renderWidth);

/// The arm pose of a carry. `renderArms` false (client setting) gives no pose at all.
[[nodiscard]] Pose computePose(
    bool               entityOrPlayer,
    float              renderWidth,
    ArmOverride const& right      = {},
    ArmOverride const& left       = {},
    bool               renderArms = true
);

/// The rotation Carry On gives the arm (radians), crouch term included.
[[nodiscard]] std::array<float, 3> javaRotation(ArmPose const& arm, bool crouching);

[[nodiscard]] float toDegrees(float radians);
/// Nearest step index, clamped to ±kMaxIndex.
[[nodiscard]] int   quantize(float radians);
[[nodiscard]] float dequantize(int index);

/// "animation.liftoff.carry.right.x.m20" (index -20 = -1.00 rad); `crouchX`: the ".xc." family that
/// also adds the crouch term (x only).
[[nodiscard]] std::string animationName(Side side, Axis axis, int index, bool crouchX = false);
/// Runtime controller (slot) of an arm axis: "liftoff.carry.right.x".
[[nodiscard]] std::string controllerName(Side side, Axis axis);

/// First-person pose of the right arm (the only arm Bedrock draws in first person).
inline constexpr char kFirstPersonAnimation[]  = "animation.liftoff.carry.first_person.right";
inline constexpr char kFirstPersonController[] = "liftoff.carry.first_person.right";
/// An empty animation: played into a slot to end what it played.
inline constexpr char kNoneAnimation[] = "animation.liftoff.carry.none";
/// One animation played into one runtime controller.
struct SlotAnimation {
    std::string controller;
    std::string animation;
    friend bool operator==(SlotAnimation const&, SlotAnimation const&) = default;
};
/// What a vanilla client plays for `pose`: one entry per slot (both arms' axes and the first-person
/// right arm), `kNoneAnimation` for inactive arms.
[[nodiscard]] std::vector<SlotAnimation> vanillaAnimations(Pose const& pose);
/// Every slot `vanillaAnimations` uses (to end them all).
[[nodiscard]] std::vector<std::string> allControllers();

} // namespace lo::core::armpose
