#include <client/render/BlockInteractionEffects.hpp>

#include <algorithm>
#include <cmath>

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

} // namespace

void BlockInteractionEffects::setTarget(std::optional<shared::BlockTarget> target,
    float authoritative_progress) noexcept
{
    m_target = target && shared::isSolidBlock(target->block) ? target : std::nullopt;
    m_progress = std::isfinite(authoritative_progress)
        ? std::clamp(authoritative_progress, 0.0F, 1.0F) : 0.0F;
}

bool BlockInteractionEffects::acceptBreak(BlockBreakEventIdentity event_id,
    shared::WorldCoordinate cell, shared::Block material, Clock::time_point now)
{
    if (!shared::isSolidBlock(material) || (m_last_event && event_id <= *m_last_event)) {
        return false;
    }
    m_last_event = event_id;
    std::erase_if(m_particles, [&](Particle const& particle) {
        return now - particle.started_at >= std::chrono::seconds{1};
    });
    if (m_particles.size() + PARTICLES_PER_BREAK > MAXIMUM_PARTICLES) {
        m_particles.erase(m_particles.begin(), m_particles.begin()
            + static_cast<ptrdiff_t>(m_particles.size() + PARTICLES_PER_BREAK - MAXIMUM_PARTICLES));
    }
    uint64_t seed = mix(event_id.join_epoch) ^ mix(event_id.server_tick)
        ^ mix(event_id.sequence) ^ mix(event_id.cell_version)
        ^ mix(static_cast<uint64_t>(cell.x)) ^ mix(static_cast<uint64_t>(cell.y))
        ^ mix(static_cast<uint64_t>(cell.z));
    auto const random = [&]() {
        seed = mix(seed + 0x9e37'79b9'7f4a'7c15ULL);
        return static_cast<float>(seed & 0xffffU) / 65'535.0F;
    };
    for (uint32_t index = 0U; index < PARTICLES_PER_BREAK; ++index) {
        glm::vec3 const offset{random(), random(), random()};
        m_particles.push_back({
            .origin = glm::dvec3{static_cast<double>(cell.x), static_cast<double>(cell.y),
                static_cast<double>(cell.z)} + glm::dvec3{offset},
            .velocity = {(offset.x - 0.5F) * 2.0F, (offset.y - 0.5F) * 2.0F,
                0.8F + random() * 1.2F},
            .color = materialColor(material),
            .started_at = now,
        });
    }
    return true;
}

void BlockInteractionEffects::clear() noexcept
{
    m_target.reset();
    m_progress = 0.0F;
    m_last_event.reset();
    m_particles.clear();
}

std::vector<BlockEffectBox> BlockInteractionEffects::boxes(Clock::time_point now)
{
    std::vector<BlockEffectBox> result;
    result.reserve(MAXIMUM_PARTICLES + 60U);
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
        uint32_t const stage = static_cast<uint32_t>(std::ceil(m_progress * 8.0F));
        for (int axis = 0; axis < 3; ++axis) {
            int const u = (axis + 1) % 3;
            int const v = (axis + 2) % 3;
            for (int side = 0; side < 2; ++side) {
                for (uint32_t segment = 0U; segment < stage; ++segment) {
                    glm::dvec3 origin = cell;
                    origin[axis] += side == 0 ? -0.004 : 1.001;
                    origin[u] += 0.12 + static_cast<double>(segment) * 0.09;
                    origin[v] += segment % 2U == 0U ? 0.35 : 0.44;
                    glm::vec3 extent{0.026F};
                    extent[axis] = 0.003F;
                    extent[u] = 0.11F;
                    extent[v] = 0.11F;
                    // A thin L on each face forms an angular, growing crack network.
                    glm::vec3 horizontal = extent;
                    horizontal[v] = 0.026F;
                    result.push_back({origin, horizontal, {0.04F, 0.04F, 0.04F, 1.0F}});
                    origin[u] += 0.084;
                    extent[u] = 0.026F;
                    result.push_back({origin, extent, {0.04F, 0.04F, 0.04F, 1.0F}});
                }
            }
        }
    }
    std::erase_if(m_particles, [&](Particle const& particle) {
        return now - particle.started_at >= std::chrono::seconds{1};
    });
    for (Particle const& particle : m_particles) {
        float const age = std::max(0.0F, std::chrono::duration<float>(now - particle.started_at).count());
        glm::dvec3 const offset = glm::dvec3{particle.velocity} * static_cast<double>(age)
            + glm::dvec3{0.0, 0.0, -2.0 * age * age};
        result.push_back({particle.origin + offset, glm::vec3{0.075F}, particle.color});
    }
    return result;
}

} // namespace client
