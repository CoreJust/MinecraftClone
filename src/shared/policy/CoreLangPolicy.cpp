#include <shared/policy/Policy.hpp>

#include <core/lang/CoreLang.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace shared {

namespace {

enum class HostCall : uint8_t {
    Group,
    Member,
    Selector,
    Preset,
    Assignment,
};

[[nodiscard]]
PolicyDiagnostic diagnostic(
    PolicyDiagnosticCode const code,
    std::string_view const source_id,
    std::string message
)
{
    return {
        .code = code,
        .source_id = std::string{source_id},
        .message = std::move(message),
    };
}

[[nodiscard]]
core::lang::Type type(core::lang::TypeKind const kind)
{
    return {
        .kind = kind,
        .elements = {},
        .method_owner_name = {},
        .callable_precondition = {},
        .callable_postcondition = {},
        .struct_definition = {},
        .recursive_struct_definition = {},
    };
}

[[nodiscard]]
core::lang::Value unitValue()
{
    return {
        .type = type(core::lang::TypeKind::Unit),
        .bytes = {},
        .elements = {},
        .literal_lease = {},
        .runtime_lease = {},
    };
}

[[nodiscard]]
std::expected<void, std::string> count(
    std::span<core::lang::Value const> const arguments,
    uint64_t const expected
)
{
    if (arguments.size() != expected) {
        return std::unexpected("policy host call received the wrong number of arguments");
    }
    return {};
}

[[nodiscard]]
std::expected<uint64_t, std::string> unsignedValue(
    core::lang::Value const& value,
    core::lang::TypeKind const expected,
    uint8_t const byte_count
)
{
    if (value.type != type(expected) || value.bytes.size() != byte_count || !value.elements.empty()) {
        return std::unexpected("policy host call received an invalid unsigned integer");
    }
    uint64_t result = 0U;
    for (uint8_t index = 0U; index < byte_count; ++index) {
        result |= static_cast<uint64_t>(value.bytes[index]) << (index * 8U);
    }
    return result;
}

[[nodiscard]]
std::expected<int64_t, std::string> signedValue(
    core::lang::Value const& value,
    core::lang::TypeKind const expected,
    uint8_t const byte_count
)
{
    auto const raw = unsignedValue(value, expected, byte_count);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return std::bit_cast<int64_t>(*raw);
}

[[nodiscard]]
std::expected<std::string, std::string> textValue(core::lang::Value const& value)
{
    if (value.type != type(core::lang::TypeKind::Str) || !value.elements.empty()) {
        return std::unexpected("policy host call received an invalid text value");
    }
    std::string result;
    result.reserve(value.bytes.size());
    for (uint8_t const byte : value.bytes) {
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

[[nodiscard]]
std::expected<bool, std::string> boolValue(core::lang::Value const& value)
{
    auto const result = unsignedValue(value, core::lang::TypeKind::U8, 1U);
    if (!result || *result > 1U) {
        return std::unexpected("policy boolean must be 0u8 or 1u8");
    }
    return *result == 1U;
}

[[nodiscard]]
bool validText(std::string_view const text) noexcept
{
    if (text.empty() || text.size() > 256U) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char const character) {
        return character >= 0x21 && character <= 0x7e;
    });
}

[[nodiscard]]
bool validCapabilityKey(std::string_view const key) noexcept
{
    size_t const separator = key.find(':');
    if (separator == 0U || separator == std::string_view::npos || separator + 1U >= key.size()
        || key.find(':', separator + 1U) != std::string_view::npos) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](char const character) {
        return (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == ':' || character == '.' || character == '_' || character == '-'
            || character == '/';
    });
}

[[nodiscard]]
bool isBuiltInGroupName(std::string_view const name) noexcept
{
    return name == "all" || name == "players" || name == "mobs" || name == "entities";
}

[[nodiscard]]
bool validEntityClass(PolicyEntityClass const entity_class) noexcept
{
    switch (entity_class) {
        case PolicyEntityClass::Player:
        case PolicyEntityClass::Mob:
        case PolicyEntityClass::TechnicalEntity: return true;
    }
    return false;
}

[[nodiscard]]
bool isReservedWorldTarget(std::string_view const target) noexcept
{
    return target == "world" || target.starts_with("world:");
}

[[nodiscard]]
std::optional<PolicyEntityId> entityTargetId(std::string_view const target) noexcept
{
    static constexpr std::string_view PREFIX{"entity:"};
    if (!target.starts_with(PREFIX)) {
        return std::nullopt;
    }
    PolicyEntityId result{0U};
    auto const conversion = std::from_chars(
        target.data() + PREFIX.size(),
        target.data() + target.size(),
        result
    );
    if (conversion.ec != std::errc{} || conversion.ptr != target.data() + target.size()) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]]
std::optional<std::string_view> entityKindTarget(std::string_view const target) noexcept
{
    static constexpr std::string_view PREFIX{"kind:"};
    if (!target.starts_with(PREFIX)) {
        return std::nullopt;
    }
    std::string_view kind = target;
    kind.remove_prefix(PREFIX.size());
    return validText(kind) ? std::optional<std::string_view>{kind} : std::nullopt;
}

[[nodiscard]]
uint8_t entityClassSpecificity(
    std::string_view const target,
    PolicyEntityClass const entity_class
) noexcept
{
    if (target == "all") {
        return 1U;
    }
    if ((target == "players" && entity_class == PolicyEntityClass::Player)
        || (target == "mobs" && entity_class == PolicyEntityClass::Mob)
        || (target == "entities" && entity_class == PolicyEntityClass::TechnicalEntity)) {
        return 2U;
    }
    return 0U;
}

[[nodiscard]]
PolicyDiagnosticCode collectorFailureCode(std::string_view const message) noexcept
{
    if (message.find("statement limit") != std::string_view::npos) {
        return PolicyDiagnosticCode::StatementLimitExceeded;
    }
    if (message.find("group limit") != std::string_view::npos) {
        return PolicyDiagnosticCode::GroupLimitExceeded;
    }
    if (message.find("member limit") != std::string_view::npos) {
        return PolicyDiagnosticCode::MemberLimitExceeded;
    }
    if (message.find("selector limit") != std::string_view::npos) {
        return PolicyDiagnosticCode::SelectorLimitExceeded;
    }
    if (message.find("preset limit") != std::string_view::npos) {
        return PolicyDiagnosticCode::PresetLimitExceeded;
    }
    if (message.find("rule limit") != std::string_view::npos) {
        return PolicyDiagnosticCode::RuleLimitExceeded;
    }
    if (message.find("assignment limit") != std::string_view::npos) {
        return PolicyDiagnosticCode::AssignmentLimitExceeded;
    }
    if (message.find("cancel") != std::string_view::npos) {
        return PolicyDiagnosticCode::Cancelled;
    }
    return PolicyDiagnosticCode::InvalidDeclaration;
}

