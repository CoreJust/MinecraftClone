#pragma once

#include <shared/world/Chunk.hpp>
#include <shared/world/World.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace shared {

using ScenarioActorId = uint32_t;

namespace scenario_detail {

class ScenarioParser;
class CoreLangScenarioLowerer;
class ScenarioPlanCollector;

} // namespace scenario_detail

enum class ScenarioProfile : uint8_t {
    Flat2dV1,
    Flat3dV1,
    Flight3dV1,
    SparseWorldV1,
};

[[nodiscard]]
std::string_view scenarioProfileName(ScenarioProfile const profile) noexcept;

struct ScenarioLimits final {
    uint64_t max_source_bytes;
    uint64_t max_statements;
    uint64_t max_actors;
    uint64_t max_total_ticks;
    uint64_t max_operations;
    uint64_t max_evidence;
    uint64_t max_sparse_world_resident_chunks = 128U;
};

struct ScenarioLocation final {
    uint32_t line;
    uint32_t column;
};

enum class ScenarioDiagnosticCode : uint8_t {
    InvalidLimits,
    SourceTooLarge,
    StatementLimitExceeded,
    ActorLimitExceeded,
    TickLimitExceeded,
    OperationLimitExceeded,
    EvidenceLimitExceeded,
    MalformedSyntax,
    UnsupportedVersion,
    UnsupportedProfile,
    DuplicateActor,
    UnknownActor,
    UnsupportedCommand,
    IntegerOverflow,
    InvalidInteger,
    InvalidRange,
    InvalidCharacter,
    MissingPlayer,
    UnknownSourceHeader,
    CoreLangCompileFailure,
    CoreLangRuntimeFailure,
    Cancelled,
};

class ScenarioCancellation {
public:
    virtual ~ScenarioCancellation() = default;

    [[nodiscard]]
    virtual bool isCancellationRequested() const noexcept = 0;
};

[[nodiscard]]
std::string_view scenarioDiagnosticCodeName(ScenarioDiagnosticCode const code) noexcept;

struct ScenarioDiagnostic final {
    ScenarioDiagnosticCode code;
    std::string filename;
    ScenarioLocation location;
    std::string message;
};

struct ScenarioActor final {
    ScenarioActorId id;
    std::string name;
    char character;
    int32_t x;
    int32_t y;
    int32_t z;
    int16_t yaw_degrees;
    int16_t pitch_degrees;
    int16_t roll_degrees;
    ScenarioLocation location;
};

struct ScenarioInputOperation final {
    ScenarioActorId actor;
    int8_t x;
    int8_t y;
    int8_t z = 0;
    uint64_t effective_boundary;
    enum class Intent : uint8_t {
        Direct,
        Flight,
        Phase,
    } intent = Intent::Direct;
};

struct ScenarioCameraInputOperation final {
    ScenarioActorId actor;
    int8_t strafe;
    int8_t forward;
    int8_t vertical = 0;
    uint64_t effective_boundary;
};

struct ScenarioWaitOperation final {
    uint64_t ticks;
};

struct ScenarioExpectPositionOperation final {
    ScenarioActorId actor;
    int32_t x;
    int32_t y;
    int32_t z;
};

struct ScenarioMovementPermissionsOperation final {
    bool flight;
    bool collision_bypass;
};

struct ScenarioJumpOperation final {
    ScenarioActorId actor;
};

struct ScenarioExpectMovementPermissionsOperation final {
    ScenarioActorId actor;
    bool flight;
    bool collision_bypass;
};

struct ScenarioExpectVerticalVelocityOperation final {
    ScenarioActorId actor;
    int32_t velocity_subcells;
};

struct ScenarioSparseWorldOptionsOperation final {
    uint32_t generator_version;
    uint64_t max_resident_chunks;
};

struct ScenarioExpectBlockOperation final {
    int64_t x;
    int64_t y;
    int64_t z;
    Block block;
};

struct ScenarioExpectResidentChunksOperation final {
    uint64_t count;
};

using ScenarioOperationData = std::variant<
    ScenarioInputOperation,
    ScenarioCameraInputOperation,
    ScenarioWaitOperation,
    ScenarioExpectPositionOperation,
    ScenarioMovementPermissionsOperation,
    ScenarioJumpOperation,
    ScenarioExpectMovementPermissionsOperation,
    ScenarioExpectVerticalVelocityOperation,
    ScenarioSparseWorldOptionsOperation,
    ScenarioExpectBlockOperation,
    ScenarioExpectResidentChunksOperation>;

struct ScenarioOperation final {
    ScenarioLocation location;
    uint64_t boundary;
    ScenarioOperationData data;
};

class ScenarioPlan;

struct ScenarioCameraDirectionInput final {
    int16_t yaw_degrees;
    int8_t strafe;
    int8_t forward;
    int8_t vertical = 0;
};

// This is the scenario-side form of the client camera controller.  A yaw of
// zero faces +Y; positive yaw turns toward +X.  It deliberately returns the
// normalized signed wire Direction so the server remains the sole authority.
[[nodiscard]]
Direction scenarioCameraRelativeDirection(ScenarioCameraDirectionInput const input) noexcept;

// Stable plan fingerprint recorded with runtime evidence.  It identifies the
// replay inputs and camera pose without including wall-clock measurements.
[[nodiscard]]
std::string scenarioReplayId(ScenarioPlan const& plan);

class ScenarioPlan final {
public:
    [[nodiscard]]
    uint32_t version() const noexcept;
    [[nodiscard]]
    ScenarioProfile profile() const noexcept;
    [[nodiscard]]
    uint64_t seed() const noexcept;
    [[nodiscard]]
    std::vector<ScenarioActor> const& actors() const noexcept;
    [[nodiscard]]
    std::vector<ScenarioOperation> const& operations() const noexcept;
    [[nodiscard]]
    uint64_t totalTicks() const noexcept;
    [[nodiscard]]
    uint64_t evidenceCount() const noexcept;

private:
    struct Counts final {
        uint64_t total_ticks;
        uint64_t evidence_count;
    };

    ScenarioPlan(
        uint32_t version,
        ScenarioProfile profile,
        uint64_t seed,
        std::vector<ScenarioActor> actors,
        std::vector<ScenarioOperation> operations,
        Counts counts
    );

private:
    uint32_t m_version;
    ScenarioProfile m_profile;
    uint64_t m_seed;
    std::vector<ScenarioActor> m_actors;
    std::vector<ScenarioOperation> m_operations;
    uint64_t m_total_ticks;
    uint64_t m_evidence_count;

    friend class scenario_detail::ScenarioParser;
    friend class scenario_detail::CoreLangScenarioLowerer;
    friend class scenario_detail::ScenarioPlanCollector;
};

[[nodiscard]]
std::expected<ScenarioPlan, ScenarioDiagnostic> parseScenario(
    std::string_view filename,
    std::string_view source,
    ScenarioLimits const& limits
);

// Selects the finite legacy frontend or the CoreLang frontend from its explicit
// source header. Both frontends lower to the same immutable ScenarioPlan.
[[nodiscard]]
std::expected<ScenarioPlan, ScenarioDiagnostic> parseScenarioSource(
    std::string_view filename,
    std::string_view source,
    ScenarioLimits const& limits,
    ScenarioCancellation const* cancellation = nullptr
);

} // namespace shared
