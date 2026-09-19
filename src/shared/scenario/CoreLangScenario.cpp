#include <shared/scenario/Scenario.hpp>

#include <core/lang/CoreLang.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace shared {

namespace scenario_detail {

namespace {

static constexpr std::string_view FLIGHT_PROFILE = "flight3d-v1";
static constexpr uint64_t HOST_ABI_MAJOR = 1U;

enum class HostCall : uint8_t {
    Profile,
    Seed,
    Player,
    Move,
    Camera,
    Wait,
    Expect,
};

[[nodiscard]]
ScenarioDiagnostic diagnostic(
    ScenarioDiagnosticCode const code,
    std::string_view const filename,
    ScenarioLocation const location,
    std::string message
)
{
    return {
        .code = code,
        .filename = std::string{filename},
        .location = location,
        .message = std::move(message),
    };
}

[[nodiscard]]
ScenarioLocation sourceLocation(std::string_view const source, uint64_t const offset) noexcept
{
    uint32_t line = 1U;
    uint32_t column = 1U;
    for (uint64_t index = 0U; index < offset && index < source.size(); ++index) {
        if (source[static_cast<std::string_view::size_type>(index)] == '\n') {
            ++line;
            column = 1U;
        } else {
            ++column;
        }
    }
    return {.line = line, .column = column};
}

[[nodiscard]]
core::lang::Type type(core::lang::TypeKind const kind)
{
    return {.kind = kind};
}

[[nodiscard]]
core::lang::Value unitValue()
{
    return {.type = type(core::lang::TypeKind::Unit)};
}

[[nodiscard]]
std::expected<void, std::string> count(
    std::span<core::lang::Value const> const arguments,
    uint64_t const expected
)
{
    if (arguments.size() != expected) {
        return std::unexpected("host call received the wrong number of arguments");
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
        return std::unexpected("host call received an invalid integer argument");
    }
    uint64_t result = 0U;
    for (uint8_t index = 0U; index < byte_count; ++index) {
        result |= static_cast<uint64_t>(value.bytes[index]) << (index * 8U);
    }
    return result;
}

[[nodiscard]]
std::expected<int32_t, std::string> signedValue(
    core::lang::Value const& value,
    core::lang::TypeKind const expected,
    uint8_t const byte_count
)
{
    auto const raw = unsignedValue(value, expected, byte_count);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    uint64_t const sign_bit = uint64_t{1U} << (byte_count * 8U - 1U);
    uint64_t const magnitude = *raw & (sign_bit - 1U);
    int64_t const result = *raw & sign_bit
        ? -static_cast<int64_t>(sign_bit) + static_cast<int64_t>(magnitude)
        : static_cast<int64_t>(magnitude);
    return static_cast<int32_t>(result);
}

[[nodiscard]]
std::expected<std::string, std::string> textValue(core::lang::Value const& value)
{
    if (value.type != type(core::lang::TypeKind::Str) || !value.elements.empty()) {
        return std::unexpected("host call received an invalid text argument");
    }
    std::string text;
    text.reserve(value.bytes.size());
    for (uint8_t const byte : value.bytes) {
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

[[nodiscard]]
bool isIdentifier(std::string_view const text) noexcept
{
    if (text.empty() || !((text.front() >= 'a' && text.front() <= 'z')
        || (text.front() >= 'A' && text.front() <= 'Z') || text.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(text.substr(1), [](char const character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '_';
    });
}

[[nodiscard]]
bool isAllowedCharacter(char const character) noexcept
{
    return character == '@' || character == '#' || character == '$' || character == '%' || character == '&';
}

} // namespace

class ScenarioPlanCollector final {
public:
    ScenarioPlanCollector(
        std::string_view const filename,
        ScenarioLimits const& limits,
        ScenarioCancellation const* const cancellation
    )
        : m_filename(filename)
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
            return std::unexpected("scenario compilation was cancelled");
        }
        if (m_statements >= m_limits.max_statements) {
            return std::unexpected("scenario statement limit exceeded");
        }
        ++m_statements;
        switch (host_call) {
            case HostCall::Profile:
                return profile(arguments);
            case HostCall::Seed:
                return seed(arguments);
            case HostCall::Player:
                return player(arguments);
            case HostCall::Move:
                return input(arguments, false);
            case HostCall::Camera:
                return input(arguments, true);
            case HostCall::Wait:
                return wait(arguments);
            case HostCall::Expect:
                return expect(arguments);
        }
        return std::unexpected("unknown scenario host call");
    }

    [[nodiscard]]
    std::expected<ScenarioPlan, ScenarioDiagnostic> build() &&
    {
        if (isCancelled()) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::Cancelled,
                m_filename,
                {.line = 1U, .column = 1U},
                "scenario compilation was cancelled"
            ));
        }
        if (!m_profile_set || !m_seed_set || m_actors.empty()) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::CoreLangRuntimeFailure,
                m_filename,
                {.line = 1U, .column = 1U},
                "CoreLang scenario must declare profile, seed, and at least one player"
            ));
        }
        return ScenarioPlan{
            1U,
            ScenarioProfile::Flight3dV1,
            m_seed,
            std::move(m_actors),
            std::move(m_operations),
            m_total_ticks,
            m_evidence_count,
        };
    }