[[nodiscard]]
core::lang::JitMode coreJitMode(PolicyJitMode const mode) noexcept
{
    switch (mode) {
        case PolicyJitMode::Required: return core::lang::JitMode::Required;
        case PolicyJitMode::Preferred: return core::lang::JitMode::Preferred;
        case PolicyJitMode::Disabled: return core::lang::JitMode::Disabled;
    }
    return core::lang::JitMode::Disabled;
}

struct HostSpec final {
    std::string_view name;
    HostCall call;
    std::vector<core::lang::Type> arguments;
};

[[nodiscard]]
std::vector<HostSpec> hostSpecs()
{
    using TypeKind = core::lang::TypeKind;
    return {
        {"policyGroup", HostCall::Group, {
            type(TypeKind::Str), type(TypeKind::U8), type(TypeKind::Str),
        }},
        {"policyMember", HostCall::Member, {
            type(TypeKind::Str), type(TypeKind::U64),
        }},
        {"policySelector", HostCall::Selector, {
            type(TypeKind::Str), type(TypeKind::Str), type(TypeKind::U64),
            type(TypeKind::U64), type(TypeKind::U64),
        }},
        {"policyRule", HostCall::Preset, {
            type(TypeKind::Str), type(TypeKind::Str), type(TypeKind::I64), type(TypeKind::U8),
        }},
        {"policyAssign", HostCall::Assignment, {
            type(TypeKind::Str), type(TypeKind::Str), type(TypeKind::U8),
        }},
    };
}

[[nodiscard]]
core::lang::CustomManifest manifest(HostSpec const& spec)
{
    core::lang::CustomManifest result{
        .provider_key = "minecraft.policy." + std::string{spec.name},
        .abi_major = 1U,
        .effect = core::lang::CustomEffect::Observable,
        .arguments = spec.arguments,
        .result = type(core::lang::TypeKind::Unit),
        .borrow = {
            .parameters = std::vector<core::lang::BorrowAccess>(spec.arguments.size()),
            .returns = {},
        },
    };
    result.digest = core::lang::customManifestDigest(result);
    return result;
}

} // namespace

class PolicyCollector final {
public:
    PolicyCollector(
        std::string_view const source_id,
        PolicyLimits const& limits,
        PolicyCancellation const* const cancellation
    )
        : m_source_id(source_id)
        , m_limits(limits)
        , m_cancellation(cancellation)
    {
    }

    [[nodiscard]]
    std::expected<void, std::string> call(
        HostCall const host_call,
        std::span<core::lang::Value const> const arguments
    )
    {
        if (isCancelled()) {
            return std::unexpected("policy compilation was cancelled");
        }
        if (m_statements >= m_limits.max_statements) {
            return std::unexpected("policy statement limit exceeded");
        }
        ++m_statements;
        switch (host_call) {
            case HostCall::Group: return group(arguments);
            case HostCall::Member: return member(arguments);
            case HostCall::Selector: return selector(arguments);
            case HostCall::Preset: return preset(arguments);
            case HostCall::Assignment: return assignment(arguments);
        }
        return std::unexpected("unknown policy host call");
    }

    [[nodiscard]]
    std::expected<PolicyPlan, std::string> build() &&
    {
        if (isCancelled()) {
            return std::unexpected("policy compilation was cancelled");
        }
        if (m_statements == 0U) {
            return std::unexpected("policy must contain at least one declaration");
        }
        return PolicyPlan{
            std::move(m_source_id),
            std::move(m_groups),
            std::move(m_selectors),
            std::move(m_presets),
            std::move(m_assignments),
            m_statements,
        };
    }

private:
    [[nodiscard]]
    std::expected<void, std::string> group(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 3U); !valid_count) {
            return valid_count;
        }
        auto name = textValue(arguments[0]);
        auto kind = unsignedValue(arguments[1], core::lang::TypeKind::U8, 1U);
        auto scope_name = textValue(arguments[2]);
        if (!name || !kind || !scope_name || !validText(*name) || *kind > static_cast<uint64_t>(PolicyGroupKind::Selector)) {
            return std::unexpected("policyGroup received an invalid typed declaration");
        }
        PolicyGroupKind const group_kind = static_cast<PolicyGroupKind>(*kind);
        if (isBuiltInGroupName(*name)) {
            return std::unexpected("policyGroup cannot redefine a built-in group");
        }
        if (isReservedWorldTarget(*name)) {
            return std::unexpected("policyGroup cannot use the reserved world subject namespace");
        }
        if (entityTargetId(*name).has_value() || name->starts_with("kind:")) {
            return std::unexpected("policyGroup cannot use a built-in entity target name");
        }
        if (group_kind != PolicyGroupKind::EntityKind
            && group_kind != PolicyGroupKind::Explicit
            && group_kind != PolicyGroupKind::Selector) {
            return std::unexpected("custom policy groups must use an entity kind, explicit membership, or selector");
        }
        if (!scope_name->empty()
            && group_kind != PolicyGroupKind::Selector
            && group_kind != PolicyGroupKind::EntityKind) {
            return std::unexpected("only selector and entity-kind groups may name a scope");
        }
        if (std::ranges::any_of(m_groups, [&name](PolicyGroup const& group) { return group.name == *name; })) {
            return std::unexpected("duplicate policy group");
        }
        if (m_groups.size() >= m_limits.max_groups) {
            return std::unexpected("policy group limit exceeded");
        }
        m_groups.push_back({
            .id = static_cast<uint32_t>(m_groups.size()),
            .name = std::move(*name),
            .kind = group_kind,
            .entity_kind = group_kind == PolicyGroupKind::EntityKind ? *scope_name : std::string{},
            .selector = group_kind == PolicyGroupKind::Selector ? std::move(*scope_name) : std::string{},
            .members = {},
        });
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> member(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 2U); !valid_count) {
            return valid_count;
        }
        auto group_name = textValue(arguments[0]);
        auto entity = unsignedValue(arguments[1], core::lang::TypeKind::U64, 8U);
        if (!group_name || !entity || !validText(*group_name)) {
            return std::unexpected("policyMember received an invalid typed declaration");
        }
        auto const found = std::ranges::find(m_groups, *group_name, &PolicyGroup::name);
        if (found == m_groups.end()) {
            return std::unexpected("policyMember references an unknown group");
        }
        if (found->kind != PolicyGroupKind::Explicit) {
            return std::unexpected("policyMember requires an explicit group");
        }
        if (found->members.size() >= m_limits.max_members_per_group) {
            return std::unexpected("policy member limit exceeded");
        }
        if (std::ranges::find(found->members, *entity) != found->members.end()) {
            return std::unexpected("duplicate policy group member");
        }
        found->members.push_back(*entity);
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> selector(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 5U); !valid_count) {
            return valid_count;
        }
        auto name = textValue(arguments[0]);
        auto entity_kind = textValue(arguments[1]);
        auto first_id = unsignedValue(arguments[2], core::lang::TypeKind::U64, 8U);
        auto last_id = unsignedValue(arguments[3], core::lang::TypeKind::U64, 8U);
        auto max_results = unsignedValue(arguments[4], core::lang::TypeKind::U64, 8U);
        if (!name || !entity_kind || !first_id || !last_id || !max_results
            || !validText(*name) || !validText(*entity_kind) || *first_id > *last_id || *max_results == 0U) {
            return std::unexpected("policySelector received an invalid typed declaration");
        }
        if (std::ranges::any_of(m_selectors, [&name](PolicySelector const& selector) {
                return selector.name == *name;
            })) {
            return std::unexpected("duplicate policy selector");
        }
        if (m_selectors.size() >= m_limits.max_selectors) {
            return std::unexpected("policy selector limit exceeded");
        }
        m_selectors.push_back({
            .id = static_cast<uint32_t>(m_selectors.size()),
            .name = std::move(*name),
            .criterion = PolicySelector::Criterion::EntityKindIdRange,
            .entity_kind = std::move(*entity_kind),
            .first_id = *first_id,
            .last_id = *last_id,
            .max_results = *max_results,
        });
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> preset(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 4U); !valid_count) {
            return valid_count;
        }
        auto name = textValue(arguments[0]);
        auto key = textValue(arguments[1]);
        auto value = signedValue(arguments[2], core::lang::TypeKind::I64, 8U);
        auto hard = boolValue(arguments[3]);
        if (!name || !key || !value || !hard || !validText(*name) || !validText(*key)) {
            return std::unexpected("policyRule received an invalid typed declaration");
        }
        auto found = std::ranges::find(m_presets, *name, &PolicyPreset::name);
        if (found == m_presets.end()) {
            if (m_presets.size() >= m_limits.max_presets) {
                return std::unexpected("policy preset limit exceeded");
            }
            m_presets.push_back({
                .id = static_cast<uint32_t>(m_presets.size()),
                .name = std::move(*name),
                .rules = {},
            });
            found = std::prev(m_presets.end());
        }
        if (found->rules.size() >= m_limits.max_rules_per_preset) {
            return std::unexpected("policy rule limit exceeded");
        }
        if (std::ranges::any_of(found->rules, [&key](PolicyRule const& rule) { return rule.key == *key; })) {
            return std::unexpected("duplicate policy preset rule");
        }
        found->rules.push_back({.key = std::move(*key), .value = *value, .hard = *hard});
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> assignment(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 3U); !valid_count) {
            return valid_count;
        }
        auto target = textValue(arguments[0]);
        auto preset_name = textValue(arguments[1]);
        auto hard = boolValue(arguments[2]);
        if (!target || !preset_name || !hard || !validText(*target) || !validText(*preset_name)) {
            return std::unexpected("policyAssign received an invalid typed declaration");
        }
        if (isReservedWorldTarget(*target)) {
            return std::unexpected("policyAssign cannot target the reserved world subject namespace");
        }
        if (std::ranges::find(m_presets, *preset_name, &PolicyPreset::name) == m_presets.end()) {
            return std::unexpected("policyAssign references an unknown preset");
        }
        if (std::ranges::any_of(m_assignments, [&target](PolicyAssignment const& assignment) {
                return assignment.target == *target;
            })) {
            return std::unexpected("duplicate policy assignment");
        }
        if (m_assignments.size() >= m_limits.max_assignments) {
            return std::unexpected("policy assignment limit exceeded");
        }
        m_assignments.push_back({
            .target = std::move(*target),
            .preset = std::move(*preset_name),
            .hard = *hard,
        });
        return {};
    }

    [[nodiscard]]
    bool isCancelled() const noexcept
    {
        return m_cancellation && m_cancellation->isCancellationRequested();
    }

