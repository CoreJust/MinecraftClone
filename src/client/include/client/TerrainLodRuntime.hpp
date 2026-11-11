#pragma once

#include <client/TerrainLodPresentation.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace client {

class VulkanRenderer;
class TerrainLodRuntimePlan;
struct TerrainLodUpdateDiagnostics;

struct TerrainLodColumnKey final {
    uint32_t x = 0U;
    uint32_t y = 0U;
    bool operator==(TerrainLodColumnKey const&) const = default;
};

// Retains pending capture order and duplicates, selecting exact rendered coordinate membership.
// The caller supplies an empty retired vector to receive the previous presentation.
void selectPresentedTerrainLodColumns(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> pending,
    std::span<TerrainLodColumnKey const> rendered,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& presented,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& retired);

// Selects the ready nearest frontier footprint alongside retained presented inputs.
bool selectTerrainLodFrontierColumns(
    TerrainLodRuntimePlan const& plan,
    std::span<std::shared_ptr<TerrainLodCapturedColumn const> const> captured,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& columns);

// Selects one scene transaction from current captures and the active scene's exact membership.
// Returns whether local frontier promotion changes that transaction.
bool selectTerrainLodSceneColumns(
    TerrainLodRuntimePlan const& plan,
    std::span<std::shared_ptr<TerrainLodCapturedColumn const> const> captured,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> presented,
    bool active,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& columns);

// Keeps the active mask intact and promotes ready neighbors within the supplied visual disk, if any.
bool selectTerrainLodLocalColumns(
    TerrainLodRuntimePlan const& plan,
    std::span<std::shared_ptr<TerrainLodCapturedColumn const> const> captured,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> presented,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& columns,
    std::vector<TerrainLodColumnCoordinate>& targets,
    std::span<TerrainLodColumnCoordinate const> edited = {},
    std::optional<TerrainLodColumnCoordinate> visual_center = {});
// Rebases only explicit local targets; far owns every other capture and retirement.
bool mergeTerrainLodLocalColumns(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> base,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> live,
    std::span<TerrainLodColumnCoordinate const> targets,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& merged);

// Captures each ready edited column independently. False keeps unavailable edits retryable.
bool captureEditedTerrainLodColumns(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> presented,
    shared::SparseWorld const& actual,
    TerrainLodAuthorityBatch& batch,
    std::vector<std::shared_ptr<TerrainLodColumnInput const>>& ready,
    TerrainLodUpdateDiagnostics* diagnostics = nullptr);

// A complete capture acknowledges only its captured revision after publication.
// Failed or partial work remains retryable even when no new edit arrives.
class TerrainLodEditCheckpoint final {
public:
    [[nodiscard]] bool needsCapture(uint64_t const revision) const noexcept { return revision != m_checked; }
    [[nodiscard]] bool canPublish() const noexcept { return m_pending && m_pending->complete; }

    void captured(uint64_t const revision, bool const complete) noexcept
    {
        m_pending = Capture{revision, complete};
    }

    void published() noexcept
    {
        if (m_pending && m_pending->complete) {
            m_checked = m_pending->revision;
        }
        discard();
    }

    void discard() noexcept { m_pending.reset(); }
    void reset() noexcept
    {
        m_checked = 0U;
        discard();
    }

private:
    struct Capture final {
        uint64_t revision;
        bool complete;
    };
    uint64_t m_checked = 0U;
    std::optional<Capture> m_pending;
};

struct TerrainLodWorkTicket final {
    uint64_t generation = 0U;
    uint32_t center_x = 0U;
    uint32_t center_y = 0U;
    std::vector<TerrainLodColumnKey> columns;
};

struct TerrainLodSourceIndexDiagnostics final {
    std::chrono::nanoseconds change_read_duration{0};
    std::chrono::nanoseconds resident_snapshot_duration{0};
    std::chrono::nanoseconds resident_diff_duration{0};
    std::chrono::nanoseconds dependency_selection_duration{0};
    uint64_t selection_count = 0U;
    uint64_t authoritative_change_count = 0U;
    uint64_t resident_snapshot_count = 0U;
    uint64_t resident_chunk_count = 0U;
    uint64_t resident_diff_change_count = 0U;
    uint64_t affected_column_count = 0U;
    uint64_t selected_column_count = 0U;
    uint64_t discovery_candidate_count = 0U;
    uint64_t full_resync_count = 0U;
};

