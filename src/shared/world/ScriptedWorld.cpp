#include <shared/world/ScriptedWorld.hpp>

#include <core/lang/CoreLang.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace core003 = core::lang;

static constexpr uint64_t WORLD_RULESET_VERSION = 1U;
static constexpr uint64_t WORLD_HOST_ABI_VERSION = 1U;
static constexpr uint8_t AIR_BLOCK_ID = static_cast<uint8_t>(shared::Block::Air);
static constexpr uint8_t STONE_BLOCK_ID = static_cast<uint8_t>(shared::Block::Stone);

[[nodiscard]]
core003::Type unitType()
{
    return {.kind = core003::TypeKind::Unit};
}

[[nodiscard]]
core003::Type signedIntegerType()
{
    return {.kind = core003::TypeKind::I32};
}

[[nodiscard]]
core003::Type unsignedSeedType()
{
    return {.kind = core003::TypeKind::U64};
}

[[nodiscard]]
core003::Value unitValue()
{
    return {.type = unitType()};
}

[[nodiscard]]
core003::Value signedIntegerValueForHost(int32_t const number)
{
    auto value = core003::Value{
        .type = signedIntegerType(),
        .bytes = std::vector<uint8_t>(sizeof(uint32_t)),
    };
    uint32_t const bits = static_cast<uint32_t>(number);
    for (uint64_t index = 0U; index < value.bytes.size(); ++index) {
        value.bytes[static_cast<std::vector<uint8_t>::size_type>(index)] = static_cast<uint8_t>(
            bits >> (index * 8U)
        );
    }
    return value;
}

[[nodiscard]]
core003::Value unsignedSeedValue(uint64_t const seed)
{
    auto value = core003::Value{
        .type = unsignedSeedType(),
        .bytes = std::vector<uint8_t>(sizeof(uint64_t)),
    };
    for (uint64_t index = 0U; index < value.bytes.size(); ++index) {
        value.bytes[static_cast<std::vector<uint8_t>::size_type>(index)] = static_cast<uint8_t>(
            seed >> (index * 8U)
        );
    }
    return value;
}

[[nodiscard]]
std::optional<uint64_t> unsignedValue(core003::Value const& value, core003::TypeKind const type)
{
    if (value.type.kind != type || value.bytes.empty() || value.bytes.size() > sizeof(uint64_t)) {
        return std::nullopt;
    }

    uint64_t result = 0U;
    for (uint64_t index = 0U; index < value.bytes.size(); ++index) {
        result |= static_cast<uint64_t>(value.bytes[static_cast<std::vector<uint8_t>::size_type>(index)])
            << (index * 8U);
    }
    return result;
}

[[nodiscard]]
std::optional<int32_t> signedIntegerValue(core003::Value const& value)
{
    std::optional<uint64_t> const raw = unsignedValue(value, core003::TypeKind::I32);
    if (!raw || value.bytes.size() != sizeof(uint32_t)) {
        return std::nullopt;
    }
    return static_cast<int32_t>(static_cast<uint32_t>(*raw));
}

[[nodiscard]]
core003::CustomManifest customManifest(
    std::string provider_key,
    std::vector<core003::Type> arguments,
    core003::Type result
)
{
    std::vector<core003::BorrowAccess> const parameter_borrows(
        arguments.size(),
        core003::BorrowAccess::None
    );
    auto manifest = core003::CustomManifest{
        .provider_key = std::move(provider_key),
        .abi_major = WORLD_HOST_ABI_VERSION,
        .effect = core003::CustomEffect::Opaque,
        .arguments = std::move(arguments),
        .result = std::move(result),
        .borrow = {.parameters = parameter_borrows},
    };
    manifest.digest = core003::customManifestDigest(manifest);
    return manifest;
}

