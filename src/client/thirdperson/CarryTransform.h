#pragma once

// Third-person transform of a carried object (docs/SPEC.md §4.3). Pure math, no game headers: the
// renderer (ThirdPerson.cpp, client build) feeds it the carrier's pose and draws with the result, the
// unit tests (tests/thirdperson) check it.
//
// Carry On draws the carried object as a render layer of the player model, so its transforms are in the
// player's *model space*: rotated by the body yaw, flipped by scale(-1, -1, 1), scaled by 0.9375 and
// lowered by 1.501 (the vanilla player renderer). `carrierFrame` rebuilds that space in world
// coordinates; `blockTransform` / `entityTransform` are Carry On's chains in it. Ops are post-multiplied
// (each later op applies in the local frame of the earlier ones), like a Java PoseStack.
//
//   Mat4 m = carrierFrame(pose) * blockTransform(pose, block);   // then draw the item centred at m
//
// Lengths in blocks, angles in degrees, right-handed, +Y up, yaw 0 = facing +Z (Java and Bedrock agree).

#include <algorithm>
#include <array>
#include <cmath>

namespace lo::client::thirdperson {

/// 4x4 matrix, column-major (`m[col][row]`, like glm), acting on column vectors.
struct Mat4 {
    std::array<std::array<float, 4>, 4> m{
        {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}, {0.f, 0.f, 0.f, 1.f}}
    };

    [[nodiscard]] static Mat4 identity() { return {}; }

    [[nodiscard]] friend Mat4 operator*(Mat4 const& a, Mat4 const& b) {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                float s = 0.f;
                for (int k = 0; k < 4; ++k) s += a.m[k][row] * b.m[c][k];
                r.m[c][row] = s;
            }
        }
        return r;
    }

    /// Transforms a point.
    [[nodiscard]] std::array<float, 3> apply(float x, float y, float z) const {
        return {
            m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0],
            m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1],
            m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2],
        };
    }
    /// Transforms a direction (no translation).
    [[nodiscard]] std::array<float, 3> applyDir(float x, float y, float z) const {
        return {
            m[0][0] * x + m[1][0] * y + m[2][0] * z,
            m[0][1] * x + m[1][1] * y + m[2][1] * z,
            m[0][2] * x + m[1][2] * y + m[2][2] * z,
        };
    }

    // PoseStack-style ops (post-multiplied).
    Mat4& translate(float x, float y, float z) {
        Mat4 t;
        t.m[3] = {x, y, z, 1.f};
        return *this = *this * t;
    }
    Mat4& scale(float x, float y, float z) {
        Mat4 s;
        s.m[0][0] = x;
        s.m[1][1] = y;
        s.m[2][2] = z;
        return *this = *this * s;
    }
    Mat4& scale(float k) { return scale(k, k, k); }
    Mat4& rotateX(float deg) {
        float r = deg * kRad, c = std::cos(r), s = std::sin(r);
        Mat4  t;
        t.m[1][1] = c;
        t.m[1][2] = s;
        t.m[2][1] = -s;
        t.m[2][2] = c;
        return *this = *this * t;
    }
    Mat4& rotateY(float deg) {
        float r = deg * kRad, c = std::cos(r), s = std::sin(r);
        Mat4  t;
        t.m[0][0] = c;
        t.m[0][2] = -s;
        t.m[2][0] = s;
        t.m[2][2] = c;
        return *this = *this * t;
    }
    Mat4& rotateZ(float deg) {
        float r = deg * kRad, c = std::cos(r), s = std::sin(r);
        Mat4  t;
        t.m[0][0] = c;
        t.m[0][1] = s;
        t.m[1][0] = -s;
        t.m[1][1] = c;
        return *this = *this * t;
    }

    static constexpr float kRad = 3.14159265358979f / 180.f;
};

/// What the transform needs to know about the carrier (one frame).
struct CarrierPose {
    std::array<float, 3> feet{0.f, 0.f, 0.f}; ///< interpolated feet position (render space)
    float                bodyYaw = 0.f;       ///< interpolated body yaw
    float                pitch   = 0.f;       ///< view pitch (swimming / gliding tilt)
    float                scale   = 1.f;       ///< carrier scale
    bool                 crouching = false;   ///< crouch pose: the model is lowered by 2/16
    bool                 sneakCheck = false;  ///< Carry On `doSneakCheck`: sneaking and not flying
    bool                 swimming   = false;  ///< swimming / crawling pose
    bool                 gliding    = false;  ///< elytra
    bool                 inWater    = false;  ///< swimming tilt follows the pitch only in water
};

