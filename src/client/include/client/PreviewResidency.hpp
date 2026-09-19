#pragma once

#include <shared/net/Message.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace client {

using HeightTileKey = shared::HeightTileKey;

struct HeightTileRevision final {
    uint64_t generation;
    uint64_t revision;

    constexpr bool operator==(HeightTileRevision const&) const noexcept = default;
};

class HeightTile final {
public:
    [[nodiscard]] HeightTileKey key() const noexcept { return m_key; }
    [[nodiscard]] HeightTileRevision revision() const noexcept { return m_revision; }
    [[nodiscard]] uint64_t token() const noexcept { return m_token; }
    [[nodiscard]] std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> const& heights() const noexcept
    {
        return m_heights;
    }
    [[nodiscard]] static constexpr uint64_t byteSize() noexcept
    {
        return shared::HEIGHT_TILE_PAYLOAD_BYTES;
    }

private:
    HeightTile(
        HeightTileKey key,
        HeightTileRevision revision,
        uint64_t token,
        std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights
    )
        : m_key(key)
        , m_revision(revision)
        , m_token(token)
        , m_heights(std::move(heights))
    { }

    HeightTileKey m_key;
    HeightTileRevision m_revision;
    uint64_t m_token;
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> m_heights;

    friend class PreviewResidency;
};

using HeightTileHandle = std::shared_ptr<HeightTile const>;

struct PreviewResidencyLimits final {
    static constexpr uint32_t HYSTERESIS_TILES = 240U;

    uint32_t max_resident_tiles = shared::HEIGHT_TILE_INTEREST_COUNT + HYSTERESIS_TILES;
    uint64_t max_resident_bytes = static_cast<uint64_t>(
        shared::HEIGHT_TILE_INTEREST_COUNT + HYSTERESIS_TILES
    ) * shared::HEIGHT_TILE_PAYLOAD_BYTES;
};

enum class HeightTileReplacement : uint8_t {
    Published,
    Stale,
    BudgetExceeded,
};

struct HeightTileReplacementResult final {
    HeightTileReplacement replacement;
    HeightTileHandle handle;
};

enum class HeightTileChangeKind : uint8_t {
    Upsert,
    Remove,
};

struct HeightTileChange final {
    HeightTileKey key;
    HeightTileChangeKind kind;
};

struct PreviewResidencyStats final {
    uint32_t resident_tiles = 0;
    uint64_t resident_bytes = 0;
    uint32_t eviction_count = 0;
};

class PreviewResidency final {
public:
    explicit PreviewResidency(HeightTileRevision revision, PreviewResidencyLimits limits = {})
        : m_revision(revision)
        , m_limits(limits)
    { }

    [[nodiscard]] HeightTileRevision currentRevision() const noexcept { return m_revision; }
    void advanceRevision(HeightTileRevision revision);

    [[nodiscard]] HeightTileReplacementResult accept(
        HeightTileKey key,
        HeightTileRevision revision,
        uint64_t token,
        std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights
    );
    [[nodiscard]] bool evict(HeightTileKey key, HeightTileRevision revision, uint64_t token);
    [[nodiscard]] HeightTileHandle resident(HeightTileKey key) const noexcept;
    [[nodiscard]] uint32_t pendingChangeCount() const noexcept;
    [[nodiscard]] std::vector<HeightTileChange> takeChanges(uint32_t maximum_changes);
    [[nodiscard]] PreviewResidencyStats stats() const noexcept;

private:
    struct HeightTileKeyHash final {
        [[nodiscard]] uint64_t operator()(HeightTileKey key) const noexcept;
    };

    struct Resident final {
        HeightTileHandle tile;
        uint64_t touch = 0;
    };

    [[nodiscard]] uint64_t knownToken(HeightTileKey key) const noexcept;
    void recordToken(HeightTileKey key, uint64_t token);
    void markChanged(HeightTileKey key, HeightTileChangeKind kind);
    [[nodiscard]] bool makeRoom(HeightTileKey protected_key);

    HeightTileRevision m_revision;
    PreviewResidencyLimits m_limits;
    uint64_t m_touch = 1;
    uint64_t m_resident_bytes = 0;
    uint32_t m_eviction_count = 0;
    std::unordered_map<HeightTileKey, Resident, HeightTileKeyHash> m_residents;
    std::unordered_map<HeightTileKey, uint64_t, HeightTileKeyHash> m_tokens;
    std::deque<std::pair<HeightTileKey, uint64_t>> m_resident_order;
    std::deque<std::pair<HeightTileKey, uint64_t>> m_token_order;
    std::deque<HeightTileKey> m_change_order;
    std::unordered_map<HeightTileKey, HeightTileChangeKind, HeightTileKeyHash> m_changes;
};

} // namespace client
