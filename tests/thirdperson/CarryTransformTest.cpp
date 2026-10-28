#include <gtest/gtest.h>

#include "client/thirdperson/CarryTransform.h"

using namespace lo::client::thirdperson;

namespace {
constexpr float kEps = 1e-3f;

void expectNear(std::array<float, 3> const& v, float x, float y, float z, float eps = kEps) {
    EXPECT_NEAR(v[0], x, eps);
    EXPECT_NEAR(v[1], y, eps);
    EXPECT_NEAR(v[2], z, eps);
}
} // namespace

TEST(CarryTransform, OpsComposeLikeAPoseStack) {
    Mat4 m;
    m.translate(1.f, 0.f, 0.f).rotateY(90.f).translate(0.f, 0.f, 1.f);
    // Rotating +Z by 90 degrees about +Y gives +X.
    expectNear(m.apply(0.f, 0.f, 0.f), 2.f, 0.f, 0.f);
    Mat4 r;
    r.rotateX(90.f);
    expectNear(r.applyDir(0.f, 1.f, 0.f), 0.f, 0.f, 1.f);
    Mat4 z;
    z.rotateZ(90.f);
    expectNear(z.applyDir(1.f, 0.f, 0.f), 0.f, 1.f, 0.f);
}

TEST(CarryTransform, BlockIsHeldAtChestHeightInFront) {
    CarrierPose p;
    Mat4        m = carrierFrame(p) * blockTransform(p, BlockParams{});
    // Centre: 0.6 * (0, -0.5, -0.7) in the rotated layer space, then the player model space.
    expectNear(m.apply(0.f, 0.f, 0.f), 0.f, 1.1259f, 0.39375f);
    // Size 0.6 * 0.9375, axes aligned with the carrier (yaw 0: the front, -Z, faces away).
    expectNear(m.applyDir(1.f, 0.f, 0.f), -0.5625f, 0.f, 0.f); // turned 180 with the carrier
    expectNear(m.applyDir(0.f, 1.f, 0.f), 0.f, 0.5625f, 0.f);
    expectNear(m.applyDir(0.f, 0.f, -1.f), 0.f, 0.f, 0.5625f);
}

TEST(CarryTransform, BlockFollowsBodyYawAndPosition) {
    CarrierPose p;
    p.feet    = {10.f, 64.f, -3.f};
    p.bodyYaw = 90.f; // facing -X
    Mat4 m    = carrierFrame(p) * blockTransform(p, BlockParams{});
    expectNear(m.apply(0.f, 0.f, 0.f), 10.f - 0.39375f, 64.f + 1.1259f, -3.f);
    expectNear(m.applyDir(0.f, 0.f, -1.f), -0.5625f, 0.f, 0.f);
}

TEST(CarryTransform, ChestAndFacePlayerTurnTheBlock) {
    CarrierPose p;
    BlockParams chest;
    chest.isChest = true;
    Mat4 m        = carrierFrame(p) * blockTransform(p, chest);
    expectNear(m.applyDir(0.f, 0.f, 1.f), 0.f, 0.f, 0.5625f); // the chest model's front (+Z) faces away
    BlockParams both;
    both.isChest    = true;
    both.facePlayer = true;
    expectNear((carrierFrame(p) * blockTransform(p, both)).applyDir(0.f, 0.f, -1.f), 0.f, 0.f, 0.5625f);
}

TEST(CarryTransform, SneakingLowersAndLowBlocksSit) {
    CarrierPose p;
    p.sneakCheck = true;
    p.crouching  = true;
    auto c       = (carrierFrame(p) * blockTransform(p, BlockParams{})).apply(0.f, 0.f, 0.f);
    // 0.4 * 0.6 * 0.9375 lower, plus the 2/16 crouch offset.
    EXPECT_NEAR(c[1], 1.1259f - 0.225f - 0.125f, kEps);
    CarrierPose stand;
    BlockParams slab;
    slab.renderHeight = 0.5f;
    auto s            = (carrierFrame(stand) * blockTransform(stand, slab)).apply(0.f, 0.f, 0.f);
    // translate y by -(0.5 - 1) / 1.2 (the -180 Z turn and the model flip cancel out): the half
    // block is lifted by 0.4167 * 0.5625.
    EXPECT_NEAR(s[1], 1.1259f + 0.41667f * 0.5625f, kEps);
}

