#include <client/render/BlockInteractionEffects.hpp>

#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <limits>
#include <vector>

using namespace client;
using namespace std::chrono_literals;

TEST(BlockInteractionEffectsTest, TargetEdgesAndCracksClearWithoutChangingWorld)
{
    BlockInteractionEffects effects;
    auto const now = BlockInteractionEffects::Clock::time_point{};
    shared::BlockTarget const target{.cell = {10, 20, 30}, .block = shared::Block::Stone};
    effects.setTarget(target, 0.0F, now);
    EXPECT_EQ(effects.boxes(now).size(), 12U);
    effects.setTarget(target, 0.5F, now);
    auto const cracking = effects.boxes(now);
    EXPECT_GT(cracking.size(), 12U);
    EXPECT_TRUE(std::all_of(cracking.begin(), cracking.end(), [](auto const& box) {
        return box.kind == BlockEffectBox::Kind::Particle
            || (box.origin.x >= 9.98 && box.origin.x <= 11.02
                && box.origin.y >= 19.98 && box.origin.y <= 21.02
                && box.origin.z >= 29.98 && box.origin.z <= 31.02);
    }));
    auto const crack = std::find_if(cracking.begin(), cracking.end(), [](auto const& box) {
        return box.kind == BlockEffectBox::Kind::ProceduralCrack;
    });
    ASSERT_NE(crack, cracking.end());
    EXPECT_FLOAT_EQ(crack->color.a, -0.5F);
    EXPECT_GT(crack->crack_seed, 0U);

    effects.setTarget(std::nullopt, 1.0F, now);
    auto const after_target_loss = effects.boxes(now);
    EXPECT_TRUE(after_target_loss.empty());
}

TEST(BlockInteractionEffectsTest, BreakingParticlesFollowWorldGravityAndIgnoreFrameSamplingCadence)
{
    static constexpr int64_t SAMPLE_MILLISECONDS = 100;
    static constexpr double SAMPLE_SECONDS = 0.1;
    static constexpr uint32_t SAMPLE_COUNT = 3U;
    static constexpr uint32_t PARTICLE_INDEX = 0U;
    static constexpr std::array<int64_t, 5U> FRAME_SAMPLE_MILLISECONDS{17, 63, 101, 249, 401};
    BlockInteractionEffects effects;
    auto const now = BlockInteractionEffects::Clock::time_point{};
    shared::BlockTarget const target{.cell = {10, 20, 30}, .block = shared::Block::Stone};
    effects.setTarget(target, 0.1F, now);

    std::array<double, SAMPLE_COUNT> z_samples{};
    for (uint32_t sample = 0U; sample < SAMPLE_COUNT; ++sample) {
        auto const frame = effects.boxes(now + std::chrono::milliseconds{
            SAMPLE_MILLISECONDS * static_cast<int64_t>(sample)});
        std::vector<BlockEffectBox> particles;
        std::copy_if(frame.begin(), frame.end(), std::back_inserter(particles), [](auto const& box) {
            return box.kind == BlockEffectBox::Kind::Particle;
        });
        ASSERT_FALSE(particles.empty());
        z_samples[sample] = particles[PARTICLE_INDEX].origin.z;
    }
    double const tick_seconds = std::chrono::duration<double>(shared::TICK).count();
    double const gravity = static_cast<double>(shared::World::PLAYER_GRAVITY_SUBCELLS_PER_TICK)
        / static_cast<double>(shared::SUBCELLS_PER_CELL) / (tick_seconds * tick_seconds);
    EXPECT_NEAR(z_samples[2U] - 2.0 * z_samples[1U] + z_samples[0U],
        -gravity * SAMPLE_SECONDS * SAMPLE_SECONDS, 1.0e-9);

    BlockInteractionEffects direct;
    BlockInteractionEffects sampled;
    direct.setTarget(target, 0.4F, now);
    sampled.setTarget(target, 0.4F, now);
    for (int64_t const elapsed_ms : FRAME_SAMPLE_MILLISECONDS) {
        static_cast<void>(sampled.boxes(now + std::chrono::milliseconds{elapsed_ms}));
    }
    auto const direct_final = direct.boxes(now + 500ms);
    auto const sampled_final = sampled.boxes(now + 500ms);
    std::vector<BlockEffectBox> direct_particles;
    std::vector<BlockEffectBox> sampled_particles;
    std::copy_if(direct_final.begin(), direct_final.end(), std::back_inserter(direct_particles), [](auto const& box) {
        return box.kind == BlockEffectBox::Kind::Particle;
    });
    std::copy_if(sampled_final.begin(), sampled_final.end(), std::back_inserter(sampled_particles),
        [](auto const& box) { return box.kind == BlockEffectBox::Kind::Particle; });
    ASSERT_EQ(direct_particles.size(), sampled_particles.size());
    for (uint32_t particle = 0U; particle < static_cast<uint32_t>(direct_particles.size()); ++particle) {
        EXPECT_EQ(direct_particles[particle].origin, sampled_particles[particle].origin);
    }
}