private:
    [[nodiscard]]
    std::expected<void, std::string> profile(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 1U); !valid_count) {
            return valid_count;
        }
        auto const profile_name = textValue(arguments[0]);
        if (!profile_name) {
            return std::unexpected(profile_name.error());
        }
        if (m_profile_set || *profile_name != FLIGHT_PROFILE) {
            return std::unexpected("scenario profile must be specified once as flight3d-v1");
        }
        m_profile_set = true;
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> seed(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 1U); !valid_count) {
            return valid_count;
        }
        auto const value = unsignedValue(arguments[0], core::lang::TypeKind::U64, 8U);
        if (!value) {
            return std::unexpected(value.error());
        }
        if (!m_profile_set || m_seed_set) {
            return std::unexpected("scenario seed must follow profile and occur once");
        }
        m_seed = *value;
        m_seed_set = true;
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> player(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 8U); !valid_count) {
            return valid_count;
        }
        if (!m_seed_set) {
            return std::unexpected("scenario players must follow the seed");
        }
        auto const name = textValue(arguments[0]);
        auto const character = unsignedValue(arguments[1], core::lang::TypeKind::C8, 1U);
        auto const x = signedValue(arguments[2], core::lang::TypeKind::I32, 4U);
        auto const y = signedValue(arguments[3], core::lang::TypeKind::I32, 4U);
        auto const z = signedValue(arguments[4], core::lang::TypeKind::I32, 4U);
        auto const yaw = signedValue(arguments[5], core::lang::TypeKind::I16, 2U);
        auto const pitch = signedValue(arguments[6], core::lang::TypeKind::I16, 2U);
        auto const roll = signedValue(arguments[7], core::lang::TypeKind::I16, 2U);
        if (!name || !character || !x || !y || !z || !yaw || !pitch || !roll) {
            return std::unexpected("playerXYZ received an invalid typed argument");
        }
        char const player_character = static_cast<char>(*character);
        if (!isIdentifier(*name) || !isAllowedCharacter(player_character)) {
            return std::unexpected("playerXYZ received an invalid player identity");
        }
        if (*yaw < 0 || *yaw > 359 || *pitch < -89 || *pitch > 89 || *roll < -180 || *roll > 180) {
            return std::unexpected("playerXYZ orientation is outside the supported range");
        }
        if (static_cast<uint64_t>(m_actors.size()) >= m_limits.max_actors) {
            return std::unexpected("scenario actor limit exceeded");
        }
        for (ScenarioActor const& actor : m_actors) {
            if (actor.name == *name || actor.character == player_character) {
                return std::unexpected("player names and characters must be unique");
            }
        }
        m_actors.push_back({
            .id = static_cast<ScenarioActorId>(m_actors.size()),
            .name = std::move(*name),
            .character = player_character,
            .x = *x,
            .y = *y,
            .z = *z,
            .yaw_degrees = static_cast<int16_t>(*yaw),
            .pitch_degrees = static_cast<int16_t>(*pitch),
            .roll_degrees = static_cast<int16_t>(*roll),
            .location = {.line = 1U, .column = 1U},
        });
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> input(
        std::span<core::lang::Value const> const arguments,
        bool const camera
    )
    {
        if (auto const valid_count = count(arguments, 4U); !valid_count) {
            return valid_count;
        }
        auto const actor = actorId(arguments[0]);
        auto const first = signedValue(arguments[1], core::lang::TypeKind::I8, 1U);
        auto const second = signedValue(arguments[2], core::lang::TypeKind::I8, 1U);
        auto const third = signedValue(arguments[3], core::lang::TypeKind::I8, 1U);
        if (!actor || !first || !second || !third) {
            return std::unexpected("flight input received an invalid typed argument");
        }
        if (*first < -1 || *first > 1 || *second < -1 || *second > 1 || *third < -1 || *third > 1) {
            return std::unexpected("flight input direction must be in -1..1");
        }
        if (m_total_ticks == std::numeric_limits<uint64_t>::max()) {
            return std::unexpected("scenario tick boundary exceeds the supported range");
        }
        if (camera) {
            return append(ScenarioCameraInputOperation{
                .actor = *actor,
                .strafe = static_cast<int8_t>(*first),
                .forward = static_cast<int8_t>(*second),
                .vertical = static_cast<int8_t>(*third),
                .effective_boundary = m_total_ticks + 1U,
            });
        }
        return append(ScenarioInputOperation{
            .actor = *actor,
            .x = static_cast<int8_t>(*first),
            .y = static_cast<int8_t>(*second),
            .z = static_cast<int8_t>(*third),
            .effective_boundary = m_total_ticks + 1U,
        });
    }

    [[nodiscard]]
    std::expected<void, std::string> wait(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 1U); !valid_count) {
            return valid_count;
        }
        auto const ticks = unsignedValue(arguments[0], core::lang::TypeKind::U64, 8U);
        if (!ticks) {
            return std::unexpected(ticks.error());
        }
        if (*ticks == 0U || *ticks > m_limits.max_total_ticks - m_total_ticks) {
            return std::unexpected("scenario tick limit exceeded");
        }
        if (auto const appended = append(ScenarioWaitOperation{.ticks = *ticks}); !appended) {
            return appended;
        }
        m_total_ticks += *ticks;
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> expect(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 4U); !valid_count) {
            return valid_count;
        }
        auto const actor = actorId(arguments[0]);
        auto const x = signedValue(arguments[1], core::lang::TypeKind::I32, 4U);
        auto const y = signedValue(arguments[2], core::lang::TypeKind::I32, 4U);
        auto const z = signedValue(arguments[3], core::lang::TypeKind::I32, 4U);
        if (!actor || !x || !y || !z) {
            return std::unexpected("expectXYZ received an invalid typed argument");
        }
        if (m_evidence_count >= m_limits.max_evidence) {
            return std::unexpected("scenario evidence limit exceeded");
        }
        if (auto const appended = append(ScenarioExpectPositionOperation{
            .actor = *actor,
            .x = *x,
            .y = *y,
            .z = *z,
        }); !appended) {
            return appended;
        }
        ++m_evidence_count;
        return {};
    }

    [[nodiscard]]
    std::optional<ScenarioActorId> actorId(core::lang::Value const& value) const
    {
        auto const name = textValue(value);
        if (!name) {
            return std::nullopt;
        }
        auto const actor = std::ranges::find(m_actors, *name, &ScenarioActor::name);
        return actor == m_actors.end() ? std::nullopt : std::optional{actor->id};
    }

    template<typename Operation>
    [[nodiscard]]
    std::expected<void, std::string> append(Operation operation)
    {
        if (static_cast<uint64_t>(m_operations.size()) >= m_limits.max_operations) {
            return std::unexpected("scenario operation limit exceeded");
        }
        m_operations.push_back({
            .location = {.line = 1U, .column = 1U},
            .boundary = m_total_ticks,
            .data = std::move(operation),
        });
        return {};
    }

    [[nodiscard]]
    bool isCancelled() const noexcept
    {
        return m_cancellation && m_cancellation->isCancellationRequested();
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
        {"profile", HostCall::Profile, {type(TypeKind::Str)}},
        {"seed", HostCall::Seed, {type(TypeKind::U64)}},
        {"playerXYZ", HostCall::Player, {
            type(TypeKind::Str), type(TypeKind::C8), type(TypeKind::I32), type(TypeKind::I32),
            type(TypeKind::I32), type(TypeKind::I16), type(TypeKind::I16), type(TypeKind::I16),
        }},
        {"moveXYZ", HostCall::Move, {
            type(TypeKind::Str), type(TypeKind::I8), type(TypeKind::I8), type(TypeKind::I8),
        }},
        {"cameraInputXYZ", HostCall::Camera, {
            type(TypeKind::Str), type(TypeKind::I8), type(TypeKind::I8), type(TypeKind::I8),
        }},
        {"wait", HostCall::Wait, {type(TypeKind::U64)}},
        {"expectXYZ", HostCall::Expect, {
            type(TypeKind::Str), type(TypeKind::I32), type(TypeKind::I32), type(TypeKind::I32),
        }},
    };
}