TEST(CarryTransform, ScriptTranslationZFlipsInBackView) {
    CarrierPose p;
    BlockParams b;
    b.hasScript             = true;
    b.script.translation[2] = 1.f;
    auto front              = (carrierFrame(p) * blockTransform(p, b)).apply(0.f, 0.f, 0.f);
    b.backView              = true;
    auto back               = (carrierFrame(p) * blockTransform(p, b)).apply(0.f, 0.f, 0.f);
    EXPECT_NEAR(front[2] - back[2], -2.f * 0.5625f, kEps);
}

TEST(CarryTransform, EntityStandsUprightInFront) {
    CarrierPose  p;
    EntityParams pig{.height = 0.9f, .width = 0.9f};
    Mat4         m    = carrierFrame(p) * entityTransform(p, pig);
    auto         feet = m.apply(0.f, 0.f, 0.f);
    float        k    = (10.f - 0.81f) * 0.08f * 1.666f * 0.6f * 0.9375f; // world size of one entity block
    EXPECT_NEAR(feet[0], 0.f, kEps);
    EXPECT_GT(feet[1], 0.6f);
    EXPECT_LT(feet[1], 0.9f);
    EXPECT_GT(feet[2], 0.45f);
    EXPECT_LT(feet[2], 0.7f);
    expectNear(m.applyDir(0.f, 1.f, 0.f), 0.f, k, 0.f);  // upright
    expectNear(m.applyDir(0.f, 0.f, 1.f), 0.f, 0.f, k);  // faces where the carrier faces
    expectNear(m.applyDir(1.f, 0.f, 0.f), k, 0.f, 0.f);  // not mirrored
}

TEST(CarryTransform, BigEntitiesShrink) {
    CarrierPose  p;
    EntityParams small{.height = 0.5f, .width = 0.5f};
    EntityParams big{.height = 2.f, .width = 1.4f};
    float        a = (carrierFrame(p) * entityTransform(p, small)).applyDir(0.f, 1.f, 0.f)[1];
    float        b = (carrierFrame(p) * entityTransform(p, big)).applyDir(0.f, 1.f, 0.f)[1];
    EXPECT_GT(a, b);
    EXPECT_NEAR(b / a, (10.f - 2.8f) / (10.f - 0.25f), kEps);
}

TEST(CarryTransform, SidewaysRotatesTheEntity) {
    CarrierPose  p;
    EntityParams e{.height = 0.9f, .width = 0.9f, .sideways = true};
    auto         d = (carrierFrame(p) * entityTransform(p, e)).applyDir(0.f, 0.f, 1.f);
    EXPECT_NEAR(d[2], 0.f, kEps);
    EXPECT_GT(std::abs(d[0]), 0.1f);
    EXPECT_NEAR(entityRenderWidth(1.f, true), 0.65f, kEps);
    EXPECT_NEAR(entityRenderWidth(1.f, false), 0.9f, kEps);
}

TEST(CarryTransform, SwimmingLaysTheCarryFlat) {
    CarrierPose p;
    p.swimming = true;
    p.inWater  = true;
    p.pitch    = 0.f;
    Mat4 m     = carrierFrame(p) * blockTransform(p, BlockParams{});
    auto c     = m.apply(0.f, 0.f, 0.f);
    // The model lies along the swimming direction: the carry rides on the carrier's back, lower than\n    // when standing and above the carrier's position.
    EXPECT_NEAR(c[0], 0.f, kEps);
    EXPECT_NEAR(c[1], 0.665625f, kEps);
    EXPECT_NEAR(c[2], 0.041563f, kEps);
}