private:
    std::string m_source_id;
    PolicyLimits const& m_limits;
    PolicyCancellation const* m_cancellation;
    uint64_t m_statements{0U};
    std::vector<PolicyGroup> m_groups;
    std::vector<PolicySelector> m_selectors;
    std::vector<PolicyPreset> m_presets;
    std::vector<PolicyAssignment> m_assignments;
};

namespace {

[[nodiscard]]
std::expected<PolicyPlan, PolicyDiagnostic> executePolicy(
    std::string_view const source_id,
    core::lang::CompileResult const& compiled,
    PolicyLimits const& limits,
    PolicyCancellation const* const cancellation,
    PolicyJitMode const jit_mode,
    bool* const jit_prepared
)
{
    PolicyCollector collector{source_id, limits, cancellation};
    std::vector<HostSpec> const specs = hostSpecs();
    std::vector<core::lang::CustomProvider> providers;
    providers.reserve(specs.size());
    for (HostSpec const& spec : specs) {
        core::lang::CustomManifest const host_manifest = manifest(spec);
        providers.push_back({
            .manifest = host_manifest,
            .invoke = [&collector, host_call = spec.call](
                core::lang::CustomContext&,
                std::span<core::lang::Value const> const arguments
            ) {
                auto const result = collector.call(host_call, arguments);
                if (!result) {
                    return core::lang::CustomOutcome{core::lang::CustomFail{
                        .code = 1U,
                        .message = result.error(),
                    }};
                }
                return core::lang::CustomOutcome{core::lang::CustomComplete{.value = unitValue()}};
            },
            .cancel = {},
        });
    }

    core::lang::RuntimeOptions runtime_options;
    runtime_options.jit_mode = coreJitMode(jit_mode);
    runtime_options.jit_diagnostic = [jit_prepared](core::lang::JitDiagnostic const&) {
        if (jit_prepared) {
            *jit_prepared = false;
        }
    };
    core::lang::Runtime runtime{runtime_options};
    for (core::lang::CustomProvider& provider : providers) {
        auto const registered = runtime.registerProvider(std::move(provider));
        if (!registered) {
            return std::unexpected(diagnostic(
                PolicyDiagnosticCode::RuntimeFailure,
                source_id,
                registered.error().message
            ));
        }
    }
    if (cancellation && cancellation->isCancellationRequested()) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::Cancelled,
            source_id,
            "policy compilation was cancelled"
        ));
    }
    auto const program = runtime.load(compiled.bytes);
    if (!program) {
        return std::unexpected(diagnostic(
            jit_mode == PolicyJitMode::Required
                ? PolicyDiagnosticCode::JitPreparationFailure
                : PolicyDiagnosticCode::RuntimeFailure,
            source_id,
            program.error().message
        ));
    }
    if (jit_prepared && jit_mode == PolicyJitMode::Disabled) {
        *jit_prepared = false;
    }
    auto const executed = runtime.execute(*program, source_id, "policy", {}, {});
    if (!executed) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::RuntimeFailure,
            source_id,
            executed.error().message
        ));
    }
    if (auto const* const failed = std::get_if<core::lang::Failed>(&*executed)) {
        std::string_view const message = failed->failure.message;
        return std::unexpected(diagnostic(
            failed->failure.code == core::lang::FailureCode::Cancelled
                ? PolicyDiagnosticCode::Cancelled
                : collectorFailureCode(message),
            source_id,
            std::string{message}
        ));
    }
    if (!std::holds_alternative<core::lang::Completed>(*executed)) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::RuntimeFailure,
            source_id,
            "policy program did not complete"
        ));
    }
    auto plan = std::move(collector).build();
    if (!plan) {
        return std::unexpected(diagnostic(
            collectorFailureCode(plan.error()),
            source_id,
            plan.error()
        ));
    }
    return std::move(*plan);
}

