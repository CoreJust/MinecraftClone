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
static constexpr std::string_view SPARSE_WORLD_PROFILE = "sparse-world-v1";
static constexpr uint64_t HOST_ABI_MAJOR = 1U;
static constexpr uint32_t SPARSE_WORLD_GENERATOR_VERSION = 1U;

enum class HostCall : uint8_t {
    Profile,
    Seed,
    Player,
    Move,
    Flight,
    Phase,
    Camera,
    MovementPermissions,
    Jump,
    Wait,
    Expect,
    ExpectMovementPermissions,
    ExpectVerticalVelocity,
    SparseWorldOptions,
    ExpectBlock,
    ExpectResidentChunks,
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
    return {.kind = kind, .elements = {}};
}

[[nodiscard]]
core::lang::Value unitValue()
{
    return {.type = type(core::lang::TypeKind::Unit), .bytes = {}, .elements = {}};
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
std::expected<int64_t, std::string> signed64Value(core::lang::Value const& value)
{
    auto const raw = unsignedValue(value, core::lang::TypeKind::I64, 8U);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    uint64_t const sign_bit = uint64_t{1U} << 63U;
    uint64_t const magnitude = *raw & (sign_bit - 1U);
    if (*raw & sign_bit) {
        return std::numeric_limits<int64_t>::min() + static_cast<int64_t>(magnitude);
    }
    return static_cast<int64_t>(*raw);
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
std::expected<bool, std::string> booleanValue(core::lang::Value const& value)
{
    if (value.type != type(core::lang::TypeKind::Bool) || value.bytes.size() != 1U
        || !value.elements.empty() || value.bytes.front() > 1U) {
        return std::unexpected("host call received an invalid boolean argument");
    }
    return value.bytes.front() != 0U;
}

[[nodiscard]]
bool isIdentifier(std::string_view text) noexcept
{
    if (text.empty() || !((text.front() >= 'a' && text.front() <= 'z')
        || (text.front() >= 'A' && text.front() <= 'Z') || text.front() == '_')) {
        return false;
    }
    text.remove_prefix(1U);
    return std::ranges::all_of(text, [](char const character) {
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
        ScenarioCancellation const* const cancellation,
        bool const supports_sparse_world
    )
        : m_filename(filename)
        , m_limits(limits)
        , m_cancellation(cancellation)
        , m_supports_sparse_world(supports_sparse_world)
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
                return input(arguments, false, ScenarioInputOperation::Intent::Direct);
            case HostCall::Flight:
                return input(arguments, false, ScenarioInputOperation::Intent::Flight);
            case HostCall::Phase:
                return input(arguments, false, ScenarioInputOperation::Intent::Phase);
            case HostCall::Camera:
                return input(arguments, true, ScenarioInputOperation::Intent::Direct);
            case HostCall::MovementPermissions:
                return movementPermissions(arguments);
            case HostCall::Jump:
                return jump(arguments);
            case HostCall::Wait:
                return wait(arguments);
            case HostCall::Expect:
                return expect(arguments);
            case HostCall::ExpectMovementPermissions:
                return expectMovementPermissions(arguments);
            case HostCall::ExpectVerticalVelocity:
                return expectVerticalVelocity(arguments);
            case HostCall::SparseWorldOptions:
                return sparseWorldOptions(arguments);
            case HostCall::ExpectBlock:
                return expectBlock(arguments);
            case HostCall::ExpectResidentChunks:
                return expectResidentChunks(arguments);
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
        bool const sparse_world_profile = m_profile == ScenarioProfile::SparseWorldV1;
        if (!m_profile_set || !m_seed_set
            || (sparse_world_profile && (!m_world_options_set || !m_actors.empty() || m_total_ticks != 0U))
            || (!sparse_world_profile && (m_actors.empty() || m_world_options_set))) {
            return std::unexpected(diagnostic(
                ScenarioDiagnosticCode::CoreLangRuntimeFailure,
                m_filename,
                {.line = 1U, .column = 1U},
                "CoreLang scenario declarations do not match its selected profile"
            ));
        }
        return ScenarioPlan{
            1U,
            m_profile,
            m_seed,
            std::move(m_actors),
            std::move(m_operations),
            ScenarioPlan::Counts{
                .total_ticks = m_total_ticks,
                .evidence_count = m_evidence_count,
            },
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
        if (m_profile_set) {
            return std::unexpected("scenario profile must be specified once");
        }
        if (*profile_name == FLIGHT_PROFILE) {
            m_profile = ScenarioProfile::Flight3dV1;
        } else if (m_supports_sparse_world && *profile_name == SPARSE_WORLD_PROFILE) {
            m_profile = ScenarioProfile::SparseWorldV1;
        } else {
            return std::unexpected("scenario profile is unsupported by this CoreLang contract");
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
        if (m_profile == ScenarioProfile::SparseWorldV1) {
            return std::unexpected("sparse-world scenarios cannot declare players");
        }
        if (!m_seed_set) {
            return std::unexpected("scenario players must follow the seed");
        }
        auto name = textValue(arguments[0]);
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
        bool const camera,
        ScenarioInputOperation::Intent const intent
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
            .intent = intent,
        });
    }

    [[nodiscard]]
    std::expected<void, std::string> movementPermissions(
        std::span<core::lang::Value const> const arguments
    )
    {
        if (auto const valid_count = count(arguments, 2U); !valid_count) {
            return valid_count;
        }
        auto const flight = booleanValue(arguments[0]);
        auto const collision_bypass = booleanValue(arguments[1]);
        if (!flight || !collision_bypass) {
            return std::unexpected("movementPermissions received an invalid boolean argument");
        }
        if (m_profile == ScenarioProfile::SparseWorldV1) {
            return std::unexpected("sparse-world scenarios do not support movement permissions");
        }
        if (*collision_bypass && !*flight) {
            return std::unexpected("collision bypass requires flight permission");
        }
        return append(ScenarioMovementPermissionsOperation{
            .flight = *flight,
            .collision_bypass = *collision_bypass,
        });
    }

    [[nodiscard]]
    std::expected<void, std::string> jump(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 1U); !valid_count) {
            return valid_count;
        }
        auto const actor = actorId(arguments[0]);
        if (!actor) {
            return std::unexpected("jump references an unknown player");
        }
        if (m_total_ticks >= m_limits.max_total_ticks) {
            return std::unexpected("scenario tick limit exceeded");
        }
        if (auto const appended = append(ScenarioJumpOperation{.actor = *actor}); !appended) {
            return appended;
        }
        ++m_total_ticks;
        return {};
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
        if (m_profile == ScenarioProfile::SparseWorldV1) {
            return std::unexpected("sparse-world scenarios do not support tick waits");
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
    std::expected<void, std::string> expectMovementPermissions(
        std::span<core::lang::Value const> const arguments
    )
    {
        if (auto const valid_count = count(arguments, 3U); !valid_count) {
            return valid_count;
        }
        auto const actor = actorId(arguments[0]);
        auto const flight = booleanValue(arguments[1]);
        auto const collision_bypass = booleanValue(arguments[2]);
        if (!actor || !flight || !collision_bypass) {
            return std::unexpected("expectMovementPermissions received an invalid argument");
        }
        if (*collision_bypass && !*flight) {
            return std::unexpected("collision bypass requires flight permission");
        }
        if (m_evidence_count >= m_limits.max_evidence) {
            return std::unexpected("scenario evidence limit exceeded");
        }
        if (auto const appended = append(ScenarioExpectMovementPermissionsOperation{
                .actor = *actor,
                .flight = *flight,
                .collision_bypass = *collision_bypass,
            }); !appended) {
            return appended;
        }
        ++m_evidence_count;
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> expectVerticalVelocity(
        std::span<core::lang::Value const> const arguments
    )
    {
        if (auto const valid_count = count(arguments, 2U); !valid_count) {
            return valid_count;
        }
        auto const actor = actorId(arguments[0]);
        auto const velocity = signedValue(arguments[1], core::lang::TypeKind::I32, 4U);
        if (!actor || !velocity) {
            return std::unexpected("expectVerticalVelocity received an invalid argument");
        }
        if (m_evidence_count >= m_limits.max_evidence) {
            return std::unexpected("scenario evidence limit exceeded");
        }
        if (auto const appended = append(ScenarioExpectVerticalVelocityOperation{
                .actor = *actor,
                .velocity_subcells = *velocity,
            }); !appended) {
            return appended;
        }
        ++m_evidence_count;
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> sparseWorldOptions(
        std::span<core::lang::Value const> const arguments
    )
    {
        if (auto const valid_count = count(arguments, 2U); !valid_count) {
            return valid_count;
        }
        auto const generator_version = unsignedValue(arguments[0], core::lang::TypeKind::U32, 4U);
        auto const max_resident_chunks = unsignedValue(arguments[1], core::lang::TypeKind::U64, 8U);
        if (!generator_version || !max_resident_chunks) {
            return std::unexpected("sparseWorldOptions received an invalid typed argument");
        }
        if (!m_supports_sparse_world || !m_profile_set || !m_seed_set
            || m_profile != ScenarioProfile::SparseWorldV1 || m_world_options_set) {
            return std::unexpected("sparseWorldOptions must follow the sparse-world profile and seed once");
        }
        if (*generator_version != SPARSE_WORLD_GENERATOR_VERSION) {
            return std::unexpected("sparse-world generator version is unsupported");
        }
        if (*max_resident_chunks == 0U
            || *max_resident_chunks > m_limits.max_sparse_world_resident_chunks) {
            return std::unexpected("sparse-world resident chunk bound is outside the configured limits");
        }
        if (auto const appended = append(ScenarioSparseWorldOptionsOperation{
                .generator_version = static_cast<uint32_t>(*generator_version),
                .max_resident_chunks = *max_resident_chunks,
            }); !appended) {
            return appended;
        }
        m_max_resident_chunks = *max_resident_chunks;
        m_world_options_set = true;
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> expectBlock(std::span<core::lang::Value const> const arguments)
    {
        if (auto const valid_count = count(arguments, 4U); !valid_count) {
            return valid_count;
        }
        auto const x = signed64Value(arguments[0]);
        auto const y = signed64Value(arguments[1]);
        auto const z = signed64Value(arguments[2]);
        auto const block = unsignedValue(arguments[3], core::lang::TypeKind::U8, 1U);
        if (!x || !y || !z || !block) {
            return std::unexpected("expectBlockXYZ received an invalid typed argument");
        }
        if (!sparseWorldReady()) {
            return std::unexpected("expectBlockXYZ requires configured sparse-world options");
        }
        if (*block > static_cast<uint64_t>(Block::Stone)) {
            return std::unexpected("expectBlockXYZ received an unsupported block id");
        }
        if (m_evidence_count >= m_limits.max_evidence) {
            return std::unexpected("scenario evidence limit exceeded");
        }
        if (auto const appended = append(ScenarioExpectBlockOperation{
                .x = *x,
                .y = *y,
                .z = *z,
                .block = static_cast<Block>(*block),
            }); !appended) {
            return appended;
        }
        ++m_evidence_count;
        return {};
    }

    [[nodiscard]]
    std::expected<void, std::string> expectResidentChunks(
        std::span<core::lang::Value const> const arguments
    )
    {
        if (auto const valid_count = count(arguments, 1U); !valid_count) {
            return valid_count;
        }
        auto const count_value = unsignedValue(arguments[0], core::lang::TypeKind::U64, 8U);
        if (!count_value) {
            return std::unexpected(count_value.error());
        }
        if (!sparseWorldReady()) {
            return std::unexpected("expectResidentChunks requires configured sparse-world options");
        }
        if (*count_value > m_max_resident_chunks) {
            return std::unexpected("expected resident chunk count exceeds the configured bound");
        }
        if (m_evidence_count >= m_limits.max_evidence) {
            return std::unexpected("scenario evidence limit exceeded");
        }
        if (auto const appended = append(ScenarioExpectResidentChunksOperation{
                .count = *count_value,
            }); !appended) {
            return appended;
        }
        ++m_evidence_count;
        return {};
    }

    [[nodiscard]]
    bool sparseWorldReady() const noexcept
    {
        return m_supports_sparse_world
            && m_profile_set
            && m_profile == ScenarioProfile::SparseWorldV1
            && m_seed_set
            && m_world_options_set;
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
    bool m_supports_sparse_world;
    bool m_profile_set{false};
    bool m_seed_set{false};
    bool m_world_options_set{false};
    ScenarioProfile m_profile{ScenarioProfile::Flat2dV1};
    uint64_t m_seed{0U};
    uint64_t m_max_resident_chunks{0U};
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
std::vector<HostSpec> hostSpecs(bool const supports_sparse_world)
{
    using TypeKind = core::lang::TypeKind;
    std::vector<HostSpec> specs{
        {"profile", HostCall::Profile, {type(TypeKind::Str)}},
        {"seed", HostCall::Seed, {type(TypeKind::U64)}},
        {"playerXYZ", HostCall::Player, {
            type(TypeKind::Str), type(TypeKind::C8), type(TypeKind::I32), type(TypeKind::I32),
            type(TypeKind::I32), type(TypeKind::I16), type(TypeKind::I16), type(TypeKind::I16),
        }},
        {"moveXYZ", HostCall::Move, {
            type(TypeKind::Str), type(TypeKind::I8), type(TypeKind::I8), type(TypeKind::I8),
        }},
        {"flightXYZ", HostCall::Flight, {
            type(TypeKind::Str), type(TypeKind::I8), type(TypeKind::I8), type(TypeKind::I8),
        }},
        {"phaseXYZ", HostCall::Phase, {
            type(TypeKind::Str), type(TypeKind::I8), type(TypeKind::I8), type(TypeKind::I8),
        }},
        {"cameraInputXYZ", HostCall::Camera, {
            type(TypeKind::Str), type(TypeKind::I8), type(TypeKind::I8), type(TypeKind::I8),
        }},
        {"movementPermissions", HostCall::MovementPermissions, {
            type(TypeKind::Bool), type(TypeKind::Bool),
        }},
        {"jump", HostCall::Jump, {type(TypeKind::Str)}},
        {"wait", HostCall::Wait, {type(TypeKind::U64)}},
        {"expectXYZ", HostCall::Expect, {
            type(TypeKind::Str), type(TypeKind::I32), type(TypeKind::I32), type(TypeKind::I32),
        }},
        {"expectMovementPermissions", HostCall::ExpectMovementPermissions, {
            type(TypeKind::Str), type(TypeKind::Bool), type(TypeKind::Bool),
        }},
        {"expectVerticalVelocity", HostCall::ExpectVerticalVelocity, {
            type(TypeKind::Str), type(TypeKind::I32),
        }},
    };
    if (supports_sparse_world) {
        specs.push_back({"sparseWorldOptions", HostCall::SparseWorldOptions, {
            type(TypeKind::U32), type(TypeKind::U64),
        }});
        specs.push_back({"expectBlockXYZ", HostCall::ExpectBlock, {
            type(TypeKind::I64), type(TypeKind::I64), type(TypeKind::I64), type(TypeKind::U8),
        }});
        specs.push_back({"expectResidentChunks", HostCall::ExpectResidentChunks, {
            type(TypeKind::U64),
        }});
    }
    return specs;
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
        .borrow = {
            .parameters = std::vector<core::lang::BorrowAccess>(spec.arguments.size()),
            .returns = {},
        },
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
        ScenarioCancellation const* const cancellation,
        bool const supports_sparse_world
    )
    {
        ScenarioPlanCollector collector{filename, limits, cancellation, supports_sparse_world};
        std::vector<HostSpec> const specs = hostSpecs(supports_sparse_world);
        core::lang::Ruleset ruleset{
            .id = "minecraft",
            .version = 1U,
            .restrictions = {},
        };
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
                .expand = {},
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
                .cancel = {},
            });
        }
        core::lang::CompilerRegistry const registry{
            .rulesets = {std::move(ruleset)},
            .defaults = {"minecraft"},
            .restrictions = {},
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
        if (!source.starts_with('#') && !source.starts_with("//")) {
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
        || limits.max_total_ticks == 0U || limits.max_operations == 0U || limits.max_evidence == 0U
        || limits.max_sparse_world_resident_chunks == 0U) {
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
        return scenario_detail::CoreLangScenarioLowerer::lower(filename, source, limits, cancellation, false);
    }
    if (isExplicitHeader(header, "@version(\"0.1.3\")")) {
        return scenario_detail::CoreLangScenarioLowerer::lower(filename, source, limits, cancellation, true);
    }
    return std::unexpected(scenario_detail::diagnostic(
        ScenarioDiagnosticCode::UnknownSourceHeader,
        filename,
        {.line = 1U, .column = 1U},
        "expected @version(\"0.1.2\") or @version(\"0.1.3\") CoreLang source header"
    ));
}

} // namespace shared
