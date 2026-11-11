#pragma once

#include <shared/world/BlockInteraction.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <vector>

namespace client::detail {

// Keeps block-effect coordinates precise when they are rebased for curved-world rendering.
[[nodiscard]] glm::dvec3 cameraRelativeBlockEffectOrigin(
    glm::dvec3 origin, glm::dvec3 camera_position) noexcept;

} // namespace client::detail

namespace client {

struct BlockBreakEventIdentity final {
    uint64_t join_epoch = 0U;
    uint64_t server_tick = 0U;
    uint32_t sequence = 0U;
    uint64_t cell_version = 0U;

    auto operator<=>(BlockBreakEventIdentity const&) const = default;
};

struct BlockEffectBox final {
    enum class Kind : uint8_t {
        TargetOutline,
        ProceduralCrack,
        Particle,
    };

    glm::dvec3 origin{};
    glm::vec3 extent{};
    glm::vec4 color{};
    Kind kind = Kind::TargetOutline;
    uint32_t crack_seed = 0U;
};

class BlockInteractionEffects final {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint32_t PARTICLES_PER_BREAK = 16U;
    static constexpr uint32_t PARTICLES_PER_BREAKING_BURST = 3U;
    static constexpr uint32_t MAXIMUM_PARTICLES = 256U;
    static constexpr std::chrono::milliseconds BREAKING_PARTICLE_INTERVAL{120};

    BlockInteractionEffects();
    void setTarget(std::optional<shared::BlockTarget> target, float authoritative_progress,
        Clock::time_point now = Clock::now()) noexcept;
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
        bool from_breaking_hold = false;
    };
    void emitParticles(shared::WorldCoordinate cell, shared::Block material, uint32_t count,
        uint64_t seed, Clock::time_point now, bool from_breaking_hold,
        shared::BlockFace entered_face = shared::BlockFace::None) noexcept;

    std::optional<shared::BlockTarget> m_target;
    float m_progress = 0.0F;
    std::optional<shared::BlockTarget> m_breaking_target;
    std::optional<Clock::time_point> m_next_breaking_particle_at;
    uint64_t m_breaking_burst_index = 0U;
    std::optional<BlockBreakEventIdentity> m_last_event;
    std::vector<Particle> m_particles;
};

} // namespace client
