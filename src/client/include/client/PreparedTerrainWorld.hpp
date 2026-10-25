#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace client {

enum class PreparedTerrainWorldState : uint8_t {
    Pending,
    Ready,
    Failed,
    Cancelled,
};

struct PreparedTerrainWorldStatus final {
    PreparedTerrainWorldState state = PreparedTerrainWorldState::Pending;
    std::shared_ptr<shared::TerrainWorld const> world;
    std::string error;
};

using PreparedTerrainWorldProvider = std::function<PreparedTerrainWorldStatus(
    shared::TerrainSessionIdentity const&
)>;

} // namespace client
