#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shared {

using PolicyEntityId = uint64_t;
using PolicyCapabilityKeyId = uint32_t;

struct PolicyCapabilityQuery final {
    PolicyEntityId subject{0U};
    PolicyCapabilityKeyId key{0U};
};

enum class PolicyEntityClass : uint8_t {
    Player,
    Mob,
    TechnicalEntity,
};

struct PolicySubject final {
    PolicyEntityId id{0U};
    PolicyEntityClass entity_class{PolicyEntityClass::Player};
    std::string kind;

    bool operator==(PolicySubject const&) const = default;
};

enum class PolicyRestriction : uint8_t {
    Maximum,
    Minimum,
};

struct PolicyCapabilityDefinition final {
    std::string key;
    int64_t default_value{0};
    int64_t minimum_value{0};
    int64_t maximum_value{0};
    PolicyRestriction hard_restriction{PolicyRestriction::Maximum};

    bool operator==(PolicyCapabilityDefinition const&) const = default;
};

struct PolicyCapabilityRegistry final {
    std::vector<PolicyCapabilityDefinition> definitions;
};

enum class PolicyGroupKind : uint8_t {
    All,
    Players,
    Mobs,
    TechnicalEntities,
    EntityKind,
    Explicit,
    Selector,
};

enum class PolicyJitMode : uint8_t {
    Required,
    Preferred,
    Disabled,
};

enum class PolicyDiagnosticCode : uint8_t {
    InvalidLimits,
    SourceTooLarge,
    StatementLimitExceeded,
    GroupLimitExceeded,
    MemberLimitExceeded,
    SelectorLimitExceeded,
    PresetLimitExceeded,
    RuleLimitExceeded,
    AssignmentLimitExceeded,
    InvalidDeclaration,
    DuplicateDeclaration,
    UnknownDeclaration,
    CompileFailure,
    RuntimeFailure,
    JitPreparationFailure,
    InterpreterParityFailure,
    Cancelled,
    InstructionLimitExceeded,
    UnboundedExecution,
    MaterializationMismatch,
    MissingMaterialization,
};

class PolicyCancellation {
public:
    virtual ~PolicyCancellation() = default;

    [[nodiscard]]
    virtual bool isCancellationRequested() const noexcept = 0;
};

struct PolicyLimits final {
    uint64_t max_source_bytes{uint64_t{64U} * 1024U};
    // Bounds total CoreLang VM steps across module initialization, policy entry, and parity runs.
    uint64_t max_instructions{uint64_t{16U} * 1024U};
    uint64_t max_statements{256U};
    uint64_t max_groups{64U};
    uint64_t max_members_per_group{256U};
    uint64_t max_selectors{64U};
    uint64_t max_presets{64U};
    uint64_t max_rules_per_preset{64U};
    uint64_t max_assignments{256U};
    uint64_t max_subjects{4'096U};
    uint64_t max_capabilities{64U};
};

[[nodiscard]]
PolicyLimits defaultPolicyLimits() noexcept;

struct PolicySelector final {
    uint32_t id{0U};
    std::string name;
    enum class Criterion : uint8_t {
        EntityKindIdRange,
    } criterion{Criterion::EntityKindIdRange};
    std::string entity_kind;
    // The v1 selector includes subjects of this kind with IDs in this range.
    PolicyEntityId first_id{0U};
    PolicyEntityId last_id{0U};
    uint64_t max_results{0U};

    bool operator==(PolicySelector const&) const = default;
};

struct PolicyGroup final {
    uint32_t id{0U};
    std::string name;
    PolicyGroupKind kind{PolicyGroupKind::All};
    std::string entity_kind;
    std::string selector;
    std::vector<PolicyEntityId> members;

    bool operator==(PolicyGroup const&) const = default;
};

struct PolicyRule final {
    std::string key;
    int64_t value{0};
    bool hard{false};

    bool operator==(PolicyRule const&) const = default;
};

struct PolicyPreset final {
    uint32_t id{0U};
    std::string name;
    std::vector<PolicyRule> rules;

    bool operator==(PolicyPreset const&) const = default;
};

struct PolicyAssignment final {
    std::string target;
    std::string preset;
    bool hard{false};

    bool operator==(PolicyAssignment const&) const = default;
};

class PolicyPlan final {
public:
    [[nodiscard]]
    std::string_view sourceId() const noexcept;

    [[nodiscard]]
    std::vector<PolicyGroup> const& groups() const noexcept;

    [[nodiscard]]
    std::vector<PolicySelector> const& selectors() const noexcept;

    [[nodiscard]]
    std::vector<PolicyPreset> const& presets() const noexcept;

    [[nodiscard]]
    std::vector<PolicyAssignment> const& assignments() const noexcept;

    [[nodiscard]]
    uint64_t statementCount() const noexcept;

    [[nodiscard]]
    PolicyPlan withoutSubject(PolicyEntityId subject) const;

private:
    PolicyPlan(
        std::string source_id,
        std::vector<PolicyGroup> groups,
        std::vector<PolicySelector> selectors,
        std::vector<PolicyPreset> presets,
        std::vector<PolicyAssignment> assignments,
        uint64_t statement_count
    );

