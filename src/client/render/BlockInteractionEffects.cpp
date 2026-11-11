#include <client/render/BlockInteractionEffects.hpp>

#include <shared/world/World.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace client::detail {

glm::dvec3 cameraRelativeBlockEffectOrigin(glm::dvec3 origin, glm::dvec3 camera_position) noexcept
{
    constexpr double WORLD_WRAP_PERIOD = static_cast<double>(shared::WorldExtent::WIDTH);
    origin.x += std::round((camera_position.x - origin.x) / WORLD_WRAP_PERIOD) * WORLD_WRAP_PERIOD;
    origin.y += std::round((camera_position.y - origin.y) / WORLD_WRAP_PERIOD) * WORLD_WRAP_PERIOD;
    return origin - camera_position;
}

} // namespace client::detail

namespace client {
namespace {

glm::vec4 materialColor(shared::Block material)
{
    switch (material) {
    case shared::Block::Grass: return {0.36F, 0.58F, 0.27F, 1.0F};
    case shared::Block::Dirt: return {0.49F, 0.38F, 0.26F, 1.0F};
    case shared::Block::Snow: return {0.96F, 0.97F, 0.98F, 1.0F};
    case shared::Block::Ice: return {0.50F, 0.79F, 0.84F, 1.0F};
    case shared::Block::Sand: return {0.88F, 0.78F, 0.57F, 1.0F};
    default: return {0.50F, 0.50F, 0.50F, 1.0F};
    }
}

uint64_t mix(uint64_t value)
{
    value ^= value >> 30U;
    value *= 0xbf58'476d'1ce4'e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d0'49bb'1331'11ebULL;
    return value ^ (value >> 31U);
}

uint64_t coordinateSeed(shared::WorldCoordinate const cell)
{
    return mix(static_cast<uint64_t>(cell.x)) ^ mix(static_cast<uint64_t>(cell.y))
        ^ mix(static_cast<uint64_t>(cell.z));
}

uint32_t crackSeed(shared::BlockTarget const& target)
{
    uint64_t const seed = mix(coordinateSeed(target.cell)
        ^ mix(static_cast<uint64_t>(target.block)));
    return static_cast<uint32_t>(seed & 0x00ff'ffffU);
}

constexpr double worldGravityBlocksPerSecondSquared()
{
    // World gravity is stored in subcells per tick squared; effects use blocks per second squared.
    constexpr double TICK_SECONDS = static_cast<double>(shared::TICK.count()) / 1000.0;
    constexpr double GRAVITY_BLOCKS_PER_TICK_SQUARED =
        static_cast<double>(shared::World::PLAYER_GRAVITY_SUBCELLS_PER_TICK)
        / static_cast<double>(shared::SUBCELLS_PER_CELL);
    return GRAVITY_BLOCKS_PER_TICK_SQUARED / (TICK_SECONDS * TICK_SECONDS);
}

constexpr double WORLD_GRAVITY_BLOCKS_PER_SECOND_SQUARED = worldGravityBlocksPerSecondSquared();
constexpr std::chrono::seconds PARTICLE_LIFETIME{1};
constexpr float PARTICLE_EXTENT = 0.075F;
constexpr double PARTICLE_FACE_OUTSET = 0.025;
constexpr float PARTICLE_OUTWARD_SPEED = 0.8F;

struct FaceOrientation final {
    int axis;
    float direction;
};

std::optional<FaceOrientation> faceOrientation(shared::BlockFace face) noexcept
{
    switch (face) {
    case shared::BlockFace::NegativeX: return FaceOrientation{.axis = 0, .direction = -1.0F};
    case shared::BlockFace::PositiveX: return FaceOrientation{.axis = 0, .direction = 1.0F};
    case shared::BlockFace::NegativeY: return FaceOrientation{.axis = 1, .direction = -1.0F};
    case shared::BlockFace::PositiveY: return FaceOrientation{.axis = 1, .direction = 1.0F};
    case shared::BlockFace::NegativeZ: return FaceOrientation{.axis = 2, .direction = -1.0F};
    case shared::BlockFace::PositiveZ: return FaceOrientation{.axis = 2, .direction = 1.0F};
    case shared::BlockFace::None: return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

BlockInteractionEffects::BlockInteractionEffects()
{
    // setTarget is called per frame and stays noexcept by retaining the bounded pool up front.
    m_particles.reserve(MAXIMUM_PARTICLES);
}

void BlockInteractionEffects::setTarget(std::optional<shared::BlockTarget> target,
    float authoritative_progress, Clock::time_point now) noexcept
{
    std::optional<shared::BlockTarget> const valid_target =
        target && shared::isSolidBlock(target->block) ? target : std::nullopt;
    float const progress = std::isfinite(authoritative_progress)
        ? std::clamp(authoritative_progress, 0.0F, 1.0F) : 0.0F;

    bool const same_breaking_target = valid_target && m_breaking_target
        && m_breaking_target->cell == valid_target->cell
        && m_breaking_target->block == valid_target->block;
    if (!valid_target || progress <= 0.0F || !same_breaking_target) {
        std::erase_if(m_particles, [](Particle const& particle) { return particle.from_breaking_hold; });
    }
    m_target = valid_target;
    m_progress = progress;
    if (!m_target || m_progress <= 0.0F) {
        m_breaking_target.reset();
        m_next_breaking_particle_at.reset();
        m_breaking_burst_index = 0U;
        return;
    }

    if (!same_breaking_target) {
        m_breaking_target = m_target;
        m_next_breaking_particle_at.reset();
        m_breaking_burst_index = 0U;
    }
    if (!m_next_breaking_particle_at || now >= *m_next_breaking_particle_at) {
        uint64_t const seed = mix(coordinateSeed(m_target->cell)
            ^ mix(static_cast<uint64_t>(m_target->block)) ^ mix(m_breaking_burst_index++));
        emitParticles(m_target->cell, m_target->block, PARTICLES_PER_BREAKING_BURST, seed, now, true,
            m_target->entered_face);
        m_next_breaking_particle_at = now + BREAKING_PARTICLE_INTERVAL;
    }
}

bool BlockInteractionEffects::acceptBreak(BlockBreakEventIdentity event_id,
    shared::WorldCoordinate cell, shared::Block material, Clock::time_point now)
{
    if (!shared::isSolidBlock(material) || (m_last_event && event_id <= *m_last_event)) {
        return false;
    }
    m_last_event = event_id;
    uint64_t const seed = mix(mix(event_id.join_epoch) ^ mix(event_id.server_tick)
        ^ mix(event_id.sequence) ^ mix(event_id.cell_version) ^ coordinateSeed(cell));
    emitParticles(cell, material, PARTICLES_PER_BREAK, seed, now, false);
    return true;
}

void BlockInteractionEffects::emitParticles(shared::WorldCoordinate cell, shared::Block material,
    uint32_t count, uint64_t seed, Clock::time_point now, bool from_breaking_hold,
    shared::BlockFace entered_face) noexcept
{
    std::erase_if(m_particles, [&](Particle const& particle) {
        return now - particle.started_at >= PARTICLE_LIFETIME;
    });
    if (m_particles.size() + count > MAXIMUM_PARTICLES) {
        m_particles.erase(m_particles.begin(), m_particles.begin()
            + static_cast<std::vector<Particle>::difference_type>(
                m_particles.size() + count - MAXIMUM_PARTICLES));
    }
    auto const random = [&]() {
        seed = mix(seed + 0x9e37'79b9'7f4a'7c15ULL);
        return static_cast<float>(seed & 0xffffU) / 65'535.0F;
    };
    glm::dvec3 const cell_origin{static_cast<double>(cell.x), static_cast<double>(cell.y),
        static_cast<double>(cell.z)};
    for (uint32_t index = 0U; index < count; ++index) {
        glm::vec3 const offset{random(), random(), random()};
        glm::dvec3 origin = cell_origin + glm::dvec3{offset};
        glm::vec3 velocity{(offset.x - 0.5F) * 2.0F, (offset.y - 0.5F) * 2.0F,
            0.8F + random() * 1.2F};
        if (from_breaking_hold) {
            // A missing entered face is possible for synthetic/no-ray targets; keep their deterministic volume fallback.
            if (std::optional<FaceOrientation> const face = faceOrientation(entered_face)) {
                for (int axis = 0; axis < 3; ++axis) {
                    if (axis != face->axis) {
                        origin[axis] = cell_origin[axis]
                            + static_cast<double>(offset[axis]) * (1.0 - static_cast<double>(PARTICLE_EXTENT));
                    }
                }
                if (face->direction < 0.0F) {
                    origin[face->axis] = cell_origin[face->axis]
                        - static_cast<double>(PARTICLE_EXTENT) - PARTICLE_FACE_OUTSET;
                } else {
                    origin[face->axis] = cell_origin[face->axis] + 1.0 + PARTICLE_FACE_OUTSET;
                }
                velocity[face->axis] = face->direction * PARTICLE_OUTWARD_SPEED;
            }
        }
        m_particles.push_back({
            .origin = origin,
            .velocity = velocity,
            .color = materialColor(material),
            .started_at = now,
            .from_breaking_hold = from_breaking_hold,
        });
    }
}

void BlockInteractionEffects::clear() noexcept
{
    m_target.reset();
    m_progress = 0.0F;
    m_breaking_target.reset();
    m_next_breaking_particle_at.reset();
    m_breaking_burst_index = 0U;
    m_last_event.reset();
    m_particles.clear();
}

std::vector<BlockEffectBox> BlockInteractionEffects::boxes(Clock::time_point now)
{
    std::vector<BlockEffectBox> result;
    result.reserve(MAXIMUM_PARTICLES + 13U);
    if (m_target) {
        glm::dvec3 const cell{static_cast<double>(m_target->cell.x),
            static_cast<double>(m_target->cell.y), static_cast<double>(m_target->cell.z)};
        constexpr float EDGE = 0.018F;
        constexpr double OUTSET = 0.006;
        for (int axis = 0; axis < 3; ++axis) {
            int const u = (axis + 1) % 3;
            int const v = (axis + 2) % 3;
            for (int side_u = 0; side_u < 2; ++side_u) {
                for (int side_v = 0; side_v < 2; ++side_v) {
                    glm::dvec3 origin = cell - glm::dvec3{OUTSET};
                    origin[u] += side_u == 0 ? 0.0 : 1.0;
                    origin[v] += side_v == 0 ? 0.0 : 1.0;
                    glm::vec3 extent{EDGE};
                    extent[axis] = 1.0F + EDGE;
                    result.push_back({origin, extent, {1.0F, 0.86F, 0.14F, 1.0F}});
                }
            }
        }
        if (m_progress > 0.0F) {
            constexpr double CRACK_SURFACE_OUTSET = 0.004;
            result.push_back({
                .origin = cell - glm::dvec3{CRACK_SURFACE_OUTSET},
                .extent = glm::vec3{1.0F + 2.0F * static_cast<float>(CRACK_SURFACE_OUTSET)},
                // Negative alpha selects the procedural crack shader path and carries progress.
                .color = {0.20F, 0.18F, 0.16F, -m_progress},
                .kind = BlockEffectBox::Kind::ProceduralCrack,
                .crack_seed = crackSeed(*m_target),
            });
        }
    }
    std::erase_if(m_particles, [&](Particle const& particle) {
        return now - particle.started_at >= PARTICLE_LIFETIME;
    });
    for (Particle const& particle : m_particles) {
        double const age_seconds = std::max(0.0, std::chrono::duration<double>(now - particle.started_at).count());
        glm::dvec3 offset = glm::dvec3{particle.velocity} * age_seconds;
        offset.z -= 0.5 * WORLD_GRAVITY_BLOCKS_PER_SECOND_SQUARED * age_seconds * age_seconds;
        result.push_back({
            .origin = particle.origin + offset,
            .extent = glm::vec3{PARTICLE_EXTENT},
            .color = particle.color,
            .kind = BlockEffectBox::Kind::Particle,
        });
    }
    return result;
}

} // namespace client
