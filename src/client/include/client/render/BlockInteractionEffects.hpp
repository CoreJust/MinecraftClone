#pragma once

#include <shared/world/BlockInteraction.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <vector>

namespace client {

struct BlockBreakEventIdentity final {
    uint64_t join_epoch = 0U;
    uint64_t server_tick = 0U;
    uint32_t sequence = 0U;
    uint64_t cell_version = 0U;

    auto operator<=>(BlockBreakEventIdentity const&) const = default;
};

struct BlockEffectBox final {
    glm::dvec3 origin{};
    glm::vec3 extent{};
    glm::vec4 color{};
};

class BlockInteractionEffects final {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint32_t PARTICLES_PER_BREAK = 16U;
    static constexpr uint32_t MAXIMUM_PARTICLES = 256U;

    void setTarget(std::optional<shared::BlockTarget> target, float authoritative_progress) noexcept;
    // IDs increase within the active session. clear() starts a new session;
    // repeated/older accepted receipts cannot restart an effect, even after expiry.
    [[nodiscard]] bool acceptBreak(BlockBreakEventIdentity event_id, shared::WorldCoordinate cell,
        shared::Block material, Clock::time_point now);
    void clear() noexcept;
    [[nodiscard]] std::vector<BlockEffectBox> boxes(Clock::time_point now);

private:
    struct Particle final {
        glm::dvec3 origin{};
        glm::vec3 velocity{};
        glm::vec4 color{};
        Clock::time_point started_at{};
    };
    std::optional<shared::BlockTarget> m_target;
    float m_progress = 0.0F;
    std::optional<BlockBreakEventIdentity> m_last_event;
    std::vector<Particle> m_particles;
};

} // namespace client