TEST(BlockInteractionEffectsTest, ProceduralCrackInputsAreStablePerBlockAcrossProgress)
{
    static constexpr float INITIAL_PROGRESS = 0.35F;
    static constexpr float LATE_PROGRESS = 0.9F;
    BlockInteractionEffects first;
    BlockInteractionEffects repeated;
    auto const now = BlockInteractionEffects::Clock::time_point{};
    shared::BlockTarget const target{.cell = {10, 20, 30}, .block = shared::Block::Stone};
    first.setTarget(target, INITIAL_PROGRESS, now);
    repeated.setTarget(target, INITIAL_PROGRESS, now);
    auto const initial_boxes = first.boxes(now);
    auto const repeated_boxes = repeated.boxes(now);
    auto const find_crack = [](std::vector<BlockEffectBox> const& boxes) {
        return std::find_if(boxes.begin(), boxes.end(), [](auto const& box) {
            return box.kind == BlockEffectBox::Kind::ProceduralCrack;
        });
    };
    auto const initial_crack = find_crack(initial_boxes);
    auto const repeated_crack = find_crack(repeated_boxes);
    ASSERT_NE(initial_crack, initial_boxes.end());
    ASSERT_NE(repeated_crack, repeated_boxes.end());
    EXPECT_EQ(initial_crack->crack_seed, repeated_crack->crack_seed);

    first.setTarget(target, LATE_PROGRESS, now + BlockInteractionEffects::BREAKING_PARTICLE_INTERVAL);
    auto const late_boxes = first.boxes(now + BlockInteractionEffects::BREAKING_PARTICLE_INTERVAL);
    auto const late_crack = find_crack(late_boxes);
    ASSERT_NE(late_crack, late_boxes.end());
    EXPECT_EQ(initial_crack->crack_seed, late_crack->crack_seed);
    EXPECT_FLOAT_EQ(late_crack->color.a, -LATE_PROGRESS);
    EXPECT_LT(late_crack->origin.x, static_cast<double>(target.cell.x));
    EXPECT_GT(late_crack->extent.x, 1.0F);
}

TEST(BlockInteractionEffectsTest, CameraRelativeCrackGeometryPreservesOutsetAtHighCoordinatesAndWorldWrap)
{
    static constexpr double CRACK_SURFACE_OUTSET = 0.004;
    struct Case final {
        shared::WorldCoordinate cell;
        glm::dvec3 camera_position;
        glm::dvec3 relative_cell_origin;
    };
    std::array<Case, 2U> const CASES{{
        {{32'774, 32'740, 704}, {32'773.8, 32'739.7, 705.0}, {0.2, 0.3, -1.0}},
        {{65'535, 0, 2'047}, {0.1, 65'535.8, 2'048.0}, {-1.1, 0.2, -1.0}},
    }};
    BlockInteractionEffects::Clock::time_point const now{};

    for (Case const& test_case : CASES) {
        BlockInteractionEffects effects;
        effects.setTarget(shared::BlockTarget{.cell = test_case.cell, .block = shared::Block::Stone}, 0.5F, now);
        auto const boxes = effects.boxes(now);
        auto const crack = std::find_if(boxes.begin(), boxes.end(), [](BlockEffectBox const& box) {
            return box.kind == BlockEffectBox::Kind::ProceduralCrack;
        });
        ASSERT_NE(crack, boxes.end());

        glm::dvec3 const relative_origin = detail::cameraRelativeBlockEffectOrigin(
            crack->origin, test_case.camera_position);
        for (size_t axis = 0U; axis < 3U; ++axis) {
            double const expected_lower = test_case.relative_cell_origin[axis] - CRACK_SURFACE_OUTSET;
            EXPECT_NEAR(relative_origin[axis], expected_lower, 1.0e-9);
            float const rounded_lower = static_cast<float>(relative_origin[axis]);
            float const rounded_upper = rounded_lower + crack->extent[axis];
            float const rounded_cell_lower = static_cast<float>(test_case.relative_cell_origin[axis]);
            EXPECT_LT(rounded_lower, rounded_cell_lower);
            EXPECT_GT(rounded_upper, rounded_cell_lower + 1.0F);
        }
        EXPECT_GT(crack->color.r, 0.1F);
        EXPECT_GT(crack->color.g, 0.1F);
        EXPECT_GT(crack->color.b, 0.1F);
    }
}

