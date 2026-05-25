#include <client/BotClient.hpp>

#include <iostream>

namespace client {

shared::Direction BotClient::input() {
    if (rand() <= RAND_MAX / 50) {
        m_direction.x = (rand() % 3) - 1;
        m_direction.y = (rand() % 3) - 1;
        std::cout << "I will go into x " << static_cast<int>(m_direction.x) << " and y " << static_cast<int>(m_direction.y) << std::endl;
    }
    return m_direction;
}

} // namespace client