[[nodiscard]]
core::lang::CustomManifest manifest(HostSpec const& spec)
{
    core::lang::CustomManifest result{
        .provider_key = "minecraft.scenario." + std::string{spec.name},
        .abi_major = HOST_ABI_MAJOR,
        .effect = core::lang::CustomEffect::Observable,
        .arguments = spec.arguments,
        .result = type(core::lang::TypeKind::Unit),
        .borrow = {.parameters = std::vector<core::lang::BorrowAccess>(spec.arguments.size())},
    };
    result.digest = core::lang::customManifestDigest(result);
    return result;
}

class CoreLangScenarioLowerer final {
public:
    [[nodiscard]]
    static std::expected<ScenarioPlan, ScenarioDiagnostic> lower(
        std::string_view const filename,
        std::string_view const source,
        ScenarioLimits const& limits,
        ScenarioCancellation const* const cancellation
    )
    {
        ScenarioPlanCollector collector{filename, limits, cancellation};
        std::vector<HostSpec> const specs = hostSpecs();
        core::lang::Ruleset ruleset{.id = "minecraft", .version = 1U};
        std::vector<core::lang::CustomProvider> providers;
        providers.reserve(specs.size());
        for (HostSpec const& spec : specs) {
            core::lang::CustomManifest const host_manifest = manifest(spec);
            ruleset.operations.push_back({
                .name = std::string{spec.name},
                .kind = core::lang::ExtensionKind::Builtin,
                .overrideExisting = spec.call == HostCall::Seed,
                .arguments = host_manifest.arguments,
                .result = host_manifest.result,
                .effect = host_manifest.effect,
                .custom = host_manifest,
                .unsafe_callable = host_manifest.unsafe_,
                .borrow = host_manifest.borrow,
            });
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
            });
        }
        core::lang::CompilerRegistry const registry{
            .rulesets = {std::move(ruleset)},
            .defaults = {"minecraft"},
        };
        auto const compiled = core::lang::compile(
            core::lang::Source{.id = std::string{filename}, .text = std::string{source}},
            registry
        );
        if (!compiled) {
            if (compiled.error().empty()) {
                return std::unexpected(diagnostic(
                    ScenarioDiagnosticCode::CoreLangCompileFailure,
                    filename,
                    {.line = 1U, .column = 1U},
                    "CoreLang compilation failed without diagnostics"
                ));
            }
            core::lang::Diagnostic const& error = compiled.error().front();
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::CoreLangCompileFailure,
                filename,
                sourceLocation(source, error.offset),
                error.message
            ));
        }
        core::lang::Runtime runtime;
        for (core::lang::CustomProvider& provider : providers) {
            auto const registered = runtime.registerProvider(std::move(provider));
            if (!registered) {
                return std::unexpected(diagnostic(
                    ScenarioDiagnosticCode::CoreLangRuntimeFailure,
                    filename,
                    {.line = 1U, .column = 1U},
                    registered.error().message
                ));
            }
        }
        auto const program = runtime.load(compiled->bytes);
        if (!program) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::CoreLangRuntimeFailure,
                filename,
                {.line = 1U, .column = 1U},
                program.error().message
            ));
        }
        auto const executed = runtime.execute(*program, filename, "scenario", {}, {});
        if (!executed) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::CoreLangRuntimeFailure,
                filename,
                {.line = 1U, .column = 1U},
                executed.error().message
            ));
        }
        if (auto const* const failed = std::get_if<core::lang::Failed>(&*executed)) {
            return std::unexpected(diagnostic(
                failed->failure.code == core::lang::FailureCode::Cancelled
                    ? ScenarioDiagnosticCode::Cancelled
                    : ScenarioDiagnosticCode::CoreLangRuntimeFailure,
                filename,
                {.line = 1U, .column = 1U},
                failed->failure.message
            ));
        }
        if (!std::holds_alternative<core::lang::Completed>(*executed)) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::CoreLangRuntimeFailure,
                filename,
                {.line = 1U, .column = 1U},
                "CoreLang scenario did not complete"
            ));
        }
        return std::move(collector).build();
    }
};

} // namespace scenario_detail