[[nodiscard]]
core003::Operation customOperation(
    std::string name,
    std::string provider_key,
    std::vector<core003::Type> arguments,
    core003::Type result
)
{
    core003::CustomManifest manifest = customManifest(provider_key, arguments, result);
    core003::BorrowContract const borrow = manifest.borrow;
    return {
        .name = std::move(name),
        .arguments = std::move(arguments),
        .result = std::move(result),
        .effect = core003::CustomEffect::Opaque,
        .custom = std::move(manifest),
        .borrow = borrow,
    };
}

[[nodiscard]]
core003::CompilerRegistry worldCompilerRegistry()
{
    core003::Ruleset ruleset{
        .id = "minecraft",
        .version = WORLD_RULESET_VERSION,
        .operations = {
            customOperation(
                "terrain_random",
                "minecraft/terrain_random",
                {unsignedSeedType(), signedIntegerType(), signedIntegerType(), signedIntegerType()},
                signedIntegerType()
            ),
            customOperation(
                "set_block",
                "minecraft/set_block",
                {signedIntegerType(), signedIntegerType(), signedIntegerType(), signedIntegerType()},
                unitType()
            ),
            customOperation("publish", "minecraft/publish", {}, unitType()),
        },
    };
    return {
        .rulesets = {std::move(ruleset)},
        .defaults = {"minecraft"},
    };
}

class CandidateWorld final {
public:
    explicit CandidateWorld(shared::ScriptedWorldOptions options)
        : m_options(std::move(options))
    {
        m_blocks.fill(shared::Block::Air);
    }

    [[nodiscard]] core003::CustomOutcome terrainRandom(std::span<core003::Value const> arguments)
    {
        if (!consumeHostCall()) {
            return fail("world script exceeded its host callback limit");
        }
        if (arguments.size() != 4U) {
            return fail("terrain_random received invalid arguments");
        }
        std::optional<uint64_t> const seed = unsignedValue(arguments[0U], core003::TypeKind::U64);
        std::optional<int32_t> const x = signedIntegerValue(arguments[1U]);
        std::optional<int32_t> const y = signedIntegerValue(arguments[2U]);
        std::optional<int32_t> const z = signedIntegerValue(arguments[3U]);
        if (!seed || !x || !y || !z || *seed != m_options.seed) {
            return fail("terrain_random received invalid values");
        }
        std::optional<shared::BlockCoordinate> const coordinate = blockCoordinate(*x, *y, *z);
        if (!coordinate) {
            return fail("terrain_random coordinate is outside the configured chunk");
        }

        int8_t const sample = shared::Chunk::sampleFixtureNoise(
            m_options.chunk_coordinate,
            *coordinate,
            m_options.seed
        );
        return core003::CustomComplete{.value = signedIntegerValueForHost(sample)};
    }

    [[nodiscard]] core003::CustomOutcome setBlock(std::span<core003::Value const> arguments)
    {
        if (!consumeHostCall()) {
            return fail("world script exceeded its host callback limit");
        }
        if (m_published || arguments.size() != 4U) {
            return fail("set_block was called after publication or with invalid arguments");
        }
        std::optional<int32_t> const x = signedIntegerValue(arguments[0U]);
        std::optional<int32_t> const y = signedIntegerValue(arguments[1U]);
        std::optional<int32_t> const z = signedIntegerValue(arguments[2U]);
        std::optional<int32_t> const block_id = signedIntegerValue(arguments[3U]);
        std::optional<shared::BlockCoordinate> const coordinate = x && y && z
            ? blockCoordinate(*x, *y, *z)
            : std::nullopt;
        if (!coordinate || !block_id || (*block_id != AIR_BLOCK_ID && *block_id != STONE_BLOCK_ID)) {
            return fail("set_block requires an in-bounds Air or Stone block definition");
        }

        m_blocks[blockIndex(*coordinate)] = static_cast<shared::Block>(*block_id);
        return core003::CustomComplete{.value = unitValue()};
    }

    [[nodiscard]] core003::CustomOutcome publish(std::span<core003::Value const> arguments)
    {
        if (!consumeHostCall()) {
            return fail("world script exceeded its host callback limit");
        }
        if (m_published || !arguments.empty()) {
            return fail("publish must occur once without arguments");
        }
        m_published = true;
        return core003::CustomComplete{.value = unitValue()};
    }