[[nodiscard]]
std::expected<uint64_t, PolicyDiagnostic> boundedInstructionCount(
    std::string_view const source_id,
    core::lang::Artifact const& artifact,
    PolicyLimits const& limits,
    uint64_t const execution_count
)
{
    auto fail = [source_id](PolicyDiagnosticCode const code, std::string message) {
        return std::unexpected(diagnostic(code, source_id, std::move(message)));
    };
    std::vector<uint8_t> state(artifact.functions.size(), 0U);
    std::vector<uint64_t> costs(artifact.functions.size(), 0U);
    std::function<std::expected<uint64_t, PolicyDiagnostic>(uint64_t)> count_function;
    count_function = [&](uint64_t const function_index) -> std::expected<uint64_t, PolicyDiagnostic> {
        if (function_index >= artifact.functions.size()) {
            return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode has an invalid function target");
        }
        uint8_t& function_state = state[static_cast<std::vector<uint8_t>::size_type>(function_index)];
        if (function_state == 1U) {
            return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode contains recursive calls");
        }
        if (function_state == 2U) {
            return costs[static_cast<std::vector<uint64_t>::size_type>(function_index)];
        }
        function_state = 1U;

        core::lang::Function const& function = artifact.functions[
            static_cast<std::vector<core::lang::Function>::size_type>(function_index)
        ];
        std::vector<uint64_t> block_costs(function.blocks.size(), 0U);
        std::vector<uint8_t> block_state(function.blocks.size(), 0U);
        std::function<std::expected<uint64_t, PolicyDiagnostic>(uint64_t)> count_block;
        count_block = [&](uint64_t const block_index) -> std::expected<uint64_t, PolicyDiagnostic> {
            if (block_index >= function.blocks.size()) {
                return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode has an invalid block target");
            }
            uint8_t& block_mark = block_state[static_cast<std::vector<uint8_t>::size_type>(block_index)];
            if (block_mark == 1U) {
                return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode contains a control-flow cycle");
            }
            if (block_mark == 2U) {
                return block_costs[static_cast<std::vector<uint64_t>::size_type>(block_index)];
            }
            block_mark = 1U;

            core::lang::Block const& block = function.blocks[
                static_cast<std::vector<core::lang::Block>::size_type>(block_index)
            ];
            if (block.instructions.empty()) {
                return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode contains an empty block");
            }
            uint64_t local_cost = 0U;
            for (core::lang::Instruction const& instruction : block.instructions) {
                if (instruction.opcode == core::lang::Op::IndirectCall
                    || instruction.opcode == core::lang::Op::StaticBegin) {
                    return fail(
                        PolicyDiagnosticCode::UnboundedExecution,
                        "policy declarations may not use indirect calls or static initialization"
                    );
                }
                if (local_cost == limits.max_instructions) {
                    return fail(PolicyDiagnosticCode::InstructionLimitExceeded, "policy exceeds the instruction-step limit");
                }
                ++local_cost;
                if (instruction.opcode == core::lang::Op::Call) {
                    if (instruction.operands.size() < 2U
                        || instruction.operands[1U] != instruction.operands.size() - 2U) {
                        return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode has a malformed call");
                    }
                    auto const callee_cost = count_function(instruction.operands[0U]);
                    if (!callee_cost) {
                        return std::unexpected(callee_cost.error());
                    }
                    if (*callee_cost > limits.max_instructions - local_cost) {
                        return fail(PolicyDiagnosticCode::InstructionLimitExceeded, "policy exceeds the instruction-step limit");
                    }
                    local_cost += *callee_cost;
                }
            }

            std::vector<uint64_t> successors;
            core::lang::Instruction const& terminator = block.instructions.back();
            if (terminator.opcode == core::lang::Op::Branch) {
                if (terminator.operands.size() < 2U
                    || terminator.operands[1U] != terminator.operands.size() - 2U) {
                    return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode has a malformed branch");
                }
                successors.push_back(terminator.operands[0U]);
            } else if (terminator.opcode == core::lang::Op::ConditionalBranch) {
                if (terminator.operands.size() < 5U) {
                    return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode has a malformed conditional branch");
                }
                uint64_t const true_count = terminator.operands[2U];
                if (true_count > terminator.operands.size() - 3U) {
                    return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode has a malformed conditional branch");
                }
                uint64_t const false_offset = 3U + true_count;
                if (false_offset + 2U > terminator.operands.size()
                    || terminator.operands[false_offset + 1U] != terminator.operands.size() - (false_offset + 2U)) {
                    return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode has a malformed conditional branch");
                }
                successors.push_back(terminator.operands[1U]);
                successors.push_back(terminator.operands[false_offset]);
            } else if (terminator.opcode != core::lang::Op::ReturnValue
                && terminator.opcode != core::lang::Op::ReturnUnit
                && terminator.opcode != core::lang::Op::Halt) {
                return fail(PolicyDiagnosticCode::UnboundedExecution, "policy bytecode has a non-terminal block ending");
            }

            uint64_t longest_successor = 0U;
            for (uint64_t const successor : successors) {
                auto const successor_cost = count_block(successor);
                if (!successor_cost) {
                    return std::unexpected(successor_cost.error());
                }
                longest_successor = std::max(longest_successor, *successor_cost);
            }
            if (local_cost > limits.max_instructions
                || longest_successor > limits.max_instructions - local_cost) {
                return fail(PolicyDiagnosticCode::InstructionLimitExceeded, "policy exceeds the instruction-step limit");
            }
            block_mark = 2U;
            block_costs[static_cast<std::vector<uint64_t>::size_type>(block_index)] = local_cost + longest_successor;
            return block_costs[static_cast<std::vector<uint64_t>::size_type>(block_index)];
        };

        if (function.blocks.empty()) {
            return fail(PolicyDiagnosticCode::UnboundedExecution, "policy entry has no executable bytecode");
        }
        auto const cost = count_block(0U);
        if (!cost) {
            return std::unexpected(cost.error());
        }
        function_state = 2U;
        costs[static_cast<std::vector<uint64_t>::size_type>(function_index)] = *cost;
        return *cost;
    };

    uint64_t total = 0U;
    auto add_entry = [&](uint64_t const function_index) -> std::expected<void, PolicyDiagnostic> {
        if (function_index == core::lang::NO_OPERAND) {
            return {};
        }
        auto const cost = count_function(function_index);
        if (!cost) {
            return std::unexpected(cost.error());
        }
        if (*cost > limits.max_instructions - std::min(total, limits.max_instructions)) {
            return std::unexpected(diagnostic(
                PolicyDiagnosticCode::InstructionLimitExceeded,
                source_id,
                "policy exceeds the instruction-step limit"
            ));
        }
        total += *cost;
        return {};
    };
    for (core::lang::Module const& module : artifact.modules) {
        auto const added = add_entry(module.init_function);
        if (!added) {
            return std::unexpected(added.error());
        }
    }
    for (core::lang::Module const& module : artifact.modules) {
        if (module.id != source_id) {
            continue;
        }
        for (core::lang::Export const& exported : module.exports) {
            if (exported.kind == core::lang::ExportKind::Function && exported.name == "policy") {
                auto const added = add_entry(exported.target);
                if (!added) {
                    return std::unexpected(added.error());
                }
            }
        }
    }
    if (execution_count > 1U) {
        if (total > limits.max_instructions / execution_count) {
            return fail(PolicyDiagnosticCode::InstructionLimitExceeded, "policy exceeds the instruction-step limit");
        }
        total *= execution_count;
    }
    return total;
}

} // namespace

