#pragma once

#include <shared/net/Message.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace client {

using PreviewChunkKey = shared::PreviewChunkKey;
using PreviewLevel = shared::PreviewLevel;

struct PreviewRevision final {
    uint64_t generation;
    uint64_t revision;

    constexpr bool operator==(PreviewRevision const&) const noexcept = default;
};

class PreviewMesh final {
public:
    [[nodiscard]] PreviewChunkKey key() const noexcept { return m_key; }
    [[nodiscard]] PreviewRevision revision() const noexcept { return m_revision; }
    [[nodiscard]] PreviewLevel level() const noexcept { return m_level; }
    [[nodiscard]] std::span<uint8_t const> bytes() const noexcept { return m_bytes; }
    [[nodiscard]] uint64_t byteSize() const noexcept { return m_bytes.size(); }

private:
    PreviewMesh(
        PreviewChunkKey key,
        PreviewRevision revision,
        PreviewLevel level,
        std::vector<uint8_t> bytes
    )
        : m_key(key)
        , m_revision(revision)
        , m_level(level)
        , m_bytes(std::move(bytes))
    { }

    PreviewChunkKey m_key;
    PreviewRevision m_revision;
    PreviewLevel m_level;
    std::vector<uint8_t> m_bytes;

    friend class PreviewResidency;
};

using PreviewHandle = std::shared_ptr<PreviewMesh const>;

struct PreviewResidencyLimits final {
    uint32_t max_resident_chunks = 320;
    uint32_t max_pending_requests = 320;
    uint64_t max_resident_bytes = 4U * 1024U * 1024U;
    uint64_t max_pending_upload_bytes = 2U * 1024U * 1024U;
};

class PreviewRequest final {
public:
    [[nodiscard]] PreviewChunkKey key() const noexcept { return m_key; }
    [[nodiscard]] PreviewRevision revision() const noexcept { return m_revision; }
    [[nodiscard]] PreviewLevel level() const noexcept { return m_level; }

private:
    PreviewRequest(uint64_t token, PreviewChunkKey key, PreviewRevision revision, PreviewLevel level)
        : m_token(token), m_key(key), m_revision(revision), m_level(level)
    { }
    uint64_t m_token;
    PreviewChunkKey m_key;
    PreviewRevision m_revision;
    PreviewLevel m_level;
    friend class PreviewResidency;
};

enum class PreviewRequestAdmission : uint8_t { Accepted, StaleRevision, Saturated };

struct PreviewRequestResult final {
    PreviewRequestAdmission admission;
    std::optional<PreviewRequest> request;
};

enum class PreviewReplacement : uint8_t {
    Published,
    Stale,
    Cancelled,
    Superseded,
    BudgetExceeded,
};

struct PreviewReplacementResult final {
    PreviewReplacement replacement;
    PreviewHandle handle;
};

enum class PreviewSelectionKind : uint8_t {
    Final,
    Coarse,
    Fallback,
    Missing,
};

struct PreviewSelection final {
    PreviewSelectionKind kind;
    PreviewHandle handle;
};

struct PreviewResidencyStats final {
    uint32_t resident_chunks = 0;
    uint32_t resident_meshes = 0;
    uint32_t pending_requests = 0;
    uint64_t resident_bytes = 0;
    uint64_t pending_upload_bytes = 0;
    uint64_t uploaded_bytes = 0;
    uint32_t eviction_count = 0;
};

[[nodiscard]]
constexpr int32_t shortestWrappedDisplacement(
    int32_t const from,
    int32_t const to,
    int32_t const extent = 4'096
) noexcept
{
    if (extent <= 0) {
        return to - from;
    }
    int32_t displacement = (to - from) % extent;
    if (displacement > extent / 2) {
        displacement -= extent;
    } else if (displacement < -(extent / 2)) {
        displacement += extent;
    }
    return displacement;
}

[[nodiscard]]
constexpr PreviewLevel selectPreviewLevel(
    PreviewChunkKey const center,
    PreviewChunkKey const key
) noexcept
{
    int64_t const x = shortestWrappedDisplacement(center.x, key.x);
    int64_t const y = shortestWrappedDisplacement(center.y, key.y);
    int64_t const distance_squared = x * x + y * y;
    return distance_squared <= 16 ? PreviewLevel::Final : PreviewLevel::Coarse;
}

class PreviewResidency final {
public:
    PreviewResidency(PreviewRevision revision, PreviewResidencyLimits limits = {})
        : m_revision(revision)
        , m_limits(limits)
    { }

    [[nodiscard]] PreviewRevision currentRevision() const noexcept { return m_revision; }
    [[nodiscard]] uint64_t changeSerial() const noexcept { return m_change_serial; }
    void advanceRevision(PreviewRevision revision) noexcept;

    [[nodiscard]] PreviewRequestResult request(
        PreviewChunkKey key,
        PreviewLevel level,
        PreviewRevision revision
    );
    void cancel(PreviewRequest const& request) noexcept;
    [[nodiscard]] PreviewReplacementResult replace(
        PreviewRequest const& request,
        std::vector<uint8_t> bytes
    );

    [[nodiscard]] PreviewReplacementResult accept(
        PreviewChunkKey key,
        PreviewRevision revision,
        PreviewLevel level,
        uint64_t token,
        std::vector<uint8_t> bytes
    );
    [[nodiscard]] bool markUploaded(PreviewHandle const& handle) noexcept;
    [[nodiscard]] bool evict(PreviewChunkKey key) noexcept;
    [[nodiscard]] PreviewSelection select(
        PreviewChunkKey key,
        PreviewLevel requested_level,
        PreviewRevision revision
    ) const noexcept;
    [[nodiscard]] PreviewResidencyStats stats() const noexcept;
    [[nodiscard]] std::vector<PreviewHandle> handles() const;

private:
    struct Resident final {
        PreviewChunkKey key;
        PreviewHandle coarse;
        PreviewHandle final;
        uint64_t coarse_token = 0;
        uint64_t final_token = 0;
        bool coarse_pending = false;
        bool final_pending = false;
        uint64_t touch = 0;
    };
    struct Pending final {
        PreviewRequest request;
        bool cancelled = false;
    };

    [[nodiscard]] Resident* resident(PreviewChunkKey key) noexcept;
    [[nodiscard]] Resident const* resident(PreviewChunkKey key) const noexcept;
    void remove(Resident& value) noexcept;
    [[nodiscard]] bool makeRoom(PreviewChunkKey protected_key, uint64_t bytes) noexcept;

    PreviewRevision m_revision;
    PreviewResidencyLimits m_limits;
    uint64_t m_touch = 1;
    uint64_t m_change_serial = 1;
    uint64_t m_next_token = 1;
    std::vector<Pending> m_pending;
    std::vector<Resident> m_residents;
    uint64_t m_resident_bytes = 0;
    uint64_t m_pending_upload_bytes = 0;
    uint64_t m_uploaded_bytes = 0;
    uint32_t m_eviction_count = 0;
};

} // namespace client
