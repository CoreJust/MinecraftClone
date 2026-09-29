#include <shared/world/WorldGeneration.hpp>

#include <shared/HeightTileScript.hpp>
#include <shared/world/SparseWorld.hpp>

#include <core/lang/CoreLang.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr uint32_t HEIGHT_TILE_INDEX_MULTIPLIER = shared::HeightTile::SIDE_LENGTH;
constexpr double WORLD_CENTER = 32'768.0;
constexpr uint16_t MAXIMUM_HEIGHT = static_cast<uint16_t>(shared::WorldExtent::DEPTH);

[[nodiscard]]
uint16_t kernelHeightAt(
    int64_t const x,
    int64_t const y,
    double const base,
    double const spacing,
    double const amplitude
) noexcept {
    double const dx = static_cast<double>(x) - WORLD_CENTER;
    double const dy = static_cast<double>(y) - WORLD_CENTER;
    double const distance = std::sqrt(dx * dx + dy * dy);
    double const n = std::floor(distance / spacing + 0.5);
    double const angle = std::numbers::pi_v<double> * distance / spacing;
    double const height = std::floor(base + amplitude / (n + 1.0) * std::cos(angle) * std::cos(angle));
    return static_cast<uint16_t>(std::clamp(height, 0.0, static_cast<double>(MAXIMUM_HEIGHT)));
}

[[nodiscard]]
uint32_t sampleIndex(shared::BlockCoordinate const coordinate) noexcept
{
    return static_cast<uint32_t>(coordinate.y) * HEIGHT_TILE_INDEX_MULTIPLIER + coordinate.x;
}

enum class GenerationPlanHostCall : uint8_t {
    Version,
    Pregen,
    Refinement,
    VirtualStage,
    Priority,
};

struct GenerationPlanHostSpec final {
    std::string_view name;
    GenerationPlanHostCall call;
    std::vector<core::lang::Type> arguments;
};

[[nodiscard]]
core::lang::Type coreType(core::lang::TypeKind const kind)
{
    return {.kind = kind};
}

[[nodiscard]]
core::lang::Value unitValue()
{
    return {.type = coreType(core::lang::TypeKind::Unit)};
}

[[nodiscard]]
std::optional<uint64_t> unsignedCoreValue(
    core::lang::Value const& value,
    core::lang::TypeKind const expected_type,
    uint8_t const expected_size
)
{
    if (value.type != coreType(expected_type) || value.bytes.size() != expected_size
        || !value.elements.empty()) {
        return std::nullopt;
    }
    uint64_t result = 0U;
    for (uint8_t index = 0U; index < expected_size; ++index) {
        result |= static_cast<uint64_t>(value.bytes[index]) << (index * 8U);
    }
    return result;
}

[[nodiscard]]
std::vector<GenerationPlanHostSpec> generationPlanHostSpecs()
{
    using core::lang::TypeKind;
    return {
        {"generation_plan_version", GenerationPlanHostCall::Version, {coreType(TypeKind::U8)}},
        {"generation_pregen", GenerationPlanHostCall::Pregen, {}},
        {"generation_refinement", GenerationPlanHostCall::Refinement, {coreType(TypeKind::U64)}},
        {"generation_virtual_stage", GenerationPlanHostCall::VirtualStage, {coreType(TypeKind::U8)}},
        {"generation_priority", GenerationPlanHostCall::Priority, {coreType(TypeKind::U8)}},
    };
}

[[nodiscard]]
core::lang::CustomManifest generationPlanManifest(GenerationPlanHostSpec const& spec)
{
    core::lang::CustomManifest manifest{
        .provider_key = "minecraft.worldgen." + std::string{spec.name},
        .abi_major = 1U,
        .effect = core::lang::CustomEffect::Observable,
        .arguments = spec.arguments,
        .result = coreType(core::lang::TypeKind::Unit),
        .borrow = {.parameters = std::vector<core::lang::BorrowAccess>(
            spec.arguments.size(),
            core::lang::BorrowAccess::None
        )},
    };
    manifest.digest = core::lang::customManifestDigest(manifest);
    return manifest;
}

[[nodiscard]]
core::lang::CompilerRegistry generationPlanCompilerRegistry()
{
    core::lang::Ruleset ruleset{.id = "minecraft", .version = 1U};
    std::vector<GenerationPlanHostSpec> const specs = generationPlanHostSpecs();
    ruleset.operations.reserve(specs.size());
    for (GenerationPlanHostSpec const& spec : specs) {
        core::lang::CustomManifest const manifest = generationPlanManifest(spec);
        ruleset.operations.push_back({
            .name = std::string{spec.name},
            .kind = core::lang::ExtensionKind::Builtin,
            .arguments = manifest.arguments,
            .result = manifest.result,
            .effect = manifest.effect,
            .custom = manifest,
            .borrow = manifest.borrow,
        });
    }
    return {
        .rulesets = {std::move(ruleset)},
        .defaults = {"minecraft"},
    };
}

