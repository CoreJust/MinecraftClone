#pragma once

#include <shared/world/Chunk.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace shared {

enum class GenerationStage : uint8_t {
    HeightTile,
    Pregen,
    Refinement,
    Materialize,
};

enum class GenerationPriorityDimension : uint8_t {
    Distance,
    View,
    Movement,
};

inline constexpr std::array<GenerationPriorityDimension, 3> DEFAULT_GENERATION_PRIORITY_ORDER{
    GenerationPriorityDimension::Distance,
    GenerationPriorityDimension::View,
    GenerationPriorityDimension::Movement,
};

inline constexpr std::array<uint32_t, 5> REGISTERED_WORLD_GENERATION_EXTENTS{
    16'384U,
    4'096U,
    1'024U,
    256U,
    64U,
};

inline constexpr uint32_t MAX_GENERATION_STAGE_OUTPUT_BYTES = 4U * 1'024U;
inline constexpr uint64_t MAX_GENERATION_OUTPUT_BUDGET_BYTES = 1U * 1'024U * 1'024U;

enum class GenerationAdmission : uint8_t {
    Accepted,
    QueueFull,
    Duplicate,
    InvalidPlan,
};

enum class GenerationState : uint8_t {
    Unknown,
    Queued,
    Running,
    Preview,
    Materialized,
    Failed,
    Cancelled,
};

using GenerationJobId = uint64_t;

struct GenerationPriorityScores final {
    uint32_t distance = 0U;
    uint32_t view = 0U;
    uint32_t movement = 0U;

    constexpr bool operator==(GenerationPriorityScores const&) const noexcept = default;
};

struct GenerationRegionKey final {
    uint64_t revision = 0U;
    uint64_t seed = 0U;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    uint32_t extent = 0U;
    GenerationStage stage = GenerationStage::Refinement;

    constexpr bool operator==(GenerationRegionKey const&) const noexcept = default;
};

struct WorldGenerationPlan final {
    bool pre_generate = false;
    std::vector<uint32_t> refinement_extents;
    std::vector<GenerationStage> chunk_stages;
    std::vector<GenerationPriorityDimension> priority_order{
        GenerationPriorityDimension::Distance,
        GenerationPriorityDimension::View,
        GenerationPriorityDimension::Movement,
    };
};

[[nodiscard]]
constexpr std::array<uint32_t, 5> registeredWorldGenerationExtents() noexcept
{
    return REGISTERED_WORLD_GENERATION_EXTENTS;
}

[[nodiscard]]
std::optional<GenerationRegionKey> generationRegionForChunk(
    ChunkCoordinate coordinate,
    uint32_t extent,
    GenerationStage stage,
    uint64_t revision,
    uint64_t seed
) noexcept;

[[nodiscard]]
bool isValidWorldGenerationPlan(WorldGenerationPlan const& plan) noexcept;

struct GenerationJob final {
    GenerationJobId id = 0U;
    ChunkCoordinate coordinate{};
    uint64_t revision = 0U;
    uint64_t seed = 0U;
    GenerationStage stage = GenerationStage::HeightTile;
    uint8_t retries = 0U;
    GenerationPriorityScores priority_scores{};
    std::optional<GenerationRegionKey> region;
    std::shared_ptr<std::vector<uint8_t> const> inherited_ancestor_data;

    bool operator==(GenerationJob const&) const noexcept = default;
};

struct GenerationResult final {
    GenerationJob job;
    bool succeeded = false;
    bool cancelled = false;
    std::shared_ptr<std::vector<uint8_t> const> output;

    bool operator==(GenerationResult const&) const noexcept = default;
};

class WorldGenerationScheduler final {
public:
    static constexpr uint8_t MAX_RETRIES = 1U;
    using JobOrder = std::function<bool(GenerationJob const&, GenerationJob const&)>;

    explicit WorldGenerationScheduler(
        uint64_t max_pending_jobs = 32U,
        uint64_t max_output_bytes = MAX_GENERATION_OUTPUT_BUDGET_BYTES
    );

    [[nodiscard]]
    GenerationAdmission submit(
        ChunkCoordinate coordinate,
        uint64_t revision,
        GenerationStage stage,
        GenerationPriorityScores priority_scores = {}
    );

    [[nodiscard]]
    std::optional<GenerationJob> takeNext() noexcept;

    [[nodiscard]]
    std::optional<GenerationJob> peekNext() const noexcept;

    [[nodiscard]]
    bool complete(
        GenerationJobId id,
        bool succeeded,
        bool cancelled = false,
        std::vector<uint8_t> output = {}
    );

    [[nodiscard]]
    bool retry(GenerationJobId id) noexcept;

    [[nodiscard]]
    bool cancel(GenerationJobId id) noexcept;
    void cancelQueued() noexcept;
    void cancelQueuedIf(std::function<bool(GenerationJob const&)> const& should_cancel);
    void reorderQueued(JobOrder const& order);

    [[nodiscard]]
    bool reorderQueued(std::span<GenerationPriorityDimension const> priority_order);

    void invalidateRevision(uint64_t revision) noexcept;

    [[nodiscard]]
    std::optional<GenerationResult> takeResult() noexcept;

    [[nodiscard]]
    uint64_t pendingCount() const noexcept;

    [[nodiscard]]
    uint64_t resultCount() const noexcept;

private:
    friend class WorldGenerationCoordinator;

    struct AdmissionResult final {
        GenerationAdmission admission;
        std::optional<GenerationJob> job;
    };

    struct JobState final {
        GenerationJob job;
        bool running = false;
        bool completed = false;
        bool succeeded = false;
        bool cancelled = false;
        bool stale = false;
        bool result_taken = false;
    };

