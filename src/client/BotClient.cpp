#include <client/BotClient.hpp>

#include <client/WorldLoading.hpp>

#include <shared/world/World.hpp>

#include <chrono>
#include <cstdint>
#include <random>

namespace client {

void BotClient::render()
{
    if (worldLoadingStatus().state != WorldLoadingState::ReadyForStart) {
        return;
    }

    LoadingTimePoint const logical_start_action_at = std::chrono::steady_clock::now();
    // The headless bot uses the normal ready-prompt gate without claiming a GPU prompt frame.
    markLoadingPromptRendered(logical_start_action_at);
    static_cast<void>(acceptLoadingStartInput(logical_start_action_at));
}

shared::Direction BotClient::input() {
    static std::default_random_engine generator{ std::random_device{}() };
    std::uniform_int_distribution<int> change_distribution{ 0, 49 };
    std::uniform_int_distribution<int> direction_distribution{ -1, 1 };
    if (change_distribution(generator) == 0) {
        m_direction.x = static_cast<uint8_t>(direction_distribution(generator) * 127);
        m_direction.y = static_cast<uint8_t>(direction_distribution(generator) * 127);
        m_direction.z = static_cast<uint8_t>(direction_distribution(generator) * 127);
    }
    return m_direction;
}

} // namespace client