/// The script `render` transform (`CarryVisual`): rotation in degrees.
struct ScriptTransform {
    std::array<float, 3> translation{0.f, 0.f, 0.f};
    std::array<float, 3> rotation{0.f, 0.f, 0.f};
    std::array<float, 3> scale{1.f, 1.f, 1.f};
};

/// Vanilla player renderer: world -> player model space (the space of a render layer).
[[nodiscard]] inline Mat4 carrierFrame(CarrierPose const& p) {
    Mat4 m;
    m.translate(p.feet[0], p.feet[1], p.feet[2]);
    if (p.crouching) m.translate(0.f, -2.f / 16.f * p.scale, 0.f);
    m.scale(p.scale);
    m.rotateY(180.f - p.bodyYaw);
    if (p.gliding) {
        m.rotateX(-90.f - p.pitch);
    } else if (p.swimming) {
        m.rotateX(p.inWater ? -90.f - p.pitch : -90.f);
        m.translate(0.f, -1.f, 0.3f);
    }
    m.scale(-1.f, -1.f, 1.f);
    m.scale(0.9375f);
    m.translate(0.f, -1.501f, 0.f);
    return m;
}

/// Carry On `applyGeneralTransformations` (in model space).
inline void applyGeneral(Mat4& m, CarrierPose const& p) {
    m.scale(0.6f);
    m.translate(0.f, 0.f, -1.35f);
    if (p.sneakCheck) m.translate(0.f, -0.4f, 0.f);
    if (p.swimming || p.gliding) {
        m.translate(0.f, 0.f, 2.5f);
        m.rotateX(90.f);
    }
    m.translate(0.f, -0.5f, 0.65f);
}

/// Script transform: rotate X, Y, Z; translate (z negated when `negateZ`); scale.
inline void applyScript(Mat4& m, ScriptTransform const& s, bool negateZ) {
    m.rotateX(s.rotation[0]);
    m.rotateY(s.rotation[1]);
    m.rotateZ(s.rotation[2]);
    m.translate(s.translation[0], s.translation[1], negateZ ? -s.translation[2] : s.translation[2]);
    m.scale(s.scale[0], s.scale[1], s.scale[2]);
}

struct BlockParams {
    bool            isChest      = false; ///< the carried block is a chest (SPEC §4.1)
    bool            facePlayer   = false; ///< client setting
    float           renderHeight = 1.f;   ///< height of the drawn block's shape (0.5 with an item override)
    bool            hasScript    = false;
    ScriptTransform script;
    bool            backView = false; ///< third-person back view (Carry On perspective 1)
};

/// Carry On block chain in model space. The result maps Java's item model space, centred on the
/// origin (the unit block spans -0.5..0.5, front at -Z), to model space.
[[nodiscard]] inline Mat4 blockTransform(CarrierPose const& p, BlockParams const& b) {
    Mat4 m;
    m.rotateZ(-180.f);
    applyGeneral(m, p);
    if (b.facePlayer != b.isChest) m.rotateY(180.f);
    m.translate(0.f, -(b.renderHeight - 1.f) / 1.2f, 0.f);
    if (b.hasScript) applyScript(m, b.script, b.backView);
    return m;
}

struct EntityParams {
    float           height   = 1.f; ///< collision box
    float           width    = 1.f;
    bool            sideways = false; ///< rotateEntitiesSideways
    bool            hasScript = false;
    ScriptTransform script;
};

/// Carry On entity chain in model space. The result is where the entity is drawn: its feet at the origin,
/// facing +Z, all rotations zero.
[[nodiscard]] inline Mat4 entityTransform(CarrierPose const& p, EntityParams const& e) {
    Mat4 m;
    applyGeneral(m, p);
    m.rotateX(180.f);
    m.translate(0.f, -3.1f, -0.65f);
    m.scale(1.666f);
    float k = (10.f - std::min(9.9f, e.height * e.width)) * 0.08f;
    m.scale(k);
    m.translate(0.f, e.height / 2.f - e.height / 4.f + 1.f, std::max(e.width - 0.1f, 0.7f));
    if (p.sneakCheck) m.translate(0.f, -0.4f, 0.f);
    if (p.swimming || p.gliding) {
        m.rotateX(-180.f);
        m.translate(0.f, 0.2f * e.height - 2.f, -0.5f);
    }
    if (e.sideways) m.rotateY(90.f);
    if (e.hasScript) applyScript(m, e.script, false);
    return m;
}

/// `renderWidth` of an entity (SPEC §4.1).
[[nodiscard]] inline float entityRenderWidth(float width, bool sideways) {
    return sideways ? width - width * width * 0.35f : width * 0.9f;
}

} // namespace lo::client::thirdperson