    [[nodiscard]]
    AdmissionResult submitJob(
        ChunkCoordinate coordinate,
        uint64_t revision,
        uint64_t seed,
        GenerationStage stage,
        GenerationPriorityScores priority_scores = {},
        std::optional<GenerationRegionKey> region = std::nullopt,
        std::shared_ptr<std::vector<uint8_t> const> inherited_ancestor_data = nullptr
    );

private:
    uint64_t m_max_pending_jobs;
    uint64_t m_max_output_bytes;
    uint64_t m_output_bytes = 0U;
    GenerationJobId m_next_id = 1U;
    std::deque<GenerationJobId> m_queue;
    std::unordered_map<GenerationJobId, JobState> m_jobs;
    std::deque<GenerationResult> m_results;
};

class WorldGenerationCoordinator final {
public:
    static constexpr uint64_t DEFAULT_MAX_PENDING_JOBS = 32U;
    static constexpr uint64_t DEFAULT_WORLD_REVISION = 1U;
    static constexpr uint64_t DEFAULT_WORLD_SEED = 42U;

    explicit WorldGenerationCoordinator(
        uint64_t max_pending_jobs = DEFAULT_MAX_PENDING_JOBS,
        uint64_t revision = DEFAULT_WORLD_REVISION,
        uint64_t seed = DEFAULT_WORLD_SEED
    );

    [[nodiscard]]
    GenerationAdmission request(ChunkCoordinate coordinate, std::vector<GenerationStage> stages);

    [[nodiscard]]
    GenerationAdmission requestPlan(
        ChunkCoordinate coordinate,
        WorldGenerationPlan plan,
        GenerationPriorityScores priority_scores = {}
    );

    [[nodiscard]]
    std::optional<GenerationJob> takeNext() noexcept;

    [[nodiscard]]
    std::optional<GenerationJob> peekNext() const noexcept;

    [[nodiscard]]
    bool complete(
        GenerationJobId id,
        bool succeeded,
        bool cancelled = false,
        std::vector<uint8_t> output = {}
    );

    [[nodiscard]]
    std::optional<GenerationResult> takeResult() noexcept;

    [[nodiscard]]
    bool acceptResult(GenerationJobId id);

    [[nodiscard]]
    bool retry(GenerationJobId id) noexcept;

    [[nodiscard]]
    bool requestRetry(ChunkCoordinate coordinate, uint64_t revision, uint64_t seed) noexcept;

    [[nodiscard]]
    bool cancel(ChunkCoordinate coordinate, uint64_t revision, uint64_t seed) noexcept;

    void setWorldIdentity(uint64_t revision, uint64_t seed) noexcept;

    [[nodiscard]]
    GenerationState state(ChunkCoordinate coordinate, uint64_t revision, uint64_t seed) const noexcept;

    [[nodiscard]]
    uint64_t pendingCount() const noexcept;

    [[nodiscard]]
    uint64_t resultCount() const noexcept;

    [[nodiscard]]
    uint64_t refinementOutputBytes() const noexcept;

private:
    struct GenerationIdentity final {
        ChunkCoordinate coordinate;
        uint64_t revision;
        uint64_t seed;

        constexpr bool operator==(GenerationIdentity const&) const noexcept = default;
    };

    struct GenerationIdentityHash final {
        [[nodiscard]]
        uint64_t operator()(GenerationIdentity const& identity) const noexcept;
    };

    struct RefinementOutputHash final {
        [[nodiscard]]
        uint64_t operator()(GenerationRegionKey const& key) const noexcept;
    };

    struct PlanStage final {
        GenerationStage stage;
        std::optional<GenerationRegionKey> region;
    };

    struct RefinementOutput final {
        std::shared_ptr<std::vector<uint8_t> const> bytes;
        uint64_t last_access = 0U;
    };

    struct Pipeline final {
        std::vector<PlanStage> stages;
        GenerationPriorityScores priority_scores{};
        uint64_t stage_index = 0U;
        GenerationJobId active_job_id = 0U;
        GenerationState state = GenerationState::Unknown;
        bool result_ready = false;
        std::shared_ptr<std::vector<uint8_t> const> result_output;
        std::shared_ptr<std::vector<uint8_t> const> final_refinement_output;
    };

    [[nodiscard]]
    static bool isValidPlan(std::vector<GenerationStage> const& stages) noexcept;

    [[nodiscard]]
    GenerationAdmission submitNextStage(GenerationIdentity const& identity, Pipeline& pipeline);

    [[nodiscard]]
    std::shared_ptr<std::vector<uint8_t> const> nearestCompletedAncestor(
        GenerationRegionKey const& region
    ) noexcept;

    void retainRefinementOutput(
        GenerationRegionKey const& region,
        std::shared_ptr<std::vector<uint8_t> const> bytes
    );

    [[nodiscard]]
    Pipeline* pipelineFor(GenerationJobId id) noexcept;

    void pruneTerminalPipelines() noexcept;

private:
    WorldGenerationScheduler m_scheduler;
    uint64_t m_max_pending_jobs;
    uint64_t m_revision;
    uint64_t m_seed;
    uint64_t m_refinement_output_bytes = 0U;
    uint64_t m_refinement_access_clock = 0U;
    std::vector<GenerationPriorityDimension> m_priority_order{
        GenerationPriorityDimension::Distance,
        GenerationPriorityDimension::View,
        GenerationPriorityDimension::Movement,
    };
    bool m_priority_order_configured = false;
    std::unordered_map<GenerationIdentity, Pipeline, GenerationIdentityHash> m_pipelines;
    std::unordered_map<GenerationRegionKey, RefinementOutput, RefinementOutputHash> m_refinement_outputs;
};

} // namespace shared