    std::string m_source_id;
    std::vector<PolicyGroup> m_groups;
    std::vector<PolicySelector> m_selectors;
    std::vector<PolicyPreset> m_presets;
    std::vector<PolicyAssignment> m_assignments;
    uint64_t m_statement_count{0U};

    friend class PolicyHost;
    friend class PolicyCollector;
    friend bool operator==(PolicyPlan const& left, PolicyPlan const& right) noexcept;
};

[[nodiscard]]
bool operator==(PolicyPlan const& left, PolicyPlan const& right) noexcept;

[[nodiscard]]
std::string policyPlanId(PolicyPlan const& plan);

struct PolicyCompileOptions final {
    PolicyJitMode jit_mode{PolicyJitMode::Preferred};
    bool require_interpreter_parity{true};
};

struct PolicyDiagnostic final {
    PolicyDiagnosticCode code;
    std::string source_id;
    std::string message;
};

struct PolicyCompilation final {
    PolicyPlan plan;
    PolicyJitMode jit_mode{PolicyJitMode::Preferred};
    bool jit_prepared{false};
    bool interpreter_parity{false};
};

class PolicyCapabilitySnapshot final {
public:
    [[nodiscard]]
    uint64_t generation() const noexcept;

    [[nodiscard]]
    uint32_t capabilityCount() const noexcept;

    // This is the physics-facing query: an entity row lookup followed by a
    // dense capability index. Unknown subjects and keys deliberately fail
    // closed instead of causing a group/selector traversal on the tick path.
    [[nodiscard]]
    std::optional<int64_t> value(PolicyCapabilityQuery query) const noexcept;

    [[nodiscard]]
    bool allows(PolicyCapabilityQuery query) const noexcept;

private:
    uint64_t m_generation{0U};
    uint32_t m_capability_count{0U};
    std::optional<PolicyPlan> m_materialized_for;
    std::unordered_map<PolicyEntityId, uint32_t> m_rows;
    std::vector<int64_t> m_values;

    friend class PolicyHost;
};

class PolicySnapshot final {
public:
    [[nodiscard]]
    uint64_t generation() const noexcept;

    [[nodiscard]]
    PolicyPlan const& plan() const noexcept;

    [[nodiscard]]
    PolicyJitMode jitMode() const noexcept;

    [[nodiscard]]
    bool jitPrepared() const noexcept;

    [[nodiscard]]
    bool interpreterParity() const noexcept;

    [[nodiscard]]
    std::shared_ptr<PolicyCapabilitySnapshot const> capabilities() const noexcept;

private:
    PolicySnapshot(
        uint64_t generation,
        PolicyPlan plan,
        PolicyJitMode jit_mode,
        bool jit_prepared,
        bool interpreter_parity,
        std::shared_ptr<PolicyCapabilitySnapshot const> capabilities
    );

    uint64_t m_generation{0U};
    PolicyPlan m_plan;
    PolicyJitMode m_jit_mode{PolicyJitMode::Preferred};
    bool m_jit_prepared{false};
    bool m_interpreter_parity{false};
    std::shared_ptr<PolicyCapabilitySnapshot const> m_capabilities;

    friend class PolicyHost;
};

class PolicyHost final {
public:
    explicit PolicyHost(PolicyLimits limits = defaultPolicyLimits());

    [[nodiscard]]
    PolicyLimits const& limits() const noexcept;

    // Compilation is private and side-effect free. It performs all CoreLang
    // execution and JIT preparation before a candidate can be published.
    [[nodiscard]]
    std::expected<PolicyCompilation, PolicyDiagnostic> compile(
        std::string_view source_id,
        std::string_view source,
        PolicyCompileOptions options = {},
        PolicyCancellation const* cancellation = nullptr
    ) const;

    // Publication is one atomic snapshot replacement. A failed compilation
    // therefore cannot disturb the last authoritative policy.
    [[nodiscard]]
    std::expected<uint64_t, PolicyDiagnostic> publish(PolicyCompilation const& compilation);

    // Materialization resolves every selector and assignment before a
    // generation is published. The returned snapshot is immutable and may be
    // queried from physics without CoreLang execution or allocation.
    [[nodiscard]]
    std::expected<PolicyCapabilitySnapshot, PolicyDiagnostic> materialize(
        PolicyPlan const& plan,
        std::span<PolicySubject const> subjects,
        PolicyCapabilityRegistry const& registry
    ) const;

    [[nodiscard]]
    std::expected<uint64_t, PolicyDiagnostic> publish(
        PolicyCompilation compilation,
        PolicyCapabilitySnapshot capabilities
    );

    [[nodiscard]]
    std::shared_ptr<PolicySnapshot const> snapshot() const noexcept;

private:
    PolicyLimits m_limits;
    mutable std::mutex m_publish_mutex;
    std::shared_ptr<PolicySnapshot const> m_snapshot;
    uint64_t m_generation{0U};
};

[[nodiscard]]
std::string_view policyDiagnosticCodeName(PolicyDiagnosticCode code) noexcept;

} // namespace shared