struct TerrainLodUpdateDiagnostics final {
    bool enabled = false;
    std::chrono::nanoseconds report_duration{0};
    std::chrono::nanoseconds world_reset_duration{0};
    std::chrono::nanoseconds interest_prune_duration{0};
    std::chrono::nanoseconds worker_handoff_duration{0};
    std::chrono::nanoseconds worker_future_get_duration{0};
    std::chrono::nanoseconds worker_candidate_adopt_duration{0};
    std::chrono::nanoseconds worker_columns_copy_duration{0};
    std::chrono::nanoseconds edit_lane_duration{0};
    std::chrono::nanoseconds edit_detection_duration{0};
    std::chrono::nanoseconds edit_capture_duration{0};
    std::chrono::nanoseconds edit_renderer_replace_duration{0};
    std::chrono::nanoseconds pending_validation_duration{0};
    std::chrono::nanoseconds renderer_upsert_duration{0};
    std::chrono::nanoseconds renderer_plan_duration{0};
    std::chrono::nanoseconds renderer_buffer_update_duration{0};
    std::chrono::nanoseconds renderer_publish_duration{0};
    uint64_t renderer_uploaded_face_count = 0U;
    uint64_t prepared_draw_index_publish_count = 0U;
    uint64_t render_thread_draw_index_build_count = 0U;
    std::chrono::nanoseconds scene_handoff_duration{0};
    std::chrono::nanoseconds scene_activation_log_duration{0};
    std::chrono::nanoseconds scene_plan_presented_duration{0};
    std::chrono::nanoseconds scene_column_selection_duration{0};
    std::chrono::nanoseconds scene_pending_release_duration{0};
    std::chrono::nanoseconds source_refresh_duration{0};
    std::chrono::nanoseconds source_validation_duration{0};
    std::chrono::nanoseconds discovery_capture_duration{0};
    std::chrono::nanoseconds dispatch_preparation_duration{0};
    TerrainLodSourceIndexDiagnostics source_index;
    uint64_t source_refresh_count = 0U;
    uint64_t source_validation_count = 0U;
    uint64_t validated_column_count = 0U;
    uint64_t stale_column_count = 0U;
    uint64_t completed_job_count = 0U;
    uint64_t edit_lane_call_count = 0U;
    uint64_t edit_detection_pass_count = 0U;
    uint64_t edit_detection_column_count = 0U;
    uint64_t edit_capture_pass_count = 0U;
    uint64_t edit_renderer_replace_pass_count = 0U;
    uint64_t pending_validation_count = 0U;
    uint64_t pending_actual_column_count = 0U;
    uint64_t pending_chunk_source_count = 0U;
    uint64_t pending_authority_source_count = 0U;
    uint64_t source_rejection_count = 0U;
    uint64_t plan_rejection_count = 0U;
    uint64_t scene_handoff_attempt_count = 0U;
    uint64_t scene_handoff_complete_count = 0U;
    uint64_t discovery_capture_attempt_count = 0U;
    uint64_t discovery_capture_success_count = 0U;
    uint64_t dispatch_count = 0U;
    uint64_t dispatch_column_count = 0U;
};

enum class TerrainLodPublicationFailure : uint8_t {
    None,
    Generation,
    MissingCenter,
    MissingPresented,
    UncapturedColumn,
};

struct TerrainLodPublicationIssue final {
    TerrainLodPublicationFailure failure = TerrainLodPublicationFailure::None;
    TerrainLodColumnKey column;
};

class TerrainLodRuntimePlan final {
public:
    static constexpr uint32_t CURRENT_COLUMN_RADIUS = 41U;
    static constexpr uint32_t REQUIRED_CURRENT_COLUMN_COUNT =
        (2U * CURRENT_COLUMN_RADIUS + 1U) * (2U * CURRENT_COLUMN_RADIUS + 1U);