PolicyLimits defaultPolicyLimits() noexcept
{
    return {};
}

PolicyPlan::PolicyPlan(
    std::string source_id,
    std::vector<PolicyGroup> groups,
    std::vector<PolicySelector> selectors,
    std::vector<PolicyPreset> presets,
    std::vector<PolicyAssignment> assignments,
    uint64_t const statement_count
)
    : m_source_id(std::move(source_id))
    , m_groups(std::move(groups))
    , m_selectors(std::move(selectors))
    , m_presets(std::move(presets))
    , m_assignments(std::move(assignments))
    , m_statement_count(statement_count)
{
}

std::string_view PolicyPlan::sourceId() const noexcept
{
    return m_source_id;
}

std::vector<PolicyGroup> const& PolicyPlan::groups() const noexcept
{
    return m_groups;
}

std::vector<PolicySelector> const& PolicyPlan::selectors() const noexcept
{
    return m_selectors;
}

std::vector<PolicyPreset> const& PolicyPlan::presets() const noexcept
{
    return m_presets;
}

std::vector<PolicyAssignment> const& PolicyPlan::assignments() const noexcept
{
    return m_assignments;
}

uint64_t PolicyPlan::statementCount() const noexcept
{
    return m_statement_count;
}

PolicyPlan PolicyPlan::withoutSubject(PolicyEntityId const subject) const
{
    std::vector<PolicyGroup> groups = m_groups;
    for (PolicyGroup& group : groups) {
        std::erase(group.members, subject);
    }
    std::vector<PolicyAssignment> assignments = m_assignments;
    std::erase_if(assignments, [subject](PolicyAssignment const& assignment) {
        std::optional<PolicyEntityId> const assigned_subject = entityTargetId(assignment.target);
        return assigned_subject.has_value() && *assigned_subject == subject;
    });
    return PolicyPlan{
        m_source_id,
        std::move(groups),
        m_selectors,
        m_presets,
        std::move(assignments),
        m_statement_count,
    };
}

bool operator==(PolicyPlan const& left, PolicyPlan const& right) noexcept
{
    return left.m_source_id == right.m_source_id
        && left.m_groups == right.m_groups
        && left.m_selectors == right.m_selectors
        && left.m_presets == right.m_presets
        && left.m_assignments == right.m_assignments
        && left.m_statement_count == right.m_statement_count;
}

std::string policyPlanId(PolicyPlan const& plan)
{
    uint64_t hash = 1'469'598'103'934'665'603ULL;
    auto append = [&hash](std::string_view const text) {
        for (char const character : text) {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1'099'511'628'211ULL;
        }
        hash ^= 0xffU;
        hash *= 1'099'511'628'211ULL;
    };
    auto appendNumber = [&append](uint64_t const number) {
        char buffer[32];
        auto const result = std::to_chars(std::begin(buffer), std::end(buffer), number);
        append(std::string_view{buffer, static_cast<size_t>(result.ptr - buffer)});
    };
    append(plan.sourceId());
    appendNumber(plan.statementCount());
    for (PolicyGroup const& group : plan.groups()) {
        appendNumber(group.id);
        append(group.name);
        appendNumber(static_cast<uint8_t>(group.kind));
        append(group.entity_kind);
        append(group.selector);
        for (PolicyEntityId const member : group.members) {
            appendNumber(member);
        }
    }
    for (PolicySelector const& selector : plan.selectors()) {
        appendNumber(selector.id);
        append(selector.name);
        appendNumber(static_cast<uint8_t>(selector.criterion));
        append(selector.entity_kind);
        appendNumber(selector.first_id);
        appendNumber(selector.last_id);
        appendNumber(selector.max_results);
    }
    for (PolicyPreset const& preset : plan.presets()) {
        appendNumber(preset.id);
        append(preset.name);
        for (PolicyRule const& rule : preset.rules) {
            append(rule.key);
            appendNumber(static_cast<uint64_t>(rule.value));
            appendNumber(rule.hard ? 1U : 0U);
        }
    }
    for (PolicyAssignment const& assignment : plan.assignments()) {
        append(assignment.target);
        append(assignment.preset);
        appendNumber(assignment.hard ? 1U : 0U);
    }
    char result[17];
    auto const conversion = std::to_chars(std::begin(result), std::end(result), hash, 16);
    return {result, conversion.ptr};
}

PolicySnapshot::PolicySnapshot(
    uint64_t const generation,
    PolicyPlan plan,
    PolicyJitMode const jit_mode,
    bool const jit_prepared,
    bool const interpreter_parity,
    std::shared_ptr<PolicyCapabilitySnapshot const> capabilities
)
    : m_generation(generation)
    , m_plan(std::move(plan))
    , m_jit_mode(jit_mode)
    , m_jit_prepared(jit_prepared)
    , m_interpreter_parity(interpreter_parity)
    , m_capabilities(std::move(capabilities))
{
}

uint64_t PolicySnapshot::generation() const noexcept
{
    return m_generation;
}

PolicyPlan const& PolicySnapshot::plan() const noexcept
{
    return m_plan;
}

PolicyJitMode PolicySnapshot::jitMode() const noexcept
{
    return m_jit_mode;
}

bool PolicySnapshot::jitPrepared() const noexcept
{
    return m_jit_prepared;
}

bool PolicySnapshot::interpreterParity() const noexcept
{
    return m_interpreter_parity;
}

std::shared_ptr<PolicyCapabilitySnapshot const> PolicySnapshot::capabilities() const noexcept
{
    return m_capabilities;
}

uint64_t PolicyCapabilitySnapshot::generation() const noexcept
{
    return m_generation;
}

uint32_t PolicyCapabilitySnapshot::capabilityCount() const noexcept
{
    return m_capability_count;
}

std::optional<int64_t> PolicyCapabilitySnapshot::value(
    PolicyCapabilityQuery const query
) const noexcept
{
    auto const row = m_rows.find(query.subject);
    if (row == m_rows.end() || query.key >= m_capability_count) {
        return std::nullopt;
    }
    return m_values[static_cast<size_t>(row->second) * m_capability_count + query.key];
}