TEST(BlockInteractionEffectsTest, BreakingEmitterIsProgressGatedRateLimitedAndStopsOnTargetLoss)
{
    static constexpr int64_t EMISSION_INTERVAL_MILLISECONDS = 120;
    BlockInteractionEffects effects;
    auto const now = BlockInteractionEffects::Clock::time_point{};
    shared::BlockTarget const target{.cell = {10, 20, 30}, .block = shared::Block::Stone};
    auto particle_count = [&](BlockInteractionEffects::Clock::time_point const time) {
        auto const boxes = effects.boxes(time);
        return static_cast<uint32_t>(std::count_if(boxes.begin(), boxes.end(), [](auto const& box) {
            return box.kind == BlockEffectBox::Kind::Particle;
        }));
    };

    effects.setTarget(target, 0.0F, now);
    EXPECT_EQ(particle_count(now), 0U);
    effects.setTarget(target, 0.2F, now);
    EXPECT_EQ(particle_count(now), BlockInteractionEffects::PARTICLES_PER_BREAKING_BURST);
    effects.setTarget(target, 0.4F, now + std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS - 1});
    EXPECT_EQ(particle_count(now + std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS - 1}),
        BlockInteractionEffects::PARTICLES_PER_BREAKING_BURST);
    effects.setTarget(target, 0.6F, now + std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS});
    EXPECT_EQ(particle_count(now + std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS}),
        2 * BlockInteractionEffects::PARTICLES_PER_BREAKING_BURST);
    effects.setTarget(target, 0.0F, now + 2 * std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS});
    EXPECT_EQ(particle_count(now + 2 * std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS}), 0U);
    effects.setTarget(target, 0.8F,
        now + 2 * std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS} + 1ms);
    EXPECT_EQ(particle_count(now + 2 * std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS} + 1ms),
        BlockInteractionEffects::PARTICLES_PER_BREAKING_BURST);
    shared::BlockTarget const next_target{.cell = {11, 20, 30}, .block = shared::Block::Stone};
    effects.setTarget(next_target, 0.5F,
        now + 2 * std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS} + 2ms);
    EXPECT_EQ(particle_count(now + 2 * std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS} + 2ms),
        BlockInteractionEffects::PARTICLES_PER_BREAKING_BURST);
    effects.setTarget(std::nullopt, 1.0F,
        now + 2 * std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS} + 3ms);
    EXPECT_EQ(particle_count(now + 2 * std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS} + 3ms), 0U);
    effects.setTarget(std::nullopt, 1.0F,
        now + 3 * std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS});
    EXPECT_EQ(particle_count(now + 3 * std::chrono::milliseconds{EMISSION_INTERVAL_MILLISECONDS}), 0U);
    effects.setTarget(target, 0.2F, now + 500ms);
    EXPECT_EQ(particle_count(now + 500ms), BlockInteractionEffects::PARTICLES_PER_BREAKING_BURST);
    effects.clear();
    EXPECT_EQ(particle_count(now + 500ms), 0U);
}

