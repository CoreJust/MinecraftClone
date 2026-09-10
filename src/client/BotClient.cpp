#include <client/BotClient.hpp>

#include <random>

namespace client {

shared::Direction BotClient::input() {
    static std::default_random_engine generator{ std::random_device{}() };
    std::uniform_int_distribution<int> change_distribution{ 0, 49 };
    std::uniform_int_distribution<int> direction_distribution{ -1, 1 };
    if (change_distribution(generator) == 0) {
        m_direction.x = static_cast<uint8_t>(direction_distribution(generator));
        m_direction.y = static_cast<uint8_t>(direction_distribution(generator));
    }
    return m_direction;
}

} // namespace client