bool PolicyCapabilitySnapshot::allows(
    PolicyCapabilityQuery const query
) const noexcept
{
    auto const result = value(query);
    return result.has_value() && *result > 0;
}

PolicyHost::PolicyHost(PolicyLimits limits)
    : m_limits(limits)
{
}

PolicyLimits const& PolicyHost::limits() const noexcept
{
    return m_limits;
}

std::expected<PolicyCompilation, PolicyDiagnostic> PolicyHost::compile(
    std::string_view const source_id,
    std::string_view const source,
    PolicyCompileOptions const options,
    PolicyCancellation const* const cancellation
) const
{
    auto const invalid_limits = m_limits.max_source_bytes == 0U || m_limits.max_instructions == 0U
        || m_limits.max_statements == 0U
        || m_limits.max_groups == 0U || m_limits.max_members_per_group == 0U
        || m_limits.max_selectors == 0U || m_limits.max_presets == 0U
        || m_limits.max_rules_per_preset == 0U || m_limits.max_assignments == 0U
        || m_limits.max_subjects == 0U || m_limits.max_capabilities == 0U;
    if (invalid_limits) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::InvalidLimits,
            source_id,
            "all policy limits must be positive"
        ));
    }
    if (source.size() > m_limits.max_source_bytes) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::SourceTooLarge,
            source_id,
            "policy source exceeds the configured byte limit"
        ));
    }
    if (cancellation && cancellation->isCancellationRequested()) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::Cancelled,
            source_id,
            "policy compilation was cancelled"
        ));
    }

    std::vector<HostSpec> const specs = hostSpecs();
    core::lang::Ruleset ruleset{
        .id = "minecraft",
        .version = 1U,
        .restrictions = {},
        .operations = {},
        .triggers = {},
        .resolve = {},
        .automaticImports = {},
    };
    ruleset.operations.reserve(specs.size());
    for (HostSpec const& spec : specs) {
        core::lang::CustomManifest const host_manifest = manifest(spec);
        ruleset.operations.push_back({
            .name = std::string{spec.name},
            .kind = core::lang::ExtensionKind::Builtin,
            .arguments = host_manifest.arguments,
            .result = host_manifest.result,
            .effect = host_manifest.effect,
            .custom = host_manifest,
            .expand = {},
            .compile_time_effect = {},
            .compile_time = {},
            .borrow = host_manifest.borrow,
        });
    }
    core::lang::CompilerRegistry const registry{
        .rulesets = {std::move(ruleset)},
        .defaults = {"minecraft"},
        .restrictions = {},
    };
    core::lang::CompileOptions compile_options;
    compile_options.cancelled = [cancellation] {
        return cancellation && cancellation->isCancellationRequested();
    };
    auto const compiled = core::lang::compile(
        core::lang::Source{.id = std::string{source_id}, .text = std::string{source}},
        registry,
        compile_options
    );
    if (!compiled) {
        if (cancellation && cancellation->isCancellationRequested()) {
            return std::unexpected(diagnostic(
                PolicyDiagnosticCode::Cancelled,
                source_id,
                "policy compilation was cancelled"
            ));
        }
        std::string message = "CoreLang compilation failed";
        if (!compiled.error().empty()) {
            message = compiled.error().front().message;
        }
        return std::unexpected(diagnostic(PolicyDiagnosticCode::CompileFailure, source_id, std::move(message)));
    }

    uint64_t const execution_count = options.require_interpreter_parity
            && options.jit_mode != PolicyJitMode::Disabled
        ? 2U
        : 1U;
    auto const instruction_count = boundedInstructionCount(
        source_id,
        compiled->artifact,
        m_limits,
        execution_count
    );
    if (!instruction_count) {
        return std::unexpected(instruction_count.error());
    }

    bool jit_prepared = options.jit_mode != PolicyJitMode::Disabled;
    auto first = executePolicy(source_id, *compiled, m_limits, cancellation, options.jit_mode, &jit_prepared);
    if (!first) {
        return std::unexpected(first.error());
    }
    bool interpreter_parity = options.jit_mode == PolicyJitMode::Disabled;
    if (options.require_interpreter_parity && !interpreter_parity) {
        bool interpreted_jit_prepared = false;
        auto interpreted = executePolicy(
            source_id,
            *compiled,
            m_limits,
            cancellation,
            PolicyJitMode::Disabled,
            &interpreted_jit_prepared
        );
        if (!interpreted) {
            return std::unexpected(interpreted.error());
        }
        if (*first != *interpreted) {
            return std::unexpected(diagnostic(
                PolicyDiagnosticCode::InterpreterParityFailure,
                source_id,
                "CoreLang policy produced different native and interpreter plans"
            ));
        }
        interpreter_parity = true;
    }
    return PolicyCompilation{
        .plan = std::move(*first),
        .jit_mode = options.jit_mode,
        .jit_prepared = jit_prepared,
        .interpreter_parity = interpreter_parity,
    };
}