TEST(BlockInteractionEffectsTest, HeldParticlesSpawnAndTravelOutwardAtEveryEnteredFace)
{
    struct FaceCase final {
        shared::BlockFace face;
        int axis;
        double direction;
    };
    static constexpr std::array<FaceCase, 6U> FACES{{
        {shared::BlockFace::NegativeX, 0, -1.0},
        {shared::BlockFace::PositiveX, 0, 1.0},
        {shared::BlockFace::NegativeY, 1, -1.0},
        {shared::BlockFace::PositiveY, 1, 1.0},
        {shared::BlockFace::NegativeZ, 2, -1.0},
        {shared::BlockFace::PositiveZ, 2, 1.0},
    }};
    static constexpr std::chrono::milliseconds SAMPLE_AGE{20};
    BlockInteractionEffects::Clock::time_point const now{};
    shared::WorldCoordinate const cell{10, 20, 30};
    std::array<double, 3U> const cell_origin{
        static_cast<double>(cell.x), static_cast<double>(cell.y), static_cast<double>(cell.z),
    };

    for (FaceCase const face : FACES) {
        BlockInteractionEffects effects;
        effects.setTarget(shared::BlockTarget{
            .cell = cell, .block = shared::Block::Stone, .entered_face = face.face,
        }, 0.5F, now);
        auto const initial_boxes = effects.boxes(now);
        auto const later_boxes = effects.boxes(now + SAMPLE_AGE);
        std::vector<BlockEffectBox> initial_particles;
        std::vector<BlockEffectBox> later_particles;
        auto const is_particle = [](BlockEffectBox const& box) {
            return box.kind == BlockEffectBox::Kind::Particle;
        };
        std::copy_if(initial_boxes.begin(), initial_boxes.end(), std::back_inserter(initial_particles), is_particle);
        std::copy_if(later_boxes.begin(), later_boxes.end(), std::back_inserter(later_particles), is_particle);
        ASSERT_EQ(initial_particles.size(), BlockInteractionEffects::PARTICLES_PER_BREAKING_BURST);
        ASSERT_EQ(later_particles.size(), initial_particles.size());

        double const face_plane = cell_origin[static_cast<size_t>(face.axis)]
            + (face.direction > 0.0 ? 1.0 : 0.0);
        for (size_t particle = 0U; particle < initial_particles.size(); ++particle) {
            double const initial = initial_particles[particle].origin[face.axis];
            if (face.direction < 0.0) {
                EXPECT_LT(initial + static_cast<double>(initial_particles[particle].extent[face.axis]), face_plane);
            } else {
                EXPECT_GT(initial, face_plane);
            }
            double const displacement = later_particles[particle].origin[face.axis] - initial;
            EXPECT_GT(displacement * face.direction, 0.0);
        }
    }
}

TEST(BlockInteractionEffectsTest, MissingEnteredFaceUsesDeterministicVolumeFallback)
{
    BlockInteractionEffects first;
    BlockInteractionEffects second;
    BlockInteractionEffects::Clock::time_point const now{};
    shared::WorldCoordinate const cell{10, 20, 30};
    shared::BlockTarget const target{.cell = cell, .block = shared::Block::Stone};
    first.setTarget(target, 0.5F, now);
    second.setTarget(target, 0.5F, now);
    auto const first_boxes = first.boxes(now);
    auto const second_boxes = second.boxes(now);
    std::vector<BlockEffectBox> first_particles;
    std::vector<BlockEffectBox> second_particles;
    auto const is_particle = [](BlockEffectBox const& box) {
        return box.kind == BlockEffectBox::Kind::Particle;
    };
    std::copy_if(first_boxes.begin(), first_boxes.end(), std::back_inserter(first_particles), is_particle);
    std::copy_if(second_boxes.begin(), second_boxes.end(), std::back_inserter(second_particles), is_particle);

    ASSERT_EQ(first_particles.size(), BlockInteractionEffects::PARTICLES_PER_BREAKING_BURST);
    ASSERT_EQ(first_particles.size(), second_particles.size());
    for (size_t index = 0U; index < first_particles.size(); ++index) {
        EXPECT_EQ(first_particles[index].origin, second_particles[index].origin);
        EXPECT_GE(first_particles[index].origin.x, static_cast<double>(cell.x));
        EXPECT_LT(first_particles[index].origin.x, static_cast<double>(cell.x + 1));
        EXPECT_GE(first_particles[index].origin.y, static_cast<double>(cell.y));
        EXPECT_LT(first_particles[index].origin.y, static_cast<double>(cell.y + 1));
        EXPECT_GE(first_particles[index].origin.z, static_cast<double>(cell.z));
        EXPECT_LT(first_particles[index].origin.z, static_cast<double>(cell.z + 1));
    }
}

