#include <server/GameServer.hpp>

#include <gtest/gtest.h>

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
    static constexpr uint8_t WORLD_EDGE_X{ 31 };
    static constexpr uint8_t OUT_OF_BOUNDS_X{ 32 };
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
    std::expected<std::vector<server::GameServer::SpawnPoint>, std::string> const out_of_bounds =
        server::GameServer::validateSpawnPoints({
            server::GameServer::SpawnPoint{
                .character = '@',
                .x = OUT_OF_BOUNDS_X,
                .y = SPAWN_Y,
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
                .x = WORLD_EDGE_X,
                .y = SPAWN_Y,
            },
        });

    EXPECT_TRUE(valid.has_value());
    EXPECT_FALSE(overlapping.has_value());
    EXPECT_FALSE(duplicate.has_value());
    EXPECT_FALSE(out_of_bounds.has_value());
    EXPECT_FALSE(invalid_character.has_value());
    EXPECT_TRUE(boundary_valid.has_value());
}

TEST(GameServerLifecycle, RunStopsAfterTheCurrentProductionTick)
{
    static constexpr std::chrono::milliseconds MAXIMUM_STOP_TIME{ 200 };
    server::GameServer server{ 0 };
    std::jthread thread{ [&server](std::stop_token const stop_token) {
        server.run(stop_token);
    } };
    auto const start = std::chrono::steady_clock::now();

    thread.request_stop();
    thread.join();

    EXPECT_LT(std::chrono::steady_clock::now() - start, MAXIMUM_STOP_TIME);
}

} // namespace