    void reset()
    {
        ++m_generation;
        m_columns.clear();
        m_presented.clear();
        m_discovery.clear();
        m_priority_discovery.clear();
        m_queued.clear();
        m_priority_queued.clear();
        m_selection_demand = true;
    }

    // One event-driven selection pass; unchanged idle frames allocate no membership containers.
    [[nodiscard]] bool takeSelectionDemand() noexcept
    {
        return std::exchange(m_selection_demand, false);
    }

    [[nodiscard]] bool selectionDemand() const noexcept { return m_selection_demand; }
    void acknowledgeSelectionDemand() noexcept { m_selection_demand = false; }
    void requestSelectionDemand() noexcept { m_selection_demand = true; }

    [[nodiscard]] bool relevant(TerrainLodColumnKey const key) const noexcept
    {
        if (key.x >= 4'096U || key.y >= 4'096U) {
            return false;
        }
        return distance(key.x, m_center_x / 16U) <= CURRENT_COLUMN_RADIUS
            && distance(key.y, m_center_y / 16U) <= CURRENT_COLUMN_RADIUS;
    }

    [[nodiscard]] bool visuallyRelevant(TerrainLodColumnKey const key) const noexcept
    {
        return terrainLodVisualColumn(key.x, key.y, m_center_x / 16U, m_center_y / 16U);
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    bool updateInterest(uint32_t const x, uint32_t const y)
    {
        bool const changed_column = m_center_x / 16U != x % 65'536U / 16U
            || m_center_y / 16U != y % 65'536U / 16U;
        m_center_x = x % 65'536U;
        m_center_y = y % 65'536U;
        if (changed_column) {
            TerrainLodColumnKey const center{m_center_x / 16U, m_center_y / 16U};
            // Movement changes capture urgency even when no new surface arrives.
            for (uint64_t const encoded : m_queued) {
                auto const key = decode(encoded);
                if (key == center) {
                    std::erase(m_priority_discovery, key);
                    m_priority_queued.insert(encoded);
                    m_priority_discovery.push_front(key);
                } else if (distance(key.x, center.x) <= 2U && distance(key.y, center.y) <= 2U
                    && m_priority_queued.insert(encoded).second) {
                    m_priority_discovery.push_back(key);
                }
            }
            std::erase_if(m_columns, [this](uint64_t const key) { return !relevant(decode(key)); });
            std::erase_if(m_presented, [this](uint64_t const key) { return !visuallyRelevant(decode(key)); });
        }
        if (changed_column) {
            m_selection_demand = true;
            orderDiscovery();
        }
        return changed_column;
    }

    void remember(TerrainLodColumnKey const key)
    {
        if (relevant(key)) {
            m_selection_demand = m_columns.insert(encode(key)).second || m_selection_demand;
        }
    }

    void forget(TerrainLodColumnKey const key)
    {
        m_selection_demand = m_columns.erase(encode(key)) != 0U || m_selection_demand;
    }

    void discover(std::span<TerrainLodColumnKey const> const available)
    {
        TerrainLodColumnKey const center{m_center_x / 16U, m_center_y / 16U};
        bool changed = false;
        for (auto const key : available) {
            uint64_t const encoded = encode(key);
            if (!relevant(key) || m_columns.contains(encoded)) {
                continue;
            }
            bool const inserted = m_queued.insert(encoded).second;
            changed = changed || inserted;
            bool const near = distance(key.x, center.x) <= 2U && distance(key.y, center.y) <= 2U;
            if (near && m_priority_queued.insert(encoded).second) {
                changed = true;
                if (key == center) {
                    m_priority_discovery.push_front(key);
                } else {
                    m_priority_discovery.push_back(key);
                }
            } else if (inserted) {
                m_discovery.push_back(key);
            }
        }
        if (changed) {
            orderDiscovery();
        }
    }

    [[nodiscard]] std::optional<TerrainLodColumnKey> nextDiscovery()
    {
        while (!m_discovery.empty() || !m_priority_discovery.empty()) {
            auto const squared_distance = [this](TerrainLodColumnKey const key) {
                return terrainLodVisualDistanceSquared(key.x, key.y, m_center_x / 16U, m_center_y / 16U);
            };
            bool const priority = !m_priority_discovery.empty()
                && (m_discovery.empty()
                    || squared_distance(m_priority_discovery.front()) <= squared_distance(m_discovery.front()));
            auto& queue = priority ? m_priority_discovery : m_discovery;
            auto const key = queue.front();
            queue.pop_front();
            if (priority) {
                m_priority_queued.erase(encode(key));
            } else {
                if (m_priority_queued.contains(encode(key))) {
                    continue;
                }
            }
            if (m_queued.erase(encode(key)) != 0U && visuallyRelevant(key)) {
                return key;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] TerrainLodColumnKey currentColumn() const noexcept
    {
        return {m_center_x / 16U, m_center_y / 16U};
    }

    [[nodiscard]] TerrainLodWorkTicket ticket() const
    {
        TerrainLodWorkTicket result{m_generation, m_center_x, m_center_y, {}};
        result.columns.reserve(m_columns.size());
        for (uint64_t const key : m_columns) {
            result.columns.push_back(decode(key));
        }
        return result;
    }

    [[nodiscard]] TerrainLodPublicationIssue
    publicationIssue(TerrainLodWorkTicket const& ticket, std::span<TerrainLodColumnKey const> const rendered) const
    {
        if (ticket.generation != m_generation) {
            return {TerrainLodPublicationFailure::Generation, {}};
        }
        std::unordered_set<uint64_t> included;
        included.reserve(rendered.size());
        for (auto const key : rendered) {
            included.insert(encode(key));
        }
        uint64_t const center = encode({m_center_x / 16U, m_center_y / 16U});
        if (m_columns.contains(center) && !included.contains(center)) {
            return {TerrainLodPublicationFailure::MissingCenter, decode(center)};
        }
        for (uint64_t const key : m_presented) {
            if (m_columns.contains(key) && !included.contains(key)) {
                return {TerrainLodPublicationFailure::MissingPresented, decode(key)};
            }
        }
        std::unordered_set<uint64_t> captured;
        captured.reserve(ticket.columns.size());
        for (auto const key : ticket.columns) {
            captured.insert(encode(key));
        }
        for (uint64_t const key : included) {
            if (!captured.contains(key)) {
                return {TerrainLodPublicationFailure::UncapturedColumn, decode(key)};
            }
        }
        return {};
    }

    [[nodiscard]] bool
    canPublish(TerrainLodWorkTicket const& ticket, std::span<TerrainLodColumnKey const> const rendered) const
    {
        return publicationIssue(ticket, rendered).failure == TerrainLodPublicationFailure::None;
    }

    void presented(std::span<TerrainLodColumnKey const> const rendered)
    {
        // A completed transaction can expose another already-captured frontier input.
        m_selection_demand = true;
        m_presented.clear();
        m_presented.reserve(rendered.size());
        for (auto const key : rendered) {
            if (visuallyRelevant(key)) {
                m_presented.insert(encode(key));
            }
        }
    }

private:
    void orderDiscovery()
    {
        auto const less = [this](TerrainLodColumnKey const left, TerrainLodColumnKey const right) {
            uint32_t const a = terrainLodVisualDistanceSquared(left.x, left.y, m_center_x / 16U, m_center_y / 16U);
            uint32_t const b = terrainLodVisualDistanceSquared(right.x, right.y, m_center_x / 16U, m_center_y / 16U);
            return a != b ? a < b : encode(left) < encode(right);
        };
        std::ranges::sort(m_discovery, less);
        std::ranges::sort(m_priority_discovery, less);
    }

    [[nodiscard]] static uint32_t distance(uint32_t const a, uint32_t const b) noexcept
    {
        uint32_t const delta = a > b ? a - b : b - a;
        return std::min(delta, 4'096U - delta);
    }
    [[nodiscard]] static uint64_t encode(TerrainLodColumnKey const key) noexcept
    {
        return (static_cast<uint64_t>(key.x) << 32U) | key.y;
    }
    [[nodiscard]] static TerrainLodColumnKey decode(uint64_t const key) noexcept
    {
        return {static_cast<uint32_t>(key >> 32U), static_cast<uint32_t>(key)};
    }
    bool m_selection_demand = true;
    uint64_t m_generation = 1U;
    uint32_t m_center_x = 0U;
    uint32_t m_center_y = 0U;
    std::unordered_set<uint64_t> m_columns;
    std::unordered_set<uint64_t> m_presented;
    std::deque<TerrainLodColumnKey> m_discovery;
    std::deque<TerrainLodColumnKey> m_priority_discovery;
    std::unordered_set<uint64_t> m_priority_queued;
    std::unordered_set<uint64_t> m_queued;
};

// Selects immutable captures for the existing exact validator; it does not cache validation results.
class TerrainLodSourceIndex final {
public:
    struct Selection final {
        std::vector<std::shared_ptr<TerrainLodCapturedColumn const>> columns;
        std::vector<TerrainLodColumnKey> discovery;
        bool full_resync = false;
    };

    void clear();
    void upsert(std::shared_ptr<TerrainLodCapturedColumn const> column);
    void remove(TerrainLodColumnKey key);
    [[nodiscard]] std::shared_ptr<TerrainLodCapturedColumn const> find(TerrainLodColumnKey key) const;
    void select(
        shared::SparseWorld const& world,
        bool scan_residents,
        Selection& output,
        TerrainLodSourceIndexDiagnostics* diagnostics = nullptr
    );

private:
    struct Entry final {
        std::shared_ptr<TerrainLodCapturedColumn const> column;
        std::vector<uint64_t> dependencies;
    };
    static uint64_t key(uint32_t x, uint32_t y) noexcept;
    std::unordered_map<uint64_t, Entry> m_columns;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> m_dependents;
    shared::AuthoritativeColumnChanges m_changes;
    std::vector<shared::ResidentChunkContent> m_residents;
    std::vector<shared::ResidentChunkContent> m_previous_residents;
    uint64_t m_cursor = 0U;
    bool m_initialized = false;
};

// Membership follows published exact geometry, independently of discovery captures.
// Dirty keys survive capture/upload failures until a current capture is published.
class TerrainLodPresentedEditIndex final {
public:
    void reset();
    // The caller has source-fenced these published captures against world.
    void replace(
        std::span<std::shared_ptr<TerrainLodColumnInput const> const> presented,
        shared::SparseWorld const& world);
    void upsert(std::shared_ptr<TerrainLodColumnInput const> column);
    void select(
        shared::SparseWorld const& world,
        std::vector<std::shared_ptr<TerrainLodColumnInput const>>& edited,
        TerrainLodUpdateDiagnostics* diagnostics = nullptr);

private:
    struct Entry final {
        std::shared_ptr<TerrainLodColumnInput const> column;
        std::vector<uint64_t> dependencies;
    };
    static uint64_t key(uint32_t x, uint32_t y) noexcept;
    void remove(uint64_t column_key);
    std::unordered_map<uint64_t, Entry> m_columns;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> m_dependents;
    std::unordered_set<uint64_t> m_dirty;
    shared::AuthoritativeColumnChanges m_changes;
    uint64_t m_cursor = 0U;
    uint64_t m_sparse_revision = 0U;
    bool m_initialized = false;
};

enum class TerrainLodRuntimeFailure : uint8_t { None, Allocation, Preparation };

// Render-thread snapshot; job wall time includes time waiting for collection.
struct TerrainLodRuntimeProgress final {
    uint64_t dispatched = 0U;
    uint64_t completed = 0U;
    uint64_t failed = 0U;
    uint64_t captured = 0U;
    uint64_t published = 0U;
    uint64_t source_rejected = 0U;
    uint64_t plan_rejected = 0U;
    uint64_t source_changes = 0U;
    uint64_t actual_world_changes = 0U;
    uint64_t job_wall_ms = 0U;
    bool job_outstanding = false;
    bool job_ready = false;
    bool candidate_pending = false;
    TerrainLodRuntimeFailure last_error = TerrainLodRuntimeFailure::None;
    std::array<char, 128> last_error_text{};
    TerrainLodPublicationFailure last_plan_failure = TerrainLodPublicationFailure::None;
};

class TerrainLodRuntime final {
public:
    ~TerrainLodRuntime();
    void update(
        std::shared_ptr<shared::TerrainWorld const> source,
        shared::SparseWorld const* actual,
        std::optional<shared::Player> const& player,
        VulkanRenderer& renderer,
        std::chrono::steady_clock::time_point deadline,
        std::optional<shared::TerrainSessionIdentity> session = std::nullopt,
        std::optional<uint64_t> coverage_epoch = std::nullopt,
        bool collect_diagnostics = false,
        std::shared_ptr<shared::TerrainLodArtifact const> prepared_root = {}
    );
    void reset(VulkanRenderer* renderer = nullptr);
    [[nodiscard]] bool active() const noexcept { return m_active; }
    [[nodiscard]] TerrainLodRuntimeProgress progress() const noexcept;
    [[nodiscard]] TerrainLodUpdateDiagnostics const& lastUpdateDiagnostics() const noexcept
    {
        return m_last_update_diagnostics;
    }

private:
    friend struct TerrainLodRuntimeTestAccess;
    struct JobResult final {
        std::shared_ptr<TerrainLodScene const> scene;
        std::vector<std::shared_ptr<TerrainLodColumnInput const>> columns;
    };
    enum class Recenter : uint8_t { None, CurrentFirst, Ahead, Settle };
    static constexpr bool isTransientRecenter(Recenter mode)
    {
        return mode == Recenter::CurrentFirst || mode == Recenter::Ahead;
    }
    struct LocalJobResult final {
        TerrainLodLocalPreparation preparation;
        std::vector<std::shared_ptr<TerrainLodColumnInput const>> columns;
        std::shared_ptr<TerrainLodScene const> base;
        TerrainLodWorkTicket ticket;
        bool far = false;
        Recenter recenter = Recenter::None;
        uint64_t trace_id = 0U;
        uint64_t travel_generation = 0U;
        std::vector<TerrainLodColumnCoordinate> targets;
    };
    void startLocalJob(
        std::shared_ptr<shared::TerrainWorld const> source,
        std::shared_ptr<TerrainLodScene const> base,
        std::vector<std::shared_ptr<TerrainLodColumnInput const>> base_columns,
        std::vector<std::shared_ptr<TerrainLodColumnInput const>> columns,
        std::vector<TerrainLodColumnCoordinate> targets,
        std::vector<std::shared_ptr<TerrainLodLocalUnit const>> constraints,
        TerrainLodWorkTicket ticket,
        bool far,
        Recenter recenter = Recenter::None);
    void prepareFarHandoff();
    [[nodiscard]] std::shared_ptr<TerrainLodColumnInput const> validatedSettleForwardInput(
        shared::SparseWorld const& actual, std::string_view action);
    void traceCurrentCapture(
        TerrainLodColumnKey key,
        bool discovery,
        bool accepted,
        int64_t elapsed_us,
        uint64_t join_epoch,
        uint64_t coverage_epoch);
    std::future<LocalJobResult> m_local_job;
    Recenter m_local_job_recenter = Recenter::None;
    uint64_t m_local_job_trace_sequence = 0U;
    uint64_t m_local_job_trace_id = 0U;
    bool m_local_job_far = false;
    bool m_local_job_started_with_far_ready = false;
    bool m_local_job_targets_current = false;
    std::stop_source m_local_cancellation;
    std::optional<JobResult> m_far_ready;
    TerrainLodWorkTicket m_far_ticket;
    std::vector<std::shared_ptr<TerrainLodLocalUnit const>> m_local_units;
    std::vector<TerrainLodColumnCoordinate> m_local_edit_targets;
    std::shared_ptr<TerrainLodLocalUnit const> m_pending_unit;
    std::weak_ptr<TerrainLodScene const> m_pending_base;
    bool m_pending_far = false;
    Recenter m_pending_recenter = Recenter::None;
    uint64_t m_pending_travel_generation = 0U;
    std::vector<TerrainLodColumnCoordinate> m_pending_transient_targets;
    struct ColumnTravel final {
        shared::PlayerId player;
        TerrainLodColumnKey current;
        std::optional<TerrainLodColumnKey> forward;
        uint64_t generation;
        bool ahead_dispatched = false;
        struct Probe final {
            std::weak_ptr<TerrainLodCapturedColumn const> capture;
            std::string_view verdict;
        };
        std::optional<Probe> probe{};
    };
    std::optional<ColumnTravel> m_travel;
    uint64_t m_travel_generation = 0U;
    bool m_recenter_needs_settle = false;
    std::unordered_set<uint64_t> m_recenter_base_columns;
    void reportDiagnostics();
    void refreshColumnSources(shared::SparseWorld const& actual, bool scan_residents, bool rediscover);
    void updateEditedColumns(
        shared::SparseWorld const& actual,
        VulkanRenderer& renderer,
        std::chrono::steady_clock::time_point deadline,
        uint64_t join_epoch,
        uint64_t coverage_epoch,
        TerrainLodUpdateDiagnostics* diagnostics);
    std::shared_ptr<TerrainLodScene const> m_presented_scene;
    std::future<std::shared_ptr<TerrainLodScene const>> m_patch_job;
    std::shared_ptr<TerrainLodScene const> m_patch;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> m_patch_columns;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> m_presented_columns;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> m_retired_presented_columns;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> m_pending_columns;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> m_retired_job_columns;
    TerrainLodEditCheckpoint m_edit_checkpoint;
    TerrainLodPresentedEditIndex m_presented_edit_index;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> m_edited_columns;
    uint64_t m_presented_join_epoch = 0U;
    uint64_t m_presented_coverage_epoch = 0U;
    std::chrono::steady_clock::time_point m_diagnostic_reported{};
    std::chrono::steady_clock::time_point m_job_started{};
    std::chrono::microseconds m_last_job_duration{0};
    uint32_t m_diagnostic_records = 0U;
    uint64_t m_dispatched = 0U;
    uint64_t m_failed = 0U;
    uint64_t m_source_changes = 0U;
    uint64_t m_actual_world_changes = 0U;
    TerrainLodRuntimeFailure m_last_error = TerrainLodRuntimeFailure::None;
    std::array<char, 128> m_last_error_text{};
    uint64_t m_completed = 0U;
    uint64_t m_source_rejected = 0U;
    uint64_t m_plan_rejected = 0U;
    std::array<uint64_t, 5> m_plan_failure_counts{};
    uint64_t m_published = 0U;
    uint64_t m_captured = 0U;
    uint64_t m_diagnostic_events = 0U;
    uint64_t m_last_reported_events = 0U;
    TerrainLodPublicationIssue m_last_publication_issue;
    shared::SparseWorld const* m_actual_world = nullptr;
    std::shared_ptr<shared::TerrainWorld const> m_world;
    std::future<JobResult> m_job;
    std::shared_ptr<shared::TerrainWorld const> m_job_world;
    std::shared_ptr<TerrainLodScene const> m_pending;
    std::stop_source m_cancellation;
    std::vector<std::shared_ptr<TerrainLodCapturedColumn const>> m_columns;
    TerrainLodAuthorityBatch m_columns_authority_batch;
    TerrainLodUpdateDiagnostics m_last_update_diagnostics;
    uint32_t m_center_x = 0U;
    uint32_t m_center_y = 0U;
    TerrainLodSourceIndex m_source_index;
    TerrainLodSourceIndex::Selection m_source_selection;
    uint64_t m_discovery_revision = 0U;
    bool m_discovery_initialized = false;
    TerrainLodRuntimePlan m_plan;
    TerrainLodWorkTicket m_job_ticket;
    TerrainLodWorkTicket m_pending_ticket;
    bool m_active = false;
    bool m_actual_reported = false;
    bool m_dirty = true;
    bool m_selection_trace_initialized = false;
    bool m_selection_trace_demand = false;
    bool m_selection_trace_admitted = false;
    uint8_t m_selection_trace_blocked_reason = 0U;
    TerrainLodColumnKey m_selection_trace_key{};
    bool m_current_capture_trace_initialized = false;
    bool m_current_capture_trace_discovery = false;
    bool m_current_capture_trace_accepted = false;
    uint64_t m_current_capture_trace_join_epoch = 0U;
    uint64_t m_current_capture_trace_coverage_epoch = 0U;
    TerrainLodColumnKey m_current_capture_trace_key{};
};

} // namespace client