    [[nodiscard]] std::expected<shared::Chunk, shared::ScriptedWorldError> build()
    {
        if (!m_published) {
            return std::unexpected(shared::ScriptedWorldError{
                .code = shared::ScriptedWorldErrorCode::IncompletePublication,
                .message = "world script completed without publishing its candidate",
            });
        }
        return shared::Chunk{m_options.chunk_coordinate, std::move(m_blocks)};
    }

    [[nodiscard]] bool exceededHostCallLimit() const noexcept
    {
        return m_host_call_limit_exceeded;
    }

private:
    [[nodiscard]] bool consumeHostCall() noexcept
    {
        if (m_host_calls >= m_options.max_host_calls) {
            m_host_call_limit_exceeded = true;
            return false;
        }
        ++m_host_calls;
        return true;
    }

    [[nodiscard]] static std::optional<shared::BlockCoordinate> blockCoordinate(
        int32_t const x,
        int32_t const y,
        int32_t const z
    ) noexcept
    {
        if (x < 0 || y < 0 || z < 0 || x >= shared::Chunk::SIDE_LENGTH || y >= shared::Chunk::SIDE_LENGTH
            || z >= shared::Chunk::SIDE_LENGTH) {
            return std::nullopt;
        }
        return shared::BlockCoordinate{
            .x = static_cast<uint8_t>(x),
            .y = static_cast<uint8_t>(y),
            .z = static_cast<uint8_t>(z),
        };
    }

    [[nodiscard]] static uint32_t blockIndex(shared::BlockCoordinate const coordinate) noexcept
    {
        return static_cast<uint32_t>(coordinate.z) * shared::Chunk::FACE_BLOCK_COUNT
            + static_cast<uint32_t>(coordinate.y) * shared::Chunk::SIDE_LENGTH + coordinate.x;
    }

    [[nodiscard]] core003::CustomOutcome fail(std::string message) const
    {
        return core003::CustomFail{.code = 1U, .message = std::move(message)};
    }

private:
    shared::ScriptedWorldOptions m_options;
    shared::Chunk::Blocks m_blocks;
    uint64_t m_host_calls{0U};
    bool m_host_call_limit_exceeded{false};
    bool m_published{false};
};

[[nodiscard]]
std::expected<void, shared::ScriptedWorldError> registerProviders(
    core003::Runtime& runtime,
    CandidateWorld& candidate
)
{
    auto register_provider = [&runtime](core003::CustomProvider provider)
        -> std::expected<void, shared::ScriptedWorldError> {
        auto const result = runtime.registerProvider(std::move(provider));
        if (!result) {
            return std::unexpected(shared::ScriptedWorldError{
                .code = shared::ScriptedWorldErrorCode::RuntimeFailed,
                .message = result.error().message,
            });
        }
        return {};
    };

    if (auto const result = register_provider({
            .manifest = customManifest(
                "minecraft/terrain_random",
                {unsignedSeedType(), signedIntegerType(), signedIntegerType(), signedIntegerType()},
                signedIntegerType()
            ),
            .invoke = [&candidate](core003::CustomContext&, std::span<core003::Value const> arguments) {
                return candidate.terrainRandom(arguments);
            },
        }); !result) {
        return result;
    }
    if (auto const result = register_provider({
            .manifest = customManifest(
                "minecraft/set_block",
                {signedIntegerType(), signedIntegerType(), signedIntegerType(), signedIntegerType()},
                unitType()
            ),
            .invoke = [&candidate](core003::CustomContext&, std::span<core003::Value const> arguments) {
                return candidate.setBlock(arguments);
            },
        }); !result) {
        return result;
    }
    return register_provider({
        .manifest = customManifest("minecraft/publish", {}, unitType()),
        .invoke = [&candidate](core003::CustomContext&, std::span<core003::Value const> arguments) {
            return candidate.publish(arguments);
        },
    });
}

