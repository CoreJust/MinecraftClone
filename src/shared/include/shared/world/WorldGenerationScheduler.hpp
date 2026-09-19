#pragma once

#include <shared/world/Chunk.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

namespace shared {

enum class GenerationStage : uint8_t {
    HeightTile,
    Materialize,
};

enum class GenerationAdmission : uint8_t {
    Accepted,
    QueueFull,
    Duplicate,
};

using GenerationJobId = uint64_t;

struct GenerationJob final {
    GenerationJobId id = 0U;
    ChunkCoordinate coordinate{};
    uint64_t revision = 0U;
    GenerationStage stage = GenerationStage::HeightTile;
    uint8_t retries = 0U;

    constexpr bool operator==(GenerationJob const&) const noexcept = default;
};

struct GenerationResult final {
    GenerationJob job;
    bool succeeded = false;
    bool cancelled = false;

    constexpr bool operator==(GenerationResult const&) const noexcept = default;
};

class WorldGenerationScheduler final {
public:
    static constexpr uint8_t MAX_RETRIES = 1U;

    explicit WorldGenerationScheduler(uint64_t max_pending_jobs = 32U);

    [[nodiscard]]
    GenerationAdmission submit(
        ChunkCoordinate coordinate,
        uint64_t revision,
        GenerationStage stage
    );

    [[nodiscard]]
    std::optional<GenerationJob> takeNext() noexcept;

    [[nodiscard]]
    bool complete(GenerationJobId id, bool succeeded, bool cancelled = false) noexcept;

    [[nodiscard]]
    bool retry(GenerationJobId id) noexcept;

    [[nodiscard]]
    bool cancel(GenerationJobId id) noexcept;

    void invalidateRevision(uint64_t revision) noexcept;

    [[nodiscard]]
    std::optional<GenerationResult> takeResult() noexcept;

    [[nodiscard]]
    uint64_t pendingCount() const noexcept;

    [[nodiscard]]
    uint64_t resultCount() const noexcept;

private:
    struct JobState final {
        GenerationJob job;
        bool running = false;
        bool completed = false;
        bool cancelled = false;
        bool stale = false;
    };

private:
    uint64_t m_max_pending_jobs;
    GenerationJobId m_next_id = 1U;
    std::deque<GenerationJobId> m_queue;
    std::unordered_map<GenerationJobId, JobState> m_jobs;
    std::deque<GenerationResult> m_results;
};

} // namespace shared
