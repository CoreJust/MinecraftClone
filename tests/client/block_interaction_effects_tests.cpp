#include <client/render/BlockInteractionEffects.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <limits>

using namespace client;
using namespace std::chrono_literals;

TEST(BlockInteractionEffectsTest, TargetEdgesAndCracksClearWithoutChangingWorld)
{
    BlockInteractionEffects effects;
    auto const now = BlockInteractionEffects::Clock::time_point{};
    shared::BlockTarget const target{.cell = {10, 20, 30}, .block = shared::Block::Stone};
    effects.setTarget(target, 0.0F);
    EXPECT_EQ(effects.boxes(now).size(), 12U);
    effects.setTarget(target, 0.5F);
    auto const cracking = effects.boxes(now);
    EXPECT_GT(cracking.size(), 12U);
    EXPECT_TRUE(std::all_of(cracking.begin(), cracking.end(), [](auto const& box) {
        return box.origin.x >= 9.98 && box.origin.x <= 11.02
            && box.origin.y >= 19.98 && box.origin.y <= 21.02
            && box.origin.z >= 29.98 && box.origin.z <= 31.02;
    }));
    effects.setTarget(std::nullopt, 1.0F);
    EXPECT_TRUE(effects.boxes(now).empty());
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
    effects.setTarget(shared::BlockTarget{.block = shared::Block::Air}, 1.0F);
    EXPECT_TRUE(effects.boxes(now).empty());
    EXPECT_FALSE(effects.acceptBreak({1U, 1U, 1U, 1U}, {}, shared::Block::Air, now));
    effects.setTarget(shared::BlockTarget{.block = shared::Block::Stone},
        std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(effects.boxes(now).size(), 12U);
}