TEST(BlockInteractionEffectsTest, TargetLossStopsHoldParticlesAndKeepsAcceptedCompletionParticles)
{
    BlockInteractionEffects effects;
    auto const now = BlockInteractionEffects::Clock::time_point{};
    shared::BlockTarget const target{.cell = {10, 20, 30}, .block = shared::Block::Stone};
    auto particle_count = [&](BlockInteractionEffects::Clock::time_point const time) {
        auto const boxes = effects.boxes(time);
        return static_cast<uint32_t>(std::count_if(boxes.begin(), boxes.end(), [](auto const& box) {
            return box.kind == BlockEffectBox::Kind::Particle;
        }));
    };

    ASSERT_TRUE(effects.acceptBreak({1U, 1U, 1U, 1U}, target.cell, target.block, now));
    effects.setTarget(target, 0.5F, now + 10ms);
    EXPECT_EQ(particle_count(now + 10ms),
        BlockInteractionEffects::PARTICLES_PER_BREAK + BlockInteractionEffects::PARTICLES_PER_BREAKING_BURST);

    effects.setTarget(std::nullopt, 0.0F, now + 20ms);
    EXPECT_EQ(particle_count(now + 20ms), BlockInteractionEffects::PARTICLES_PER_BREAK);
}

TEST(BlockInteractionEffectsTest, AcceptedParticlesAreDeterministicBoundedAndNeverRestartFromOldReceipts)
{
    BlockInteractionEffects first;
    BlockInteractionEffects second;
    auto const now = BlockInteractionEffects::Clock::time_point{};
    EXPECT_TRUE(first.acceptBreak({1U, 1U, 1U, 1U}, {1, 2, 3}, shared::Block::Dirt, now));
    EXPECT_TRUE(second.acceptBreak({1U, 1U, 1U, 1U}, {1, 2, 3}, shared::Block::Dirt, now));
    auto const a = first.boxes(now + 100ms);
    auto const b = second.boxes(now + 100ms);
    ASSERT_EQ(a.size(), BlockInteractionEffects::PARTICLES_PER_BREAK);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i].origin, b[i].origin);
    EXPECT_FALSE(first.acceptBreak({1U, 1U, 1U, 1U}, {1, 2, 3}, shared::Block::Dirt, now + 100ms));
    EXPECT_TRUE(first.boxes(now + 1s).empty());
    EXPECT_FALSE(first.acceptBreak({1U, 1U, 1U, 1U}, {1, 2, 3}, shared::Block::Dirt, now + 2s));
    for (uint64_t id = 2U; id < 40U; ++id)
        EXPECT_TRUE(first.acceptBreak({1U, id, 1U, 1U}, {1, 2, 3}, shared::Block::Stone, now + 2s));
    EXPECT_EQ(first.boxes(now + 2s).size(), BlockInteractionEffects::MAXIMUM_PARTICLES);
    first.clear();
    EXPECT_TRUE(first.acceptBreak({1U, 1U, 1U, 1U}, {1, 2, 3}, shared::Block::Snow, now + 3s));
}

TEST(BlockInteractionEffectsTest, InvalidTargetsAndMaterialsDoNotCreateEffects)
{
    BlockInteractionEffects effects;
    auto const now = BlockInteractionEffects::Clock::time_point{};
    effects.setTarget(shared::BlockTarget{.block = shared::Block::Air}, 1.0F, now);
    EXPECT_TRUE(effects.boxes(now).empty());
    EXPECT_FALSE(effects.acceptBreak({1U, 1U, 1U, 1U}, {}, shared::Block::Air, now));
    effects.setTarget(shared::BlockTarget{.block = shared::Block::Stone},
        std::numeric_limits<float>::quiet_NaN(), now);
    EXPECT_EQ(effects.boxes(now).size(), 12U);
}

TEST(BlockInteractionEffectsTest, SandBreakParticlesUseWarmMaterialColor)
{
    BlockInteractionEffects effects;
    auto const now = BlockInteractionEffects::Clock::time_point{};

    ASSERT_TRUE(effects.acceptBreak({1U, 1U, 1U, 1U}, {}, shared::Block::Sand, now));
    auto const particles = effects.boxes(now + 100ms);

    ASSERT_EQ(particles.size(), BlockInteractionEffects::PARTICLES_PER_BREAK);
    for (BlockEffectBox const& particle : particles) {
        EXPECT_FLOAT_EQ(particle.color.r, 0.88F);
        EXPECT_FLOAT_EQ(particle.color.g, 0.78F);
        EXPECT_FLOAT_EQ(particle.color.b, 0.57F);
        EXPECT_FLOAT_EQ(particle.color.a, 1.0F);
    }
}