namespace {

[[nodiscard]]
std::string_view firstHeader(std::string_view source) noexcept
{
    while (!source.empty()) {
        auto const first = source.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            return {};
        }
        source.remove_prefix(first);
        if (!source.starts_with("#") && !source.starts_with("//")) {
            return source;
        }
        auto const newline = source.find_first_of("\r\n");
        if (newline == std::string_view::npos) {
            return {};
        }
        source.remove_prefix(newline + 1U);
    }
    return {};
}

[[nodiscard]]
bool isExplicitHeader(std::string_view const source, std::string_view const expected) noexcept
{
    if (!source.starts_with(expected)) {
        return false;
    }
    if (source.size() == expected.size()) {
        return true;
    }
    char const next = source[expected.size()];
    return next == ' ' || next == '\t' || next == '\r' || next == '\n';
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
        return std::unexpected(scenario_detail::diagnostic(
            ScenarioDiagnosticCode::InvalidLimits,
            filename,
            {.line = 1U, .column = 1U},
            "all scenario limits must be positive"
        ));
    }
    if (source.size() > limits.max_source_bytes) {
        return std::unexpected(scenario_detail::diagnostic(
            ScenarioDiagnosticCode::SourceTooLarge,
            filename,
            {.line = 1U, .column = 1U},
            "scenario source exceeds the configured byte limit"
        ));
    }
    if (cancellation && cancellation->isCancellationRequested()) {
        return std::unexpected(scenario_detail::diagnostic(
            ScenarioDiagnosticCode::Cancelled,
            filename,
            {.line = 1U, .column = 1U},
            "scenario compilation was cancelled"
        ));
    }
    std::string_view const header = firstHeader(source);
    if (isExplicitHeader(header, "@version(\"0.1.2\")")) {
        return scenario_detail::CoreLangScenarioLowerer::lower(filename, source, limits, cancellation);
    }
    return std::unexpected(scenario_detail::diagnostic(
        ScenarioDiagnosticCode::UnknownSourceHeader,
        filename,
        {.line = 1U, .column = 1U},
        "expected @version(\"0.1.2\") CoreLang source header"
    ));
}

} // namespace shared
