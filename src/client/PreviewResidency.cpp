#include <client/PreviewResidency.hpp>

#include <algorithm>
#include <utility>

namespace client {

void PreviewResidency::advanceRevision(HeightTileRevision const revision)
{
    if (revision == m_revision) {
        return;
    }
    for (auto const& [key, resident] : m_residents) {
        static_cast<void>(resident);
        markChanged(key, HeightTileChangeKind::Remove);
    }
    m_revision = revision;
    m_residents.clear();
    m_tokens.clear();
    m_resident_order.clear();
    m_token_order.clear();
    m_resident_bytes = 0U;
}

HeightTileReplacementResult PreviewResidency::accept(
    HeightTileKey const key,
    HeightTileRevision const revision,
    uint64_t const token,
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights
)
{
    if (revision != m_revision || token == 0U || token <= knownToken(key)) {
        return { .replacement = HeightTileReplacement::Stale };
    }
    auto existing = m_residents.find(key);
    if (existing == m_residents.end() && !makeRoom(key)) {
        return { .replacement = HeightTileReplacement::BudgetExceeded };
    }

    HeightTileHandle const tile{ new HeightTile{ key, revision, token, std::move(heights) } };
    uint64_t const touch = m_touch++;
    if (existing == m_residents.end()) {
        m_residents.emplace(key, Resident{ .tile = tile, .touch = touch });
        m_resident_bytes += HeightTile::byteSize();
    } else {
        existing->second = Resident{ .tile = tile, .touch = touch };
    }
    m_resident_order.emplace_back(key, touch);
    recordToken(key, token);
    markChanged(key, HeightTileChangeKind::Upsert);
    return { .replacement = HeightTileReplacement::Published, .handle = tile };
}

bool PreviewResidency::evict(
    HeightTileKey const key,
    HeightTileRevision const revision,
    uint64_t const token
)
{
    if (revision != m_revision || token == 0U || token < knownToken(key)) {
        return false;
    }
    recordToken(key, token);
    auto const resident = m_residents.find(key);
    if (resident == m_residents.end()) {
        return false;
    }
    m_residents.erase(resident);
    m_resident_bytes -= HeightTile::byteSize();
    markChanged(key, HeightTileChangeKind::Remove);
    return true;
}

HeightTileHandle PreviewResidency::resident(HeightTileKey const key) const noexcept
{
    auto const found = m_residents.find(key);
    return found == m_residents.end() ? nullptr : found->second.tile;
}

uint32_t PreviewResidency::pendingChangeCount() const noexcept
{
    return static_cast<uint32_t>(m_change_order.size());
}

std::vector<HeightTileChange> PreviewResidency::takeChanges(uint32_t const maximum_changes)
{
    uint32_t const count = std::min<uint32_t>(maximum_changes, static_cast<uint32_t>(m_change_order.size()));
    std::vector<HeightTileChange> result;
    result.reserve(count);
    for (uint32_t index = 0U; index < count; ++index) {
        HeightTileKey const key = m_change_order.front();
        m_change_order.pop_front();
        auto const change = m_changes.find(key);
        result.push_back({.key = key, .kind = change->second});
        m_changes.erase(change);
    }
    return result;
}

PreviewResidencyStats PreviewResidency::stats() const noexcept
{
    return {
        .resident_tiles = static_cast<uint32_t>(m_residents.size()),
        .resident_bytes = m_resident_bytes,
        .eviction_count = m_eviction_count,
    };
}

uint64_t PreviewResidency::HeightTileKeyHash::operator()(HeightTileKey const key) const noexcept
{
    uint64_t const x = static_cast<uint32_t>(key.x);
    uint64_t const y = static_cast<uint32_t>(key.y);
    return x << 32U | y;
}

uint64_t PreviewResidency::knownToken(HeightTileKey const key) const noexcept
{
    auto const found = m_tokens.find(key);
    return found == m_tokens.end() ? 0U : found->second;
}

void PreviewResidency::recordToken(HeightTileKey const key, uint64_t const token)
{
    m_tokens.insert_or_assign(key, token);
    m_token_order.emplace_back(key, token);
    uint64_t const maximum_tokens = static_cast<uint64_t>(m_limits.max_resident_tiles)
        + PreviewResidencyLimits::HYSTERESIS_TILES;
    while (static_cast<uint64_t>(m_tokens.size()) > maximum_tokens) {
        while (!m_token_order.empty()) {
            auto const [candidate_key, candidate_token] = m_token_order.front();
            auto const candidate = m_tokens.find(candidate_key);
            if (candidate != m_tokens.end() && candidate->second == candidate_token
                && !m_residents.contains(candidate_key)) {
                m_tokens.erase(candidate);
                m_token_order.pop_front();
                break;
            }
            m_token_order.pop_front();
        }
        if (m_token_order.empty() && static_cast<uint64_t>(m_tokens.size()) > maximum_tokens) {
            return;
        }
    }
}

void PreviewResidency::markChanged(HeightTileKey const key, HeightTileChangeKind const kind)
{
    auto const found = m_changes.find(key);
    if (found == m_changes.end()) {
        m_changes.emplace(key, kind);
        m_change_order.push_back(key);
    } else {
        found->second = kind;
    }
}

bool PreviewResidency::makeRoom(HeightTileKey const protected_key)
{
    if (HeightTile::byteSize() > m_limits.max_resident_bytes) {
        return false;
    }
    while (m_residents.size() >= m_limits.max_resident_tiles
        || m_resident_bytes > m_limits.max_resident_bytes - HeightTile::byteSize()) {
        auto victim = m_residents.end();
        while (!m_resident_order.empty()) {
            auto const [candidate_key, candidate_touch] = m_resident_order.front();
            m_resident_order.pop_front();
            auto const candidate = m_residents.find(candidate_key);
            if (candidate != m_residents.end() && candidate->second.touch == candidate_touch
                && candidate_key != protected_key) {
                victim = candidate;
                break;
            }
        }
        if (victim == m_residents.end()) {
            return false;
        }
        HeightTileKey const key = victim->first;
        recordToken(key, victim->second.tile->token());
        m_residents.erase(victim);
        m_resident_bytes -= HeightTile::byteSize();
        ++m_eviction_count;
        markChanged(key, HeightTileChangeKind::Remove);
    }
    return true;
}

} // namespace client
