#include <gtest/gtest.h>

#include "core/ArmPose.h"

using namespace lo::core::armpose;

TEST(ArmPose, RenderWidth) {
    EXPECT_FLOAT_EQ(blockRenderWidth(0.875f, false), 0.875f);
    EXPECT_FLOAT_EQ(blockRenderWidth(1.0f, true), 0.8f);
    EXPECT_FLOAT_EQ(entityRenderWidth(0.9f, false), 0.81f);
    EXPECT_FLOAT_EQ(entityRenderWidth(1.0f, true), 0.65f);
}

TEST(ArmPose, Offset) {
    EXPECT_FLOAT_EQ(armOffset(1.0f), 0.0f);
    EXPECT_FLOAT_EQ(armOffset(0.875f), -0.125f / 1.5f);
    EXPECT_FLOAT_EQ(armOffset(2.0f), 0.2f); // capped
    EXPECT_FLOAT_EQ(armOffset(0.0f), -1.0f / 1.5f);
}

TEST(ArmPose, Block) {
    auto pose = computePose(false, 1.0f);
    ASSERT_TRUE(pose.right.active && pose.left.active);
    EXPECT_FLOAT_EQ(pose.right.rotation[0], -1.0f);
    EXPECT_FLOAT_EQ(pose.right.rotation[1], 0.0f);
    EXPECT_FLOAT_EQ(pose.right.rotation[2], -0.05f);
    EXPECT_FLOAT_EQ(pose.left.rotation[0], -1.0f);
    EXPECT_FLOAT_EQ(pose.left.rotation[2], 0.05f);
    EXPECT_TRUE(pose.right.crouchX);
    auto crouched = javaRotation(pose.right, true);
    EXPECT_FLOAT_EQ(crouched[0], -1.2f);
    EXPECT_FLOAT_EQ(javaRotation(pose.right, false)[0], -1.0f);
}

TEST(ArmPose, EntityAndOffsetSigns) {
    float w    = entityRenderWidth(0.9f, false); // pig
    auto  pose = computePose(true, w);
    float off  = (w - 1.0f) / 1.5f;
    EXPECT_FLOAT_EQ(pose.right.rotation[0], -1.3f);
    EXPECT_FLOAT_EQ(pose.right.rotation[1], off);
    EXPECT_FLOAT_EQ(pose.left.rotation[1], -off);
    EXPECT_FLOAT_EQ(javaRotation(pose.left, true)[0], -1.5f);
}

TEST(ArmPose, ScriptOverrides) {
    ArmOverride right;
    right.rotation[0] = -0.5f;
    ArmOverride left;
    left.render = false;
    auto pose   = computePose(false, 1.0f, right, left);
    EXPECT_FLOAT_EQ(pose.right.rotation[0], -0.5f);
    EXPECT_FALSE(pose.right.crouchX); // a script x replaces the crouch term too
    EXPECT_FLOAT_EQ(javaRotation(pose.right, true)[0], -0.5f);
    EXPECT_FLOAT_EQ(pose.right.rotation[2], -0.05f);
    EXPECT_FALSE(pose.left.active);
    EXPECT_FALSE(computePose(false, 1.0f, {}, {}, false).right.active); // renderArms false
}

TEST(ArmPose, Quantize) {
    EXPECT_EQ(quantize(-1.0f), -20);
    EXPECT_EQ(quantize(-1.3f), -26);
    EXPECT_EQ(quantize(0.024f), 0);
    EXPECT_EQ(quantize(0.026f), 1);
    EXPECT_EQ(quantize(4.0f), quantize(4.0f - 2.0f * 3.14159265f)); // wrapped, not clamped
    EXPECT_EQ(quantize(-1.0f - 2.0f * 3.14159265f), -20);
    EXPECT_LE(quantize(3.15f), kMaxIndex);
    EXPECT_GE(quantize(-3.15f), -kMaxIndex);
    EXPECT_FLOAT_EQ(dequantize(-20), -1.0f);
    EXPECT_NEAR(toDegrees(-1.0f), -57.2958f, 1e-3f);
}

TEST(ArmPose, Names) {
    EXPECT_EQ(animationName(Side::Right, Axis::X, -20, true), "animation.liftoff.carry.right.xc.m20");
    EXPECT_EQ(animationName(Side::Left, Axis::Y, 3), "animation.liftoff.carry.left.y.p3");
    EXPECT_EQ(animationName(Side::Left, Axis::Z, 0, true), "animation.liftoff.carry.left.z.0"); // xc only for x
    EXPECT_EQ(controllerName(Side::Right, Axis::Z), "liftoff.carry.right.z");
}

TEST(ArmPose, VanillaAnimations) {
    auto anims = vanillaAnimations(computePose(false, 1.0f));
    ASSERT_EQ(anims.size(), 7u);
    EXPECT_EQ(anims[0], (SlotAnimation{"liftoff.carry.right.x", "animation.liftoff.carry.right.xc.m20"}));
    EXPECT_EQ(anims[1], (SlotAnimation{"liftoff.carry.right.y", "animation.liftoff.carry.right.y.0"}));
    EXPECT_EQ(anims[2], (SlotAnimation{"liftoff.carry.right.z", "animation.liftoff.carry.right.z.m1"}));
    EXPECT_EQ(anims[5], (SlotAnimation{"liftoff.carry.left.z", "animation.liftoff.carry.left.z.p1"}));
    EXPECT_EQ(anims[6], (SlotAnimation{kFirstPersonController, kFirstPersonAnimation}));

    auto none = vanillaAnimations(Pose{});
    for (auto const& a : none) EXPECT_EQ(a.animation, kNoneAnimation);
    EXPECT_EQ(allControllers().size(), 7u);
}