[[nodiscard]]
std::string firstDiagnostic(std::vector<core003::Diagnostic> const& diagnostics)
{
    if (diagnostics.empty()) {
        return "CoreLang rejected the world script without a diagnostic";
    }
    return diagnostics.front().code + ": " + diagnostics.front().message;
}

[[nodiscard]]
std::string outcomeMessage(core003::CallOutcome const& outcome)
{
    if (auto const* const failure = std::get_if<core003::Failed>(&outcome)) {
        return failure->failure.message;
    }
    if (std::holds_alternative<core003::Halted>(outcome)) {
        return "world script halted before publication";
    }
    if (std::holds_alternative<core003::Suspended>(outcome)) {
        return "world script suspended before publication";
    }
    return "world script generation did not complete";
}

} // namespace

namespace shared {

ScriptedWorld::ScriptedWorld(Chunk chunk, WorldConfiguration const configuration) noexcept
    : m_chunk(std::move(chunk))
    , m_configuration(configuration)
{
}

std::expected<ScriptedWorld, ScriptedWorldError> ScriptedWorld::load(
    std::string_view const source,
    ScriptedWorldOptions const options
)
{
    if (!isValidScriptedWorldOptions(options)) {
        return std::unexpected(ScriptedWorldError{
            .code = ScriptedWorldErrorCode::InvalidOptions,
            .message = "scripted worlds require positive bounds and exactly one 16 by 16 by 16 chunk",
        });
    }
    if (source.size() > options.max_source_bytes) {
        return std::unexpected(ScriptedWorldError{
            .code = ScriptedWorldErrorCode::SourceTooLarge,
            .message = "world script exceeds its configured source bound",
        });
    }

    core003::CompilerRegistry const registry = worldCompilerRegistry();
    auto const compiled = core003::compile(
        {.id = "canonical_world", .text = std::string{source}},
        registry,
        {}
    );
    if (!compiled) {
        return std::unexpected(ScriptedWorldError{
            .code = ScriptedWorldErrorCode::CompilationFailed,
            .message = firstDiagnostic(compiled.error()),
        });
    }

    CandidateWorld candidate{options};
    core003::Runtime runtime;
    if (auto const registered = registerProviders(runtime, candidate); !registered) {
        return std::unexpected(registered.error());
    }
    auto const program = runtime.load(compiled->bytes);
    if (!program) {
        return std::unexpected(ScriptedWorldError{
            .code = ScriptedWorldErrorCode::RuntimeFailed,
            .message = program.error().message,
        });
    }
    std::array<core003::Type, 1U> const parameter_types{unsignedSeedType()};
    std::array<core003::Value, 1U> const arguments{unsignedSeedValue(options.seed)};
    auto const outcome = runtime.execute(
        *program,
        "canonical_world",
        "generate",
        parameter_types,
        arguments
    );
    if (!outcome || !std::holds_alternative<core003::Completed>(*outcome)) {
        return std::unexpected(ScriptedWorldError{
            .code = candidate.exceededHostCallLimit()
                ? ScriptedWorldErrorCode::HostCallLimitExceeded
                : ScriptedWorldErrorCode::RuntimeFailed,
            .message = outcome ? outcomeMessage(*outcome) : outcome.error().message,
        });
    }

    auto chunk = candidate.build();
    if (!chunk) {
        return std::unexpected(chunk.error());
    }
    uint64_t const digest = chunk->contentIdentity().content_hash;
    return ScriptedWorld{
        std::move(*chunk),
        WorldConfiguration{
            .seed = options.seed,
            .chunk_width = options.chunk_width,
            .chunk_height = options.chunk_height,
            .chunk_depth = options.chunk_depth,
            .chunk_content_digest = digest,
        },
    };
}

Chunk const& ScriptedWorld::chunk() const noexcept
{
    return m_chunk;
}

WorldConfiguration const& ScriptedWorld::configuration() const noexcept
{
    return m_configuration;
}

} // namespace shared
