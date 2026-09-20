#include <server/GameServer.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <string>
#include <thread>
#include <vector>

namespace {

TEST(GameServerLifecycle, ValidatesScenarioSpawnPointContracts)
{
    static constexpr uint8_t SPAWN_X{ 4 };
    static constexpr uint8_t SPAWN_Y{ 4 };
    static constexpr uint8_t DISTANT_SPAWN_X{ 6 };
    static constexpr uint8_t LAST_FOOTPRINT_ORIGIN{ 30 };
    static constexpr uint8_t OUT_OF_BOUNDS_ORIGIN{ 31 };
    std::expected<std::vector<server::GameServer::SpawnPoint>, std::string> const valid =
        server::GameServer::validateSpawnPoints({
            server::GameServer::SpawnPoint{
                .character = '@',
                .x = SPAWN_X,
                .y = SPAWN_Y,
            },
            server::GameServer::SpawnPoint{
                .character = '#',
                .x = DISTANT_SPAWN_X,
                .y = SPAWN_Y,
            },
        });
    std::expected<std::vector<server::GameServer::SpawnPoint>, std::string> const overlapping =
        server::GameServer::validateSpawnPoints({
            server::GameServer::SpawnPoint{
                .character = '@',
                .x = SPAWN_X,
                .y = SPAWN_Y,
            },
            server::GameServer::SpawnPoint{
                .character = '#',
                .x = SPAWN_X,
                .y = SPAWN_Y,
            },
        });
    std::expected<std::vector<server::GameServer::SpawnPoint>, std::string> const duplicate =
        server::GameServer::validateSpawnPoints({
            server::GameServer::SpawnPoint{
                .character = '@',
                .x = SPAWN_X,
                .y = SPAWN_Y,
            },
            server::GameServer::SpawnPoint{
                .character = '@',
                .x = DISTANT_SPAWN_X,
                .y = SPAWN_Y,
            },
        });
    std::expected<std::vector<server::GameServer::SpawnPoint>, std::string> const out_of_bounds_x =
        server::GameServer::validateSpawnPoints({
            server::GameServer::SpawnPoint{
                .character = '@',
                .x = OUT_OF_BOUNDS_ORIGIN,
                .y = SPAWN_Y,
            },
        });
    std::expected<std::vector<server::GameServer::SpawnPoint>, std::string> const out_of_bounds_y =
        server::GameServer::validateSpawnPoints({
            server::GameServer::SpawnPoint{
                .character = '@',
                .x = SPAWN_X,
                .y = OUT_OF_BOUNDS_ORIGIN,
            },
        });
    std::expected<std::vector<server::GameServer::SpawnPoint>, std::string> const invalid_character =
        server::GameServer::validateSpawnPoints({
            server::GameServer::SpawnPoint{
                .character = 'x',
                .x = SPAWN_X,
                .y = SPAWN_Y,
            },
        });
    std::expected<std::vector<server::GameServer::SpawnPoint>, std::string> const boundary_valid =
        server::GameServer::validateSpawnPoints({
            server::GameServer::SpawnPoint{
                .character = '@',
                .x = LAST_FOOTPRINT_ORIGIN,
                .y = LAST_FOOTPRINT_ORIGIN,
            },
        });

    EXPECT_TRUE(valid.has_value());
    EXPECT_FALSE(overlapping.has_value());
    EXPECT_FALSE(duplicate.has_value());
    EXPECT_FALSE(out_of_bounds_x.has_value());
    EXPECT_FALSE(out_of_bounds_y.has_value());
    EXPECT_FALSE(invalid_character.has_value());
    EXPECT_TRUE(boundary_valid.has_value());
}

TEST(GameServerLifecycle, RunStopsAfterTheCurrentProductionTick)
{
    static constexpr std::chrono::milliseconds MAXIMUM_STOP_TIME{ 200 };
    server::GameServer server{ 0 };
    std::atomic_bool stop_requested{ false };
    std::thread thread{ [&server, &stop_requested] {
        server.run(stop_requested);
    } };
    auto const start = std::chrono::steady_clock::now();

    stop_requested.store(true, std::memory_order_relaxed);
    thread.join();

    EXPECT_LT(std::chrono::steady_clock::now() - start, MAXIMUM_STOP_TIME);
}

TEST(GameServerLifecycle, FixedCadenceIsIndependentOfPresentationWork)
{
    EXPECT_EQ(server::GameServer::fixedTickDelay(std::chrono::milliseconds::zero()), shared::TICK);
    EXPECT_EQ(server::GameServer::fixedTickDelay(std::chrono::milliseconds{ 37 }), std::chrono::milliseconds{ 63 });
    EXPECT_EQ(server::GameServer::fixedTickDelay(shared::TICK), std::chrono::milliseconds::zero());
    EXPECT_EQ(server::GameServer::fixedTickDelay(std::chrono::milliseconds{ 250 }), std::chrono::milliseconds::zero());
}

TEST(GameServerLifecycle, TerrainWorkersReserveCapacityForAuthorityAndTransport)
{
    EXPECT_EQ(server::GameServer::terrainWorkerCount(0U), 1U);
    EXPECT_EQ(server::GameServer::terrainWorkerCount(1U), 1U);
    EXPECT_EQ(server::GameServer::terrainWorkerCount(2U), 1U);
    EXPECT_EQ(server::GameServer::terrainWorkerCount(3U), 1U);
    EXPECT_EQ(server::GameServer::terrainWorkerCount(4U), 2U);
    EXPECT_EQ(server::GameServer::terrainWorkerCount(10U), 8U);
    EXPECT_EQ(server::GameServer::terrainWorkerCount(64U), 8U);
}

} // namespace
