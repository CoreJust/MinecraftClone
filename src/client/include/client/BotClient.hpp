#pragma once

#include "GameClient.hpp"

namespace client {

class BotClient final : public GameClient {
public:
    explicit BotClient(shared::WorldMode const mode = shared::WorldMode::Flat)
        : GameClient{ mode }
    { }
private:
    shared::Direction input() override;
    void render() override { }
private:
    shared::Direction m_direction{ 0, 0 };
};

} // namespace client
