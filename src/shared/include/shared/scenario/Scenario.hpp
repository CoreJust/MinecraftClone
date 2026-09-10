#pragma once

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

} // namespace scenario_detail

enum class ScenarioProfile : uint8_t {
    Flat2dV1,
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
    uint8_t x;
    uint8_t y;
    ScenarioLocation location;
};

struct ScenarioInputOperation final {
    ScenarioActorId actor;
    int8_t x;
    int8_t y;
    uint64_t effective_boundary;
};

struct ScenarioWaitOperation final {
    uint64_t ticks;
};

struct ScenarioExpectPositionOperation final {
    ScenarioActorId actor;
    uint8_t x;
    uint8_t y;
};

using ScenarioOperationData = std::variant<
    ScenarioInputOperation,
    ScenarioWaitOperation,
    ScenarioExpectPositionOperation>;

struct ScenarioOperation final {
    ScenarioLocation location;
    uint64_t boundary;
    ScenarioOperationData data;
};

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
    ScenarioPlan(
        uint32_t version,
        ScenarioProfile profile,
        uint64_t seed,
        std::vector<ScenarioActor> actors,
        std::vector<ScenarioOperation> operations,
        uint64_t total_ticks,
        uint64_t evidence_count
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
};

[[nodiscard]]
std::expected<ScenarioPlan, ScenarioDiagnostic> parseScenario(
    std::string_view filename,
    std::string_view source,
    ScenarioLimits const& limits
);

} // namespace shared