class GenerationPlanCollector final {
public:
    static constexpr uint64_t MAX_DECLARATION_CALLS = 16U;
    static constexpr uint8_t MANIFEST_VERSION = 1U;

    [[nodiscard]] std::expected<void, std::string> call(
        GenerationPlanHostCall const call,
        std::span<core::lang::Value const> arguments
    )
    {
        if (m_call_count >= MAX_DECLARATION_CALLS) {
            return std::unexpected("world generation declaration call limit exceeded");
        }
        ++m_call_count;
        if (!m_version_seen && call != GenerationPlanHostCall::Version) {
            return std::unexpected("world generation declaration must begin with its version");
        }
        switch (call) {
        case GenerationPlanHostCall::Version:
            return version(arguments);
        case GenerationPlanHostCall::Pregen:
            return pregen(arguments);
        case GenerationPlanHostCall::Refinement:
            return refinement(arguments);
        case GenerationPlanHostCall::VirtualStage:
            return virtualStage(arguments);
        case GenerationPlanHostCall::Priority:
            return priority(arguments);
        }
        return std::unexpected("unknown world generation declaration");
    }

    [[nodiscard]] std::expected<shared::WorldGenerationPlan, std::string> build() const
    {
        if (!m_version_seen || m_version != MANIFEST_VERSION) {
            return std::unexpected("world generation declaration has a missing or unsupported version");
        }
        shared::WorldGenerationPlan plan = m_plan;
        if (plan.priority_order.empty()) {
            plan.priority_order.assign(
                shared::DEFAULT_GENERATION_PRIORITY_ORDER.begin(),
                shared::DEFAULT_GENERATION_PRIORITY_ORDER.end()
            );
        }
        if (!shared::isValidWorldGenerationPlan(plan)) {
            return std::unexpected("world generation declaration is invalid");
        }
        return plan;
    }

private:
    [[nodiscard]] std::expected<void, std::string> version(
        std::span<core::lang::Value const> arguments
    )
    {
        if (m_version_seen || arguments.size() != 1U) {
            return std::unexpected("world generation version must be declared exactly once");
        }
        std::optional<uint64_t> const value = unsignedCoreValue(
            arguments.front(),
            core::lang::TypeKind::U8,
            sizeof(uint8_t)
        );
        if (!value) {
            return std::unexpected("world generation version must be a u8");
        }
        m_version_seen = true;
        m_version = static_cast<uint8_t>(*value);
        if (m_version != MANIFEST_VERSION) {
            return std::unexpected("world generation declaration has an unsupported version");
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> pregen(
        std::span<core::lang::Value const> arguments
    )
    {
        if (!arguments.empty() || m_plan.pre_generate) {
            return std::unexpected("world pre-generation may be declared only once without arguments");
        }
        m_plan.pre_generate = true;
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> refinement(
        std::span<core::lang::Value const> arguments
    )
    {
        if (arguments.size() != 1U || m_plan.refinement_extents.size()
                >= shared::REGISTERED_WORLD_GENERATION_EXTENTS.size()) {
            return std::unexpected("world refinement declaration exceeded its bounded extent list");
        }
        std::optional<uint64_t> const value = unsignedCoreValue(
            arguments.front(),
            core::lang::TypeKind::U64,
            sizeof(uint64_t)
        );
        if (!value || *value > std::numeric_limits<uint32_t>::max()) {
            return std::unexpected("world refinement extent must be a bounded u64");
        }
        uint32_t const extent = static_cast<uint32_t>(*value);
        if (std::ranges::find(shared::REGISTERED_WORLD_GENERATION_EXTENTS, extent)
            == shared::REGISTERED_WORLD_GENERATION_EXTENTS.end()) {
            return std::unexpected("world refinement extent is not registered");
        }
        m_plan.refinement_extents.push_back(extent);
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> virtualStage(
        std::span<core::lang::Value const> arguments
    )
    {
        if (arguments.size() != 1U || m_plan.chunk_stages.size() >= 2U) {
            return std::unexpected("virtual chunk stage declaration exceeded its bounded list");
        }
        std::optional<uint64_t> const value = unsignedCoreValue(
            arguments.front(),
            core::lang::TypeKind::U8,
            sizeof(uint8_t)
        );
        if (!value || *value < 1U || *value > 2U) {
            return std::unexpected("virtual chunk stage must be HeightTile (1) or Materialize (2)");
        }
        m_plan.chunk_stages.push_back(*value == 1U
            ? shared::GenerationStage::HeightTile
            : shared::GenerationStage::Materialize);
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> priority(
        std::span<core::lang::Value const> arguments
    )
    {
        if (arguments.size() != 1U || m_plan.priority_order.size() >= 3U) {
            return std::unexpected("world priority declaration exceeded its bounded dimension list");
        }
        std::optional<uint64_t> const value = unsignedCoreValue(
            arguments.front(),
            core::lang::TypeKind::U8,
            sizeof(uint8_t)
        );
        if (!value || *value >= 3U) {
            return std::unexpected("world priority dimension must be Distance (0), View (1), or Movement (2)");
        }
        auto const dimension = static_cast<shared::GenerationPriorityDimension>(*value);
        if (std::ranges::find(m_plan.priority_order, dimension) != m_plan.priority_order.end()) {
            return std::unexpected("world priority dimensions must be unique");
        }
        m_plan.priority_order.push_back(dimension);
        return {};
    }

private:
    shared::WorldGenerationPlan m_plan{.priority_order = {}};
    uint64_t m_call_count{0U};
    uint8_t m_version{0U};
    bool m_version_seen{false};
};

[[nodiscard]]
std::expected<std::optional<uint64_t>, std::string> generationPlanExport(
    core::lang::Artifact const& artifact,
    std::string_view const module_id
)
{
    auto const module = std::ranges::find(artifact.modules, module_id, &core::lang::Module::id);
    if (module == artifact.modules.end()) {
        return std::unexpected("compiled terrain module is missing");
    }
    std::optional<uint64_t> target;
    for (core::lang::Export const& exported : module->exports) {
        if (exported.name != "configure_generation") {
            continue;
        }
        if (exported.kind != core::lang::ExportKind::Function || target.has_value()
            || exported.target >= artifact.functions.size()) {
            return std::unexpected("configure_generation must be one exported function");
        }
        core::lang::Function const& function = artifact.functions[
            static_cast<std::vector<core::lang::Function>::size_type>(exported.target)
        ];
        if (!function.exported || !function.parameters.empty()
            || function.result.kind != core::lang::TypeKind::Unit) {
            return std::unexpected("configure_generation must have signature () -> unit");
        }
        target = exported.target;
    }
    return target;
}

[[nodiscard]]
std::expected<void, std::string> registerGenerationPlanProviders(
    core::lang::Runtime& runtime,
    GenerationPlanCollector& collector
)
{
    for (GenerationPlanHostSpec const& spec : generationPlanHostSpecs()) {
        core::lang::CustomManifest const manifest = generationPlanManifest(spec);
        auto const registered = runtime.registerProvider({
            .manifest = manifest,
            .invoke = [&collector, call = spec.call](
                core::lang::CustomContext&,
                std::span<core::lang::Value const> arguments
            ) -> core::lang::CustomOutcome {
                auto const result = collector.call(call, arguments);
                if (!result) {
                    return core::lang::CustomFail{.code = 1U, .message = result.error()};
                }
                return core::lang::CustomComplete{.value = unitValue()};
            },
        });
        if (!registered) {
            return std::unexpected(registered.error().message);
        }
    }
    return {};
}

} // namespace

namespace shared {

std::optional<uint16_t> HeightTile::heightAt(BlockCoordinate const coordinate) const noexcept
{
    if (!Chunk::isValid(coordinate)) {
        return std::nullopt;
    }
    return heights[sampleIndex(coordinate)];
}

struct TerrainGenerator::ScriptState final {
    GenerationPlanCollector generation_plan_collector;
    core::lang::Runtime runtime;
    std::optional<core::lang::Program> program;
    WorldGenerationPlan generation_plan{
        .chunk_stages = {GenerationStage::Materialize},
    };
    double base{0.0};
    double spacing{0.0};
    double amplitude{0.0};

    [[nodiscard]]
    double parameter(std::string_view const name)
    {
        auto const outcome = runtime.execute(*program, "s6_height_tile", name, {}, {});
        if (!outcome || !std::holds_alternative<core::lang::Completed>(*outcome)) {
            throw std::runtime_error{"CoreLang terrain parameter failed"};
        }
        core::lang::Value const& value = std::get<core::lang::Completed>(*outcome).value;
        if (value.type.kind != core::lang::TypeKind::F64 || value.bytes.size() != sizeof(double)) {
            throw std::runtime_error{"CoreLang terrain parameter has invalid type"};
        }
        uint64_t bits = 0U;
        for (uint32_t index = 0U; index < sizeof(double); ++index) {
            bits |= static_cast<uint64_t>(value.bytes[index]) << (index * 8U);
        }
        return std::bit_cast<double>(bits);
    }

    explicit ScriptState(std::string_view const script_source)
    {
        core::lang::CompilerRegistry const registry = generationPlanCompilerRegistry();
        auto const compiled = core::lang::compile(
            {
                .id = "s6_height_tile",
                .text = script_source.empty()
                    ? detail::S6_HEIGHT_TILE_SCRIPT
                    : std::string{script_source},
            },
            registry
        );
        if (!compiled) {
            throw std::runtime_error{"CoreLang terrain script failed to compile"};
        }
        auto const configure_generation = generationPlanExport(compiled->artifact, "s6_height_tile");
        if (!configure_generation) {
            throw std::runtime_error{"CoreLang terrain generation declaration failed: " + configure_generation.error()};
        }
        if (auto const registered = registerGenerationPlanProviders(runtime, generation_plan_collector); !registered) {
            throw std::runtime_error{"CoreLang terrain generation providers failed to register: " + registered.error()};
        }
        auto loaded = runtime.load(compiled->bytes);
        if (!loaded) {
            throw std::runtime_error{"CoreLang terrain script failed to load"};
        }
        program = std::move(*loaded);
        if (configure_generation->has_value()) {
            auto const outcome = runtime.execute(*program, "s6_height_tile", "configure_generation", {}, {});
            if (!outcome || !std::holds_alternative<core::lang::Completed>(*outcome)) {
                throw std::runtime_error{"CoreLang terrain generation declaration failed to execute"};
            }
            auto configured_plan = generation_plan_collector.build();
            if (!configured_plan) {
                throw std::runtime_error{"CoreLang terrain generation declaration is invalid: "
                    + configured_plan.error()};
            }
            generation_plan = std::move(*configured_plan);
        }
        base = parameter("base_height");
        spacing = parameter("crest_spacing");
        amplitude = parameter("crest_amplitude");
        if (!std::isfinite(base) || !std::isfinite(spacing) || !std::isfinite(amplitude)
            || base < 0.0 || spacing <= 0.0 || amplitude < 0.0) {
            throw std::runtime_error{"CoreLang terrain parameters are invalid"};
        }
    }
};

TerrainGenerator::TerrainGenerator()
    : TerrainGenerator(std::string_view{})
{
}

TerrainGenerator::TerrainGenerator(std::string_view const script_source)
    : m_script(std::make_shared<ScriptState>(script_source))
{
}

TerrainGenerator::~TerrainGenerator() = default;
TerrainGenerator::TerrainGenerator(TerrainGenerator&&) noexcept = default;
TerrainGenerator& TerrainGenerator::operator=(TerrainGenerator&&) noexcept = default;

uint16_t TerrainGenerator::heightAt(int64_t const x, int64_t const y) const noexcept
{
    return kernelHeightAt(x, y, m_script->base, m_script->spacing, m_script->amplitude);
}

WorldGenerationPlan const& TerrainGenerator::generationPlan() const noexcept
{
    return m_script->generation_plan;
}

HeightTile TerrainGenerator::generateHeightTile(HeightTileCoordinate coordinate) const
{
    constexpr int32_t TILE_COUNT = static_cast<int32_t>(WorldExtent::WIDTH / HeightTile::SIDE_LENGTH);
    coordinate.x = static_cast<int32_t>((coordinate.x % TILE_COUNT + TILE_COUNT) % TILE_COUNT);
    coordinate.y = static_cast<int32_t>((coordinate.y % TILE_COUNT + TILE_COUNT) % TILE_COUNT);
    HeightTile tile{.coordinate = coordinate};
    for (uint8_t y = 0U; y < HeightTile::SIDE_LENGTH; ++y) {
        for (uint8_t x = 0U; x < HeightTile::SIDE_LENGTH; ++x) {
            int64_t const world_x = static_cast<int64_t>(coordinate.x) * HeightTile::SIDE_LENGTH + x;
            int64_t const world_y = static_cast<int64_t>(coordinate.y) * HeightTile::SIDE_LENGTH + y;
            tile.heights[static_cast<uint32_t>(y) * HeightTile::SIDE_LENGTH + x] = heightAt(world_x, world_y);
        }
    }
    return tile;
}

Chunk TerrainGenerator::generateChunk(ChunkCoordinate const coordinate) const
{
    if (!WorldBounds::isValidChunk(coordinate)) {
        throw std::invalid_argument{"chunk coordinate is outside the sparse world"};
    }
    HeightTile const tile = generateHeightTile({.x = coordinate.x, .y = coordinate.y});
    return chunkFromHeightTile(coordinate, tile);
}

Chunk TerrainGenerator::generateChunk(
    ChunkCoordinate const coordinate,
    std::span<uint8_t const> const refinement_output
) const
{
    if (!WorldBounds::isValidChunk(coordinate)) {
        throw std::invalid_argument{"chunk coordinate is outside the sparse world"};
    }
    static constexpr uint32_t SAMPLE_SIDE = 16U;
    static constexpr uint32_t HEADER_BYTES = 3U * sizeof(uint32_t);
    static constexpr uint32_t SAMPLE_BYTES = sizeof(uint16_t);
    if (refinement_output.size() != HEADER_BYTES + SAMPLE_SIDE * SAMPLE_SIDE * SAMPLE_BYTES) {
        throw std::invalid_argument{"refinement output has an invalid sample payload"};
    }
    auto read_header = [refinement_output](uint32_t const offset) {
        uint32_t value = 0U;
        for (uint32_t byte = 0U; byte < sizeof(uint32_t); ++byte) {
            value |= static_cast<uint32_t>(refinement_output[offset + byte]) << (byte * 8U);
        }
        return value;
    };
    uint32_t const origin_x = read_header(0U);
    uint32_t const origin_y = read_header(sizeof(uint32_t));
    uint32_t const extent = read_header(2U * sizeof(uint32_t));
    uint64_t const chunk_x = static_cast<uint64_t>(coordinate.x) * Chunk::SIDE_LENGTH;
    uint64_t const chunk_y = static_cast<uint64_t>(coordinate.y) * Chunk::SIDE_LENGTH;
    if (std::ranges::find(REGISTERED_WORLD_GENERATION_EXTENTS, extent)
            == REGISTERED_WORLD_GENERATION_EXTENTS.end()
        || chunk_x < origin_x || chunk_y < origin_y
        || chunk_x + Chunk::SIDE_LENGTH > static_cast<uint64_t>(origin_x) + extent
        || chunk_y + Chunk::SIDE_LENGTH > static_cast<uint64_t>(origin_y) + extent) {
        throw std::invalid_argument{"refinement output does not contain its chunk"};
    }
    HeightTile tile{.coordinate = {.x = coordinate.x, .y = coordinate.y}};
    for (uint32_t y = 0U; y < Chunk::SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < Chunk::SIDE_LENGTH; ++x) {
            uint32_t const sample_x = static_cast<uint32_t>(
                (chunk_x + x - origin_x) * SAMPLE_SIDE / extent
            );
            uint32_t const sample_y = static_cast<uint32_t>(
                (chunk_y + y - origin_y) * SAMPLE_SIDE / extent
            );
            uint32_t const offset = HEADER_BYTES + (sample_y * SAMPLE_SIDE + sample_x) * SAMPLE_BYTES;
            tile.heights[y * Chunk::SIDE_LENGTH + x] = static_cast<uint16_t>(refinement_output[offset])
                | static_cast<uint16_t>(static_cast<uint16_t>(refinement_output[offset + 1U]) << 8U);
        }
    }
    return chunkFromHeightTile(coordinate, tile);
}

Chunk TerrainGenerator::chunkFromHeightTile(
    ChunkCoordinate const coordinate,
    HeightTile const& tile
) const
{
    Chunk::Blocks blocks;
    blocks.fill(Block::Air);
    for (uint8_t z = 0U; z < Chunk::SIDE_LENGTH; ++z) {
        uint32_t const world_z = static_cast<uint32_t>(coordinate.z) * Chunk::SIDE_LENGTH + z;
        for (uint8_t y = 0U; y < Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0U; x < Chunk::SIDE_LENGTH; ++x) {
                uint16_t const height = tile.heights[static_cast<uint32_t>(y) * Chunk::SIDE_LENGTH + x];
                if (world_z < height) {
                    blocks[static_cast<uint32_t>(z) * Chunk::FACE_BLOCK_COUNT
                        + static_cast<uint32_t>(y) * Chunk::SIDE_LENGTH + x] = Block::Stone;
                }
            }
        }
    }
    return Chunk{coordinate, std::move(blocks)};
}

bool TerrainGenerator::usingCoreLang() const noexcept
{
    return m_script && m_script->program.has_value();
}

} // namespace shared
