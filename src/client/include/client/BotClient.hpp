#pragma once

#include "GameClient.hpp"

namespace client {

class BotClient final : public GameClient {
private:
    shared::Direction input() override;
    void render() override { }
private:
    shared::Direction m_direction{ 0, 0 };
};

} // namespace client
