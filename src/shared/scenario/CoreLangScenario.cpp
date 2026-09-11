#include <shared/scenario/Scenario.hpp>

#include <core/lang/CoreLang.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shared {

namespace scenario_detail {

namespace {

static constexpr uint8_t MAX_POSITION = 31;

[[nodiscard]] bool isAllowedCharacter(char const character) noexcept
{
    return character == '@' || character == '#' || character == '$' || character == '%' || character == '&';
}

[[nodiscard]] bool isIdentifier(std::string_view const text) noexcept
{
    if (text.empty() || !((text.front() >= 'a' && text.front() <= 'z')
        || (text.front() >= 'A' && text.front() <= 'Z') || text.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(text.substr(1), [](char const character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '_' || character == '-';
    });
}

[[nodiscard]] bool placementsConflict(ScenarioActor const& actor, uint8_t const x, uint8_t const y) noexcept
{
    uint32_t const delta_x = actor.x > x ? actor.x - x : x - actor.x;
    uint32_t const delta_y = actor.y > y ? actor.y - y : y - actor.y;
    return delta_x <= 1U && delta_y <= 1U;
}

[[nodiscard]] ScenarioDiagnostic diagnostic(
    ScenarioDiagnosticCode const code,
    std::string_view const filename,
    ScenarioLocation const location,
    std::string message
)
{
    return ScenarioDiagnostic{
        .code = code,
        .filename = std::string{filename},
        .location = location,
        .message = std::move(message),
    };
}

} // namespace

class ScenarioPlanCollector final : public core::lang::RuntimeServices {
public:
    ScenarioPlanCollector(
        std::string_view const filename,
        ScenarioLimits const& limits,
        ScenarioCancellation const* const cancellation
    )
        : m_filename(filename)
        , m_limits(limits)
        , m_cancellation(cancellation)
    { }

    [[nodiscard]] std::expected<core::lang::Value, std::string> profile(std::span<core::lang::Value const> arguments)
    {
        if (auto const open = beginStatement(); !open) return std::unexpected(open.error());
        if (m_profile_set || arguments[0].text != "flat2d-v1") {
            return std::unexpected("scenario profile must be specified once as flat2d-v1");
        }
        m_profile_set = true;
        return core::lang::Value::unit();
    }

    [[nodiscard]] std::expected<core::lang::Value, std::string> seed(std::span<core::lang::Value const> arguments)
    {
        if (auto const open = beginStatement(); !open) return std::unexpected(open.error());
        if (!m_profile_set || m_seed_set) return std::unexpected("scenario seed must follow profile and occur once");
        m_seed = arguments[0].integer[1U];
        m_seed_set = true;
        return core::lang::Value::unit();
    }

    [[nodiscard]] std::expected<core::lang::Value, std::string> player(std::span<core::lang::Value const> arguments)
    {
        if (auto const open = beginStatement(); !open) return std::unexpected(open.error());
        if (!m_seed_set) return std::unexpected("scenario players must follow the seed");
        std::string const& name = arguments[0].text;
        char32_t const code_point = arguments[1].character;
        if (code_point > 0x7fU) return std::unexpected("player character must be one of @ # $ % &");
        char const character = static_cast<char>(code_point);
        uint8_t const x = static_cast<uint8_t>(arguments[2].integer[1U]);
        uint8_t const y = static_cast<uint8_t>(arguments[3].integer[1U]);
        if (!isIdentifier(name)) return std::unexpected("player name must be an ASCII scenario identifier");
        if (!isAllowedCharacter(character)) return std::unexpected("player character must be one of @ # $ % &");
        if (x > MAX_POSITION || y > MAX_POSITION) return std::unexpected("player position is outside flat2d-v1");
        if (m_actors.size() >= m_limits.max_actors) return std::unexpected("scenario actor limit exceeded");
        for (ScenarioActor const& actor : m_actors) {
            if (actor.name == name || actor.character == character) {
                return std::unexpected("player names and characters must be unique");
            }
            if (placementsConflict(actor, x, y)) return std::unexpected("player placement conflicts with an existing player");
        }
        m_actors.push_back(ScenarioActor{
            .id = static_cast<ScenarioActorId>(m_actors.size()),
            .name = name,
            .character = character,
            .x = x,
            .y = y,
            .location = { .line = 1, .column = 1 },
        });
        return core::lang::Value::unit();
    }

    [[nodiscard]] std::expected<core::lang::Value, std::string> input(std::span<core::lang::Value const> arguments)
    {
        if (auto const open = beginStatement(); !open) return std::unexpected(open.error());
        auto const actor = actorId(arguments[0].text);
        if (!actor) return std::unexpected("input references an unknown player");
        int8_t const x = signedByte(arguments[1]);
        int8_t const y = signedByte(arguments[2]);
        if (x < -1 || x > 1 || y < -1 || y > 1) return std::unexpected("input direction must be in -1..1");
        if (m_total_ticks == std::numeric_limits<uint64_t>::max()) {
            return std::unexpected("scenario tick boundary exceeds the supported range");
        }
        if (auto const operation = appendOperation(ScenarioOperationData{ScenarioInputOperation{
            .actor = *actor, .x = x, .y = y, .effective_boundary = m_total_ticks + 1U,
        }}); !operation) return std::unexpected(operation.error());
        return core::lang::Value::unit();
    }

    [[nodiscard]] std::expected<core::lang::Value, std::string> wait(std::span<core::lang::Value const> arguments)
    {
        if (auto const open = beginStatement(); !open) return std::unexpected(open.error());
        uint64_t const ticks = arguments[0].integer[1U];
        if (ticks == 0U || ticks > m_limits.max_total_ticks - m_total_ticks) {
            return std::unexpected("scenario tick limit exceeded");
        }
        if (auto const operation = appendOperation(ScenarioOperationData{ScenarioWaitOperation{.ticks = ticks}}); !operation) {
            return std::unexpected(operation.error());
        }
        m_total_ticks += ticks;
        return core::lang::Value::unit();
    }

    [[nodiscard]] std::expected<core::lang::Value, std::string> expectPosition(std::span<core::lang::Value const> arguments)
    {
        if (auto const open = beginStatement(); !open) return std::unexpected(open.error());
        auto const actor = actorId(arguments[0].text);
        uint8_t const x = static_cast<uint8_t>(arguments[1].integer[1U]);
        uint8_t const y = static_cast<uint8_t>(arguments[2].integer[1U]);
        if (!actor) return std::unexpected("expect_position references an unknown player");
        if (x > MAX_POSITION || y > MAX_POSITION) return std::unexpected("expected position is outside flat2d-v1");
        if (m_evidence_count >= m_limits.max_evidence) return std::unexpected("scenario evidence limit exceeded");
        if (auto const operation = appendOperation(ScenarioOperationData{ScenarioExpectPositionOperation{
            .actor = *actor, .x = x, .y = y,
        }}); !operation) return std::unexpected(operation.error());
        ++m_evidence_count;
        return core::lang::Value::unit();
    }

    [[nodiscard]] std::expected<ScenarioPlan, ScenarioDiagnostic> build() &&
    {
        if (isCancellationRequested()) return std::unexpected(diagnostic(
            ScenarioDiagnosticCode::Cancelled, m_filename, { .line = 1, .column = 1 }, "scenario compilation was cancelled"));
        if (!m_profile_set || !m_seed_set || m_actors.empty()) return std::unexpected(diagnostic(
            ScenarioDiagnosticCode::CoreLangRuntimeFailure, m_filename, { .line = 1, .column = 1 },
            "CoreLang scenario must declare profile, seed, and at least one player"));
        return ScenarioPlan{1U, ScenarioProfile::Flat2dV1, m_seed, std::move(m_actors), std::move(m_operations),
            m_total_ticks, m_evidence_count};
    }

    [[nodiscard]] std::expected<std::string, std::string> readLine() override
    { return std::unexpected("scenario scripts cannot read input"); }
    [[nodiscard]] std::expected<void, std::string> write(std::string_view) override
    { return std::unexpected("scenario scripts cannot write output"); }
    [[nodiscard]] bool isCancellationRequested() const noexcept override
    { return m_cancellation && m_cancellation->isCancellationRequested(); }

private:
    [[nodiscard]] std::expected<void, std::string> beginStatement()
    {
        if (isCancellationRequested()) return std::unexpected("scenario compilation was cancelled");
        if (m_statements >= m_limits.max_statements) return std::unexpected("scenario statement limit exceeded");
        ++m_statements;
        return {};
    }

    [[nodiscard]] std::optional<ScenarioActorId> actorId(std::string_view const name) const
    {
        auto const actor = std::ranges::find(m_actors, name, &ScenarioActor::name);
        return actor == m_actors.end() ? std::nullopt : std::optional{actor->id};
    }

    [[nodiscard]] static int8_t signedByte(core::lang::Value const& value) noexcept
    {
        uint64_t const magnitude = value.integer[1U];
        return static_cast<int8_t>(value.boolean ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude));
    }

    [[nodiscard]] std::expected<void, std::string> appendOperation(ScenarioOperationData data)
    {
        if (m_operations.size() >= m_limits.max_operations) return std::unexpected("scenario operation limit exceeded");
        m_operations.push_back(ScenarioOperation{
            .location = { .line = 1, .column = 1 }, .boundary = m_total_ticks, .data = std::move(data),
        });
        return {};
    }

private:
    std::string m_filename;
    ScenarioLimits const& m_limits;
    ScenarioCancellation const* m_cancellation;
    bool m_profile_set{false};
    bool m_seed_set{false};
    uint64_t m_seed{0U};
    uint64_t m_statements{0U};
    uint64_t m_total_ticks{0U};
    uint64_t m_evidence_count{0U};
    std::vector<ScenarioActor> m_actors;
    std::vector<ScenarioOperation> m_operations;
};

namespace {

template<typename Callback>
[[nodiscard]] core::lang::ExtensionDescriptor statementExtension(
    std::string name, std::vector<core::lang::Type> types, Callback callback
)
{
    return core::lang::ExtensionDescriptor{
        .name = std::move(name), .node_kind = core::lang::ExtensionNodeKind::CallStatement,
        .input_types = types, .output_type = core::lang::Type::Unit, .effect = core::lang::ExtensionEffect::Host,
        .child_types = std::move(types), .binding_shape = core::lang::ExtensionBindingShape::Immutable,
        .evaluate = [callback = std::move(callback)](
                        std::span<core::lang::Value const> arguments, core::lang::RuntimeServices& services
                    ) {
            auto* const collector = dynamic_cast<ScenarioPlanCollector*>(&services);
            return collector ? callback(*collector, arguments)
                : std::expected<core::lang::Value, std::string>{std::unexpected("invalid scenario runtime services")};
        },
    };
}

[[nodiscard]] core::lang::RuleSetDescriptor const& scenarioRuleset()
{
    static core::lang::RuleSetDescriptor const RULESET{
        .id = "MinecraftScenario", .version = 1U, .extensions = {
            statementExtension("profile", {core::lang::Type::Str}, [](auto& collector, auto arguments) {
                return collector.profile(arguments);
            }),
            statementExtension("seed", {core::lang::Type::U64}, [](auto& collector, auto arguments) {
                return collector.seed(arguments);
            }),
            statementExtension("player", {core::lang::Type::Str, core::lang::Type::C32,
                core::lang::Type::U8, core::lang::Type::U8}, [](auto& collector, auto arguments) {
                return collector.player(arguments);
            }),
            statementExtension("input", {core::lang::Type::Str, core::lang::Type::I8,
                core::lang::Type::I8}, [](auto& collector, auto arguments) {
                return collector.input(arguments);
            }),
            statementExtension("wait", {core::lang::Type::U64}, [](auto& collector, auto arguments) {
                return collector.wait(arguments);
            }),
            statementExtension("expect_position", {core::lang::Type::Str, core::lang::Type::U8,
                core::lang::Type::U8}, [](auto& collector, auto arguments) {
                return collector.expectPosition(arguments);
            }),
        }, .opaque_callbacks = true,
    };
    return RULESET;
}

} // namespace

class CoreLangScenarioLowerer final {
public:
    [[nodiscard]] static std::expected<ScenarioPlan, ScenarioDiagnostic> lower(
        std::string_view filename,
        std::string_view source,
        ScenarioLimits const& limits,
        ScenarioCancellation const* cancellation
    )
    {
        if (cancellation && cancellation->isCancellationRequested()) return std::unexpected(diagnostic(
            ScenarioDiagnosticCode::Cancelled, filename, { .line = 1, .column = 1 }, "scenario compilation was cancelled"));
        uint64_t directives = 0U;
        std::string_view remaining = source;
        while (!remaining.empty()) {
            size_t const end = remaining.find('\n');
            std::string_view const line = remaining.substr(0U, end);
            size_t const first = line.find_first_not_of(" \t\r");
            std::string_view const trimmed = first == std::string_view::npos ? std::string_view{} : line.substr(first);
            if (!trimmed.empty() && !trimmed.starts_with("//") && ++directives > limits.max_statements) {
                return std::unexpected(diagnostic(ScenarioDiagnosticCode::CoreLangCompileFailure, filename,
                    { .line = 1, .column = 1 }, "scenario statement limit exceeded"));
            }
            if (end == std::string_view::npos) break;
            remaining.remove_prefix(end + 1U);
        }
        auto const& ruleset = scenarioRuleset();
        auto const program = core::lang::compile(core::lang::CompileOptions{
            .root = {.id = std::string{filename}, .text = std::string{source}},
            .rulesets = std::span<core::lang::RuleSetDescriptor const>{&ruleset, 1U},
            .limits = {.max_source_bytes = limits.max_source_bytes, .max_modules = 1U,
                .max_tokens = limits.max_source_bytes, .max_nodes = limits.max_source_bytes},
        });
        if (!program) {
            core::lang::Diagnostic const& error = program.error().front();
            return std::unexpected(diagnostic(ScenarioDiagnosticCode::CoreLangCompileFailure, filename,
                {.line = error.location.line, .column = error.location.column}, error.message));
        }
        auto collector = ScenarioPlanCollector{filename, limits, cancellation};
        auto const executed = core::lang::execute(*program, collector, {.max_instructions = limits.max_source_bytes,
            .cancellation_check_interval = 1U});
        if (!executed) {
            ScenarioDiagnosticCode const code = executed.error().code == core::lang::RuntimeErrorCode::Cancelled
                ? ScenarioDiagnosticCode::Cancelled : ScenarioDiagnosticCode::CoreLangRuntimeFailure;
            auto const location = executed.error().location.value_or(core::lang::SourceSpan{});
            return std::unexpected(diagnostic(
                code, filename, {.line = location.line, .column = location.column}, executed.error().message
            ));
        }
        return std::move(collector).build();
    }
};

} // namespace scenario_detail

namespace {

[[nodiscard]] std::string_view firstHeader(std::string_view source) noexcept
{
    while (!source.empty()) {
        auto const first = source.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return {};
        source.remove_prefix(first);
        if (!source.starts_with("#") && !source.starts_with("//")) return source;
        auto const newline = source.find_first_of("\r\n");
        if (newline == std::string_view::npos) return {};
        source.remove_prefix(newline + 1U);
    }
    return {};
}

[[nodiscard]] bool isExplicitHeader(
    std::string_view const source,
    std::string_view const expected, bool const allow_semicolon = false
) noexcept
{
    if (!source.starts_with(expected)) return false;
    if (source.size() == expected.size()) return true;
    char const next = source[expected.size()];
    return next == ' ' || next == '\t' || next == '\r' || next == '\n' || (allow_semicolon && next == ';');
}

} // namespace

std::expected<ScenarioPlan, ScenarioDiagnostic> parseScenarioSource(
    std::string_view const filename,
    std::string_view const source,
    ScenarioLimits const& limits,
    ScenarioCancellation const* const cancellation
)
{
    if (limits.max_source_bytes == 0U || limits.max_statements == 0U || limits.max_actors == 0U
        || limits.max_total_ticks == 0U || limits.max_operations == 0U || limits.max_evidence == 0U) {
        return std::unexpected(scenario_detail::diagnostic(ScenarioDiagnosticCode::InvalidLimits, filename,
            {.line = 1, .column = 1}, "all scenario limits must be positive"));
    }
    if (source.size() > limits.max_source_bytes) return std::unexpected(scenario_detail::diagnostic(
        ScenarioDiagnosticCode::SourceTooLarge, filename, {.line = 1, .column = 1},
        "scenario source exceeds the configured byte limit"));
    if (cancellation && cancellation->isCancellationRequested()) return std::unexpected(scenario_detail::diagnostic(
        ScenarioDiagnosticCode::Cancelled, filename, {.line = 1, .column = 1}, "scenario compilation was cancelled"));
    std::string_view const header = firstHeader(source);
    if (isExplicitHeader(header, "scenario 1")) return parseScenario(filename, source, limits);
    if (isExplicitHeader(header, "@version(\"0.0.1\")", true)) return scenario_detail::CoreLangScenarioLowerer::lower(
        filename, source, limits, cancellation);
    return std::unexpected(scenario_detail::diagnostic(ScenarioDiagnosticCode::UnknownSourceHeader, filename,
        {.line = 1, .column = 1}, "expected scenario 1 or @version(\"0.0.1\") source header"));
}

} // namespace shared
