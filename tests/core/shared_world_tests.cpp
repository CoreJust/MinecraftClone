#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>

TEST(WorldTest, ExplicitSpawnAndLookup) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 3, 4 });
    auto const player = world.player(1);
    ASSERT_TRUE(player.has_value());
    EXPECT_EQ(player->x, 3u);
    EXPECT_EQ(player->y, 4u);
    EXPECT_TRUE(world.playerExists('@'));
    EXPECT_TRUE(world.playerByCharacter('@').has_value());
}

TEST(WorldTest, MovePlayerRejectsUnknownAndOccupiedPositions) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 5 });
    EXPECT_FALSE(world.movePlayer(99, { 1, 0 }));
    EXPECT_FALSE(world.movePlayer(1, { 1, 0 }));
    EXPECT_EQ(world.player(1)->x, 5u);
}

TEST(WorldTest, BorderCellsAreValidAndCollisionChecksDoNotUnderflow) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 0, 0 });
    EXPECT_TRUE(world.movePlayer(1, { 1, 0 }));
    EXPECT_EQ(world.player(1)->x, 1u);
    EXPECT_TRUE(world.movePlayer(1, { 255, 0 }));
    EXPECT_EQ(world.player(1)->x, 0u);
    EXPECT_FALSE(world.movePlayer(1, { 255, 0 }));
}

TEST(WorldTest, FullWidthAndHeightCellsAreValid) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ shared::World::WIDTH - 1, shared::World::HEIGHT - 1 });
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, shared::World::WIDTH - 1);
    EXPECT_EQ(world.player(1)->y, shared::World::HEIGHT - 1);
}

TEST(WorldTest, RandomSpawnsStayWithinTheWorld) {
    shared::World world;
    for (shared::PlayerId id = 0; id < 8; ++id) {
        world.spawnPlayer(id, static_cast<char>('@' + id));
        ASSERT_TRUE(world.player(id).has_value());
        EXPECT_LT(world.player(id)->x, shared::World::WIDTH);
        EXPECT_LT(world.player(id)->y, shared::World::HEIGHT);
        auto const spawned = world.player(id);
        for (auto const& other : world.players()) {
            if (other.id != id) {
                EXPECT_TRUE(
                    std::abs(static_cast<int32_t>(spawned->x) - other.x) > 1
                    || std::abs(static_cast<int32_t>(spawned->y) - other.y) > 1
                );
            }
        }
    }
}

TEST(WorldTest, CornerPlayersBlockAdjacentAndDiagonalMovement) {
    static constexpr std::array<std::pair<uint8_t, uint8_t>, 4> CORNERS{
        std::pair<uint8_t, uint8_t>{ 0, 0 },
        std::pair<uint8_t, uint8_t>{ 31, 0 },
        std::pair<uint8_t, uint8_t>{ 0, 31 },
        std::pair<uint8_t, uint8_t>{ 31, 31 },
    };
    for (auto const& [x, y] : CORNERS) {
        shared::World world;
        world.spawnPlayer(1, '@', {{ x, y }});
        auto const inside_x = static_cast<uint8_t>(x == 0 ? 2 : x - 2);
        auto const inside_y = static_cast<uint8_t>(y == 0 ? 2 : y - 2);
        auto const toward_x = static_cast<uint8_t>(x == 0 ? 255 : 1);
        auto const toward_y = static_cast<uint8_t>(y == 0 ? 255 : 1);
        world.spawnPlayer(2, '#', {{ inside_x, y }});
        EXPECT_FALSE(world.movePlayer(2, { toward_x, 0 }));
        world.despawnPlayer(2);
        world.spawnPlayer(2, '#', {{ x, inside_y }});
        EXPECT_FALSE(world.movePlayer(2, { 0, toward_y }));
        world.despawnPlayer(2);
        world.spawnPlayer(2, '#', {{ inside_x, inside_y }});
        EXPECT_FALSE(world.movePlayer(2, { toward_x, toward_y }));
    }
}

TEST(WorldTest, ZeroDirectionDoesNotMovePlayer) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    EXPECT_TRUE(world.movePlayer(1, { 0, 0 }));
    EXPECT_EQ(world.player(1)->x, 5u);
    EXPECT_EQ(world.player(1)->y, 5u);
}

TEST(WorldTest, DespawnRemovesOnlyRequestedPlayer) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 1, 1 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 10, 10 });
    world.despawnPlayer(1);
    EXPECT_FALSE(world.player(1).has_value());
    EXPECT_TRUE(world.player(2).has_value());
}
