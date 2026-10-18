#include <client/HeightTileInterestRegion.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>

#include <core/common/Assert.hpp>

namespace client {

void HeightTileInterestRegion::set(shared::HeightTileKey const center, uint32_t const radius)
{
    ASSERT(shared::isValidHeightTileInterestRadius(radius), "invalid height-tile interest radius");
    m_center = shared::normalizeHeightTileKey(center);
    m_radius = radius;
    m_size = shared::heightTileInterestCount(radius);
}

void HeightTileInterestRegion::clear() noexcept
{
    m_center.reset();
    m_radius = 0U;
    m_size = 0U;
}

bool HeightTileInterestRegion::contains(shared::HeightTileKey const key) const
{
    return m_center.has_value() && shared::heightTileWithinInterest(*m_center, key, m_radius);
}

std::optional<shared::HeightTileKey> HeightTileInterestRegion::center() const noexcept
{
    return m_center;
}

uint32_t HeightTileInterestRegion::radius() const noexcept
{
    return m_radius;
}

uint32_t HeightTileInterestRegion::size() const noexcept
{
    return m_size;
}

bool HeightTileInterestRegion::empty() const noexcept
{
    return !m_center.has_value();
}

} // namespace client