std::expected<PolicyCapabilitySnapshot, PolicyDiagnostic> PolicyHost::materialize(
    PolicyPlan const& plan,
    std::span<PolicySubject const> const subjects,
    PolicyCapabilityRegistry const& registry
) const
{
    if (subjects.size() > m_limits.max_subjects || registry.definitions.size() > m_limits.max_capabilities) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::InvalidDeclaration,
            plan.sourceId(),
            "policy materialization exceeds the configured catalog limits"
        ));
    }
    std::unordered_map<std::string_view, PolicyCapabilityKeyId> keys;
    keys.reserve(registry.definitions.size());
    for (PolicyCapabilityKeyId index = 0U; index < registry.definitions.size(); ++index) {
        PolicyCapabilityDefinition const& definition = registry.definitions[index];
        if (!validCapabilityKey(definition.key)
            || definition.minimum_value > definition.maximum_value
            || definition.default_value < definition.minimum_value
            || definition.default_value > definition.maximum_value
            || !keys.emplace(definition.key, index).second) {
            return std::unexpected(diagnostic(
                PolicyDiagnosticCode::InvalidDeclaration,
                plan.sourceId(),
                "policy capability registry is invalid"
            ));
        }
    }
    if (registry.definitions.size() > std::numeric_limits<PolicyCapabilityKeyId>::max()) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::InvalidDeclaration,
            plan.sourceId(),
            "policy capability registry exceeds the dense key range"
        ));
    }

    std::unordered_map<PolicyEntityId, PolicySubject const*> subjects_by_id;
    subjects_by_id.reserve(subjects.size());
    for (PolicySubject const& subject : subjects) {
        if (!validEntityClass(subject.entity_class) || !validText(subject.kind)
            || !subjects_by_id.emplace(subject.id, &subject).second) {
            return std::unexpected(diagnostic(
                PolicyDiagnosticCode::InvalidDeclaration,
                plan.sourceId(),
                "policy subject catalog is invalid"
            ));
        }
    }

    std::unordered_map<std::string_view, PolicyGroup const*> groups;
    groups.reserve(plan.groups().size());
    for (PolicyGroup const& group : plan.groups()) {
        if (!groups.emplace(group.name, &group).second
            || (group.kind == PolicyGroupKind::EntityKind && !validText(group.entity_kind))
            || (group.kind != PolicyGroupKind::EntityKind && !group.entity_kind.empty())) {
            return std::unexpected(diagnostic(
                PolicyDiagnosticCode::InvalidDeclaration,
                plan.sourceId(),
                "policy group declaration is invalid"
            ));
        }
        for (PolicyEntityId const member : group.members) {
            if (!subjects_by_id.contains(member)) {
                return std::unexpected(diagnostic(
                    PolicyDiagnosticCode::UnknownDeclaration,
                    plan.sourceId(),
                    "explicit policy group references an unknown subject"
                ));
            }
        }
    }
    std::unordered_map<std::string_view, PolicySelector const*> selectors;
    selectors.reserve(plan.selectors().size());
    for (PolicySelector const& selector : plan.selectors()) {
        if (!validText(selector.name) || !validText(selector.entity_kind)
            || selector.criterion != PolicySelector::Criterion::EntityKindIdRange
            || selector.first_id > selector.last_id || selector.max_results == 0U
            || !selectors.emplace(selector.name, &selector).second) {
            return std::unexpected(diagnostic(
                selectors.contains(selector.name)
                    ? PolicyDiagnosticCode::DuplicateDeclaration
                    : PolicyDiagnosticCode::InvalidDeclaration,
                plan.sourceId(),
                "policy selector declaration is invalid or duplicated"
            ));
        }
    }
    for (PolicyGroup const& group : plan.groups()) {
        if (group.kind == PolicyGroupKind::Selector && !selectors.contains(group.selector)) {
            return std::unexpected(diagnostic(
                PolicyDiagnosticCode::UnknownDeclaration,
                plan.sourceId(),
                "selector policy group references an unknown selector"
            ));
        }
    }

    std::unordered_map<std::string_view, PolicyPreset const*> presets;
    presets.reserve(plan.presets().size());
    for (PolicyPreset const& preset : plan.presets()) {
        if (!presets.emplace(preset.name, &preset).second) {
            return std::unexpected(diagnostic(
                PolicyDiagnosticCode::DuplicateDeclaration,
                plan.sourceId(),
                "duplicate policy preset"
            ));
        }
        for (PolicyRule const& rule : preset.rules) {
            auto const key = keys.find(rule.key);
            if (key == keys.end()) {
                return std::unexpected(diagnostic(
                    PolicyDiagnosticCode::UnknownDeclaration,
                    plan.sourceId(),
                    "policy rule uses an unregistered capability key"
                ));
            }
            PolicyCapabilityDefinition const& definition = registry.definitions[key->second];
            if (rule.value < definition.minimum_value || rule.value > definition.maximum_value) {
                return std::unexpected(diagnostic(
                    PolicyDiagnosticCode::InvalidDeclaration,
                    plan.sourceId(),
                    "policy rule is outside the capability range"
                ));
            }
        }
    }

    std::unordered_map<std::string_view, std::vector<PolicyEntityId>> selected_members;
    selected_members.reserve(plan.selectors().size());
    for (PolicySelector const& selector : plan.selectors()) {
        std::vector<PolicyEntityId>& members = selected_members[selector.name];
        uint64_t const reserve_count = std::min<uint64_t>(
            static_cast<uint64_t>(subjects.size()), selector.max_results
        );
        members.reserve(static_cast<size_t>(reserve_count));
        for (PolicySubject const& subject : subjects) {
            if (subject.kind != selector.entity_kind
                || subject.id < selector.first_id || subject.id > selector.last_id) {
                continue;
            }
            if (static_cast<uint64_t>(members.size()) >= selector.max_results) {
                return std::unexpected(diagnostic(
                    PolicyDiagnosticCode::SelectorLimitExceeded,
                    plan.sourceId(),
                    "policy selector exceeded its result limit"
                ));
            }
            members.push_back(subject.id);
        }
        std::ranges::sort(members);
    }

    auto const groupSpecificity = [&groups, &selected_members](
        std::string_view const target,
        PolicySubject const& subject
    ) -> uint8_t {
        if (uint8_t const builtin = entityClassSpecificity(target, subject.entity_class); builtin != 0U) {
            return builtin;
        }
        if (auto const entity_kind = entityKindTarget(target); entity_kind.has_value()) {
            return subject.kind == *entity_kind ? 3U : 0U;
        }
        if (auto const entity = entityTargetId(target); entity.has_value()) {
            return *entity == subject.id ? 6U : 0U;
        }
        auto const found = groups.find(target);
        if (found == groups.end()) {
            return 0U;
        }
        PolicyGroup const& group = *found->second;
        switch (group.kind) {
            case PolicyGroupKind::All: return 1U;
            case PolicyGroupKind::Players:
                return subject.entity_class == PolicyEntityClass::Player ? 2U : 0U;
            case PolicyGroupKind::Mobs:
                return subject.entity_class == PolicyEntityClass::Mob ? 2U : 0U;
            case PolicyGroupKind::TechnicalEntities:
                return subject.entity_class == PolicyEntityClass::TechnicalEntity ? 2U : 0U;
            case PolicyGroupKind::EntityKind:
                return subject.kind == group.entity_kind ? 3U : 0U;
            case PolicyGroupKind::Explicit:
                return std::ranges::find(group.members, subject.id) != group.members.end() ? 5U : 0U;
            case PolicyGroupKind::Selector: {
                std::vector<PolicyEntityId> const& members = selected_members.at(group.selector);
                return std::ranges::binary_search(members, subject.id) ? 4U : 0U;
            }
        }
        return 0U;
    };
    for (PolicyAssignment const& assignment : plan.assignments()) {
        if (!presets.contains(assignment.preset)) {
            return std::unexpected(diagnostic(
                PolicyDiagnosticCode::UnknownDeclaration,
                plan.sourceId(),
                "policy assignment references an unknown preset"
            ));
        }
        std::optional<PolicyEntityId> const entity_target_id = entityTargetId(assignment.target);
        bool const known_target = entityClassSpecificity(assignment.target, PolicyEntityClass::Player) != 0U
            || assignment.target == "mobs" || assignment.target == "entities"
            || entityKindTarget(assignment.target).has_value()
            || groups.contains(assignment.target)
            || (entity_target_id.has_value() && subjects_by_id.contains(*entity_target_id));
        if (isReservedWorldTarget(assignment.target) || !known_target) {
            return std::unexpected(diagnostic(
                isReservedWorldTarget(assignment.target)
                    ? PolicyDiagnosticCode::InvalidDeclaration
                    : PolicyDiagnosticCode::UnknownDeclaration,
                plan.sourceId(),
                isReservedWorldTarget(assignment.target)
                    ? "policy assignment cannot target the reserved world subject namespace"
                    : "policy assignment references an unknown target"
            ));
        }
    }

    PolicyCapabilitySnapshot result;
    result.m_capability_count = static_cast<uint32_t>(registry.definitions.size());
    result.m_materialized_for = plan;
    result.m_values.reserve(subjects.size() * registry.definitions.size());
    for (PolicySubject const& subject : subjects) {
        uint32_t const row = static_cast<uint32_t>(result.m_rows.size());
        result.m_rows.emplace(subject.id, row);
        std::vector<int64_t> values;
        std::vector<uint8_t> soft_specificity;
        std::vector<std::optional<int64_t>> hard_constraints;
        values.reserve(registry.definitions.size());
        soft_specificity.resize(registry.definitions.size(), 0U);
        hard_constraints.resize(registry.definitions.size());
        for (PolicyCapabilityDefinition const& definition : registry.definitions) {
            values.push_back(definition.default_value);
        }
        for (PolicyAssignment const& assignment : plan.assignments()) {
            uint8_t const specificity = groupSpecificity(assignment.target, subject);
            if (specificity == 0U) {
                continue;
            }
            PolicyPreset const& preset = *presets.at(assignment.preset);
            for (PolicyRule const& rule : preset.rules) {
                PolicyCapabilityKeyId const key = keys.at(rule.key);
                PolicyCapabilityDefinition const& definition = registry.definitions[key];
                if (assignment.hard || rule.hard) {
                    std::optional<int64_t>& constraint = hard_constraints[key];
                    if (!constraint.has_value()) {
                        constraint = rule.value;
                    } else if (definition.hard_restriction == PolicyRestriction::Maximum) {
                        *constraint = std::min(*constraint, rule.value);
                    } else {
                        *constraint = std::max(*constraint, rule.value);
                    }
                } else if (specificity > soft_specificity[key]) {
                    values[key] = rule.value;
                    soft_specificity[key] = specificity;
                } else if (specificity == soft_specificity[key]) {
                    values[key] = definition.hard_restriction == PolicyRestriction::Maximum
                        ? std::min(values[key], rule.value)
                        : std::max(values[key], rule.value);
                }
            }
        }
        for (PolicyCapabilityKeyId key = 0U; key < registry.definitions.size(); ++key) {
            std::optional<int64_t> const hard_constraint = hard_constraints[key];
            if (!hard_constraint.has_value()) {
                continue;
            }
            int64_t const hard_value = hard_constraint.value_or(values[key]);
            PolicyCapabilityDefinition const& definition = registry.definitions[key];
            values[key] = definition.hard_restriction == PolicyRestriction::Maximum
                ? std::min(values[key], hard_value)
                : std::max(values[key], hard_value);
        }
        result.m_values.insert(result.m_values.end(), values.begin(), values.end());
    }
    return result;
}

std::expected<uint64_t, PolicyDiagnostic> PolicyHost::publish(PolicyCompilation const& compilation)
{
    return std::unexpected(diagnostic(
        PolicyDiagnosticCode::MissingMaterialization,
        compilation.plan.sourceId(),
        "policy capabilities must be materialized before publication"
    ));
}

std::expected<uint64_t, PolicyDiagnostic> PolicyHost::publish(
    PolicyCompilation compilation,
    PolicyCapabilitySnapshot capabilities
)
{
    if (!capabilities.m_materialized_for.has_value()) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::MissingMaterialization,
            compilation.plan.sourceId(),
            "policy capabilities must be materialized before publication"
        ));
    }
    if (*capabilities.m_materialized_for != compilation.plan) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::MaterializationMismatch,
            compilation.plan.sourceId(),
            "policy capabilities were materialized for a different plan"
        ));
    }
    capabilities.m_materialized_for.reset();
    std::scoped_lock lock{m_publish_mutex};
    if (m_generation == std::numeric_limits<uint64_t>::max()) {
        return std::unexpected(diagnostic(
            PolicyDiagnosticCode::InvalidDeclaration,
            compilation.plan.sourceId(),
            "policy generation exhausted"
        ));
    }
    uint64_t const next_generation = m_generation + 1U;
    capabilities.m_generation = next_generation;
    auto materialized = std::shared_ptr<PolicyCapabilitySnapshot const>(
        new PolicyCapabilitySnapshot(std::move(capabilities))
    );
    auto next = std::shared_ptr<PolicySnapshot const>(new PolicySnapshot(
        next_generation,
        std::move(compilation.plan),
        compilation.jit_mode,
        compilation.jit_prepared,
        compilation.interpreter_parity,
        std::move(materialized)
    ));
    std::atomic_store_explicit(&m_snapshot, std::move(next), std::memory_order_release);
    m_generation = next_generation;
    return next_generation;
}

std::shared_ptr<PolicySnapshot const> PolicyHost::snapshot() const noexcept
{
    return std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
}

std::string_view policyDiagnosticCodeName(PolicyDiagnosticCode const code) noexcept
{
    switch (code) {
        case PolicyDiagnosticCode::InvalidLimits: return "invalid-limits";
        case PolicyDiagnosticCode::SourceTooLarge: return "source-too-large";
        case PolicyDiagnosticCode::StatementLimitExceeded: return "statement-limit-exceeded";
        case PolicyDiagnosticCode::GroupLimitExceeded: return "group-limit-exceeded";
        case PolicyDiagnosticCode::MemberLimitExceeded: return "member-limit-exceeded";
        case PolicyDiagnosticCode::SelectorLimitExceeded: return "selector-limit-exceeded";
        case PolicyDiagnosticCode::PresetLimitExceeded: return "preset-limit-exceeded";
        case PolicyDiagnosticCode::RuleLimitExceeded: return "rule-limit-exceeded";
        case PolicyDiagnosticCode::AssignmentLimitExceeded: return "assignment-limit-exceeded";
        case PolicyDiagnosticCode::InvalidDeclaration: return "invalid-declaration";
        case PolicyDiagnosticCode::DuplicateDeclaration: return "duplicate-declaration";
        case PolicyDiagnosticCode::UnknownDeclaration: return "unknown-declaration";
        case PolicyDiagnosticCode::CompileFailure: return "compile-failure";
        case PolicyDiagnosticCode::RuntimeFailure: return "runtime-failure";
        case PolicyDiagnosticCode::JitPreparationFailure: return "jit-preparation-failure";
        case PolicyDiagnosticCode::InterpreterParityFailure: return "interpreter-parity-failure";
        case PolicyDiagnosticCode::Cancelled: return "cancelled";
        case PolicyDiagnosticCode::InstructionLimitExceeded: return "instruction-limit-exceeded";
        case PolicyDiagnosticCode::UnboundedExecution: return "unbounded-execution";
        case PolicyDiagnosticCode::MaterializationMismatch: return "materialization-mismatch";
        case PolicyDiagnosticCode::MissingMaterialization: return "missing-materialization";
    }
    return "unknown";
}

} // namespace shared
