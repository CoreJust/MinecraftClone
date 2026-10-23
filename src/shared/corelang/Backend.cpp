#include <shared/corelang/Backend.hpp>

#include <shared/corelang/Execution.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>
#include <core/lang/Bytecode.hpp>
#include <core/lang/Compiler.hpp>
#include <core/lang/Registry.hpp>
#include <core/lang/Value.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shared::corelang {

namespace {

constexpr uint32_t MAX_MANIFEST_SOURCE_BYTES = 4U * 1'024U;
constexpr uint32_t MAX_BYTECODE_BYTES = 64U * 1'024U;
constexpr uint32_t MAX_INSTRUCTIONS = 64U;
constexpr uint8_t MAX_HOST_CALLS = 10U;
constexpr std::string_view PROVIDER_PREFIX = "minecraft.terrain_v2.";
constexpr std::string_view PLAN_MODULE = "terrain_v2_plan";
constexpr std::string_view PLAN_EXPORT = "configure_terrain";
constexpr uint8_t PLAN_MANIFEST_VERSION = 2U;

constexpr std::string_view TERRAIN_PLAN_SOURCE = R"core(@version(0.1.5)
@use minecraft

pub fn configure_terrain() -> u8 {
    terrain_plan(
        2u8,
        terrain_seed(),
        terrain_resolution(),
        terrain_epochs(),
        terrain_erosion_steps(),
        terrain_initial_climate_iterations(),
        terrain_climate_iterations(),
        terrain_currents(),
        terrain_wind_sign(),
        terrain_coastal_elaboration(),
        true,
        4096u32,
        1024u32,
        256u32,
        64u32,
        3u8
    )
}
)core";

enum class TerrainField : uint8_t {
    Seed,
    Resolution,
    Epochs,
    ErosionSteps,
    InitialClimateIterations,
    ClimateIterations,
    Currents,
    WindSign,
    CoastalElaboration,
};

struct FieldSpec final {
    std::string_view name;
    core::lang::TypeKind type;
    TerrainField field;
};

constexpr std::array<FieldSpec, 9U> FIELD_SPECS{
    FieldSpec{"terrain_seed", core::lang::TypeKind::U64, TerrainField::Seed},
    FieldSpec{"terrain_resolution", core::lang::TypeKind::U32, TerrainField::Resolution},
    FieldSpec{"terrain_epochs", core::lang::TypeKind::U32, TerrainField::Epochs},
    FieldSpec{"terrain_erosion_steps", core::lang::TypeKind::U32, TerrainField::ErosionSteps},
    FieldSpec{
        "terrain_initial_climate_iterations",
        core::lang::TypeKind::U32,
        TerrainField::InitialClimateIterations,
    },
    FieldSpec{
        "terrain_climate_iterations", core::lang::TypeKind::U32, TerrainField::ClimateIterations},
    FieldSpec{"terrain_currents", core::lang::TypeKind::Bool, TerrainField::Currents},
    FieldSpec{"terrain_wind_sign", core::lang::TypeKind::I8, TerrainField::WindSign},
    FieldSpec{
        "terrain_coastal_elaboration",
        core::lang::TypeKind::Bool,
        TerrainField::CoastalElaboration,
    },
};

struct OperationSpec final {
    std::string_view name;
    core::lang::TypeKind result;
    bool plan;
};

constexpr std::array<OperationSpec, 10U> OPERATION_SPECS{
    OperationSpec{"terrain_seed", core::lang::TypeKind::U64, false},
    OperationSpec{"terrain_resolution", core::lang::TypeKind::U32, false},
    OperationSpec{"terrain_epochs", core::lang::TypeKind::U32, false},
    OperationSpec{"terrain_erosion_steps", core::lang::TypeKind::U32, false},
    OperationSpec{"terrain_initial_climate_iterations", core::lang::TypeKind::U32, false},
    OperationSpec{"terrain_climate_iterations", core::lang::TypeKind::U32, false},
    OperationSpec{"terrain_currents", core::lang::TypeKind::Bool, false},
    OperationSpec{"terrain_wind_sign", core::lang::TypeKind::I8, false},
    OperationSpec{"terrain_coastal_elaboration", core::lang::TypeKind::Bool, false},
    OperationSpec{"terrain_plan", core::lang::TypeKind::U8, true},
};

[[nodiscard]]
BackendError error(BackendErrorCode const code, std::string message)
{
    return {.code = code, .message = std::move(message)};
}

[[nodiscard]]
core::lang::Type coreType(core::lang::TypeKind const kind)
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
uint8_t byteWidth(core::lang::TypeKind const kind) noexcept
{
    switch (kind) {
    case core::lang::TypeKind::Bool:
    case core::lang::TypeKind::I8:
    case core::lang::TypeKind::U8:
        return 1U;
    case core::lang::TypeKind::U32:
        return 4U;
    case core::lang::TypeKind::U64:
        return 8U;
    default:
        return 0U;
    }
}

[[nodiscard]]
core::lang::Value unsignedValue(core::lang::TypeKind const kind, uint64_t const raw)
{
    uint8_t const width = byteWidth(kind);
    std::vector<uint8_t> bytes(width);
    for (uint8_t index = 0U; index < width; ++index) {
        bytes[index] = static_cast<uint8_t>(raw >> (index * 8U));
    }
    return {
        .type = coreType(kind),
        .bytes = std::move(bytes),
        .elements = {},
        .literal_lease = {},
        .runtime_lease = {},
    };
}

[[nodiscard]]
core::lang::Value booleanValue(bool const raw)
{
    return {
        .type = coreType(core::lang::TypeKind::Bool),
        .bytes = {static_cast<uint8_t>(raw)},
        .elements = {},
        .literal_lease = {},
        .runtime_lease = {},
    };
}

[[nodiscard]]
core::lang::Value signedByteValue(int8_t const raw)
{
    return {
        .type = coreType(core::lang::TypeKind::I8),
        .bytes = {std::bit_cast<uint8_t>(raw)},
        .elements = {},
        .literal_lease = {},
        .runtime_lease = {},
    };
}

[[nodiscard]]
std::expected<uint64_t, std::string> readUnsigned(core::lang::Value const &value,
    core::lang::TypeKind const kind)
{
    uint8_t const width = byteWidth(kind);
    if (width == 0U || value.type != coreType(kind) || value.bytes.size() != width ||
        !value.elements.empty()) {
        return std::unexpected("manifest supplied a primitive with the wrong type or width");
    }
    uint64_t result = 0U;
    for (uint8_t index = 0U; index < width; ++index) {
        result |= static_cast<uint64_t>(value.bytes[index]) << (index * 8U);
    }
    return result;
}

[[nodiscard]]
std::expected<bool, std::string> readBoolean(core::lang::Value const &value)
{
    if (value.type != coreType(core::lang::TypeKind::Bool) || value.bytes.size() != 1U ||
        !value.elements.empty() || value.bytes.front() > 1U) {
        return std::unexpected("manifest supplied a malformed boolean field");
    }
    return value.bytes.front() != 0U;
}

[[nodiscard]]
std::expected<int8_t, std::string> readSignedByte(core::lang::Value const &value)
{
    auto const raw = readUnsigned(value, core::lang::TypeKind::I8);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return std::bit_cast<int8_t>(static_cast<uint8_t>(*raw));
}

[[nodiscard]]
core::lang::CustomManifest manifest(OperationSpec const &spec)
{
    std::vector<core::lang::Type> arguments;
    if (spec.plan) {
        arguments.reserve(16U);
        arguments.push_back(coreType(core::lang::TypeKind::U8));
        for (FieldSpec const &field : FIELD_SPECS) {
            arguments.push_back(coreType(field.type));
        }
        arguments.push_back(coreType(core::lang::TypeKind::Bool));
        arguments.insert(arguments.end(), 4U, coreType(core::lang::TypeKind::U32));
        arguments.push_back(coreType(core::lang::TypeKind::U8));
    }
    core::lang::CustomManifest result{
        .provider_key = std::string{PROVIDER_PREFIX} + std::string{spec.name},
        .abi_major = 1U,
        .effect = core::lang::CustomEffect::Observable,
        .arguments = std::move(arguments),
        .result = coreType(spec.result),
        .borrow = {},
        .clobber_regions = {0U},
    };
    result.borrow.parameters.assign(result.arguments.size(), core::lang::BorrowAccess::None);
    result.digest = core::lang::customManifestDigest(result);
    return result;
}

[[nodiscard]]
core::lang::CompilerRegistry compilerRegistry()
{
    core::lang::Ruleset ruleset{
        .id = "minecraft",
        .version = 2U,
        .restrictions = {},
        .operations = {},
        .triggers = {},
        .resolve = {},
        .automaticImports = {},
    };
    ruleset.operations.reserve(OPERATION_SPECS.size());
    for (OperationSpec const &spec : OPERATION_SPECS) {
        core::lang::CustomManifest const custom = manifest(spec);
        ruleset.operations.push_back({
            .name = std::string{spec.name},
            .kind = core::lang::ExtensionKind::Builtin,
            .arguments = custom.arguments,
            .result = custom.result,
            .effect = custom.effect,
            .custom = custom,
            .expand = {},
            .compile_time_effect = {},
            .compile_time = {},
            .borrow = custom.borrow,
        });
    }
    return {
        .rulesets = {std::move(ruleset)},
        .defaults = {"minecraft"},
        .restrictions =
            {
                .allow = {},
                .deny =
                    {
                        core::lang::Feature::Loops,
                        core::lang::Feature::LoopLabels,
                        core::lang::Feature::Statics,
                        core::lang::Feature::Unsafe,
                        core::lang::Feature::FunctionReferences,
                    },
                .allowSelectors = {},
                .denySelectors = {},
            },
    };
}

[[nodiscard]]
std::optional<FieldSpec> findField(std::string_view const name) noexcept
{
    auto const field = std::ranges::find(FIELD_SPECS, name, &FieldSpec::name);
    if (field == FIELD_SPECS.end()) {
        return std::nullopt;
    }
    return *field;
}

[[nodiscard]]
std::optional<OperationSpec> findOperation(std::string_view const provider)
{
    if (!provider.starts_with(PROVIDER_PREFIX)) {
        return std::nullopt;
    }
    std::string_view const name = provider.substr(PROVIDER_PREFIX.size());
    auto const operation = std::ranges::find(OPERATION_SPECS, name, &OperationSpec::name);
    if (operation == OPERATION_SPECS.end()) {
        return std::nullopt;
    }
    return *operation;
}

[[nodiscard]]
core::lang::Value fieldValue(FieldSpec const &spec,
                             generation::GenerationConfiguration const &configuration)
{
    switch (spec.field) {
    case TerrainField::Seed:
        return unsignedValue(spec.type, configuration.seed);
    case TerrainField::Resolution:
        return unsignedValue(spec.type, configuration.resolution);
    case TerrainField::Epochs:
        return unsignedValue(spec.type, configuration.epochs);
    case TerrainField::ErosionSteps:
        return unsignedValue(spec.type, configuration.erosion_steps);
    case TerrainField::InitialClimateIterations:
        return unsignedValue(spec.type, configuration.initial_climate_iterations);
    case TerrainField::ClimateIterations:
        return unsignedValue(spec.type, configuration.climate_iterations);
    case TerrainField::Currents:
        return booleanValue(configuration.currents);
    case TerrainField::WindSign:
        return signedByteValue(configuration.wind_sign);
    case TerrainField::CoastalElaboration:
        return booleanValue(configuration.coastal_elaboration);
    }
    return {};
}

[[nodiscard]]
std::expected<generation::GenerationConfiguration, std::string>
configurationFromManifest(std::span<core::lang::Value const> arguments)
{
    if (arguments.size() != 10U) {
        return std::unexpected(
            "terrain manifest must return exactly nine configuration fields and a version");
    }
    auto const version = readUnsigned(arguments[0U], core::lang::TypeKind::U8);
    auto const seed = readUnsigned(arguments[1U], core::lang::TypeKind::U64);
    auto const resolution = readUnsigned(arguments[2U], core::lang::TypeKind::U32);
    auto const epochs = readUnsigned(arguments[3U], core::lang::TypeKind::U32);
    auto const erosion_steps = readUnsigned(arguments[4U], core::lang::TypeKind::U32);
    auto const initial_climate_iterations = readUnsigned(arguments[5U], core::lang::TypeKind::U32);
    auto const climate_iterations = readUnsigned(arguments[6U], core::lang::TypeKind::U32);
    auto const currents = readBoolean(arguments[7U]);
    auto const wind_sign = readSignedByte(arguments[8U]);
    auto const coastal_elaboration = readBoolean(arguments[9U]);
    if (!version || !seed || !resolution || !epochs || !erosion_steps ||
        !initial_climate_iterations || !climate_iterations || !currents || !wind_sign ||
        !coastal_elaboration) {
        return std::unexpected(
            "terrain manifest configuration contains an invalid primitive field");
    }
    if (*version != PLAN_MANIFEST_VERSION) {
        return std::unexpected("terrain manifest version is unsupported");
    }
    return generation::GenerationConfiguration{
        .seed = *seed,
        .resolution = static_cast<uint32_t>(*resolution),
        .epochs = static_cast<uint32_t>(*epochs),
        .erosion_steps = static_cast<uint32_t>(*erosion_steps),
        .initial_climate_iterations = static_cast<uint32_t>(*initial_climate_iterations),
        .climate_iterations = static_cast<uint32_t>(*climate_iterations),
        .currents = *currents,
        .wind_sign = *wind_sign,
        .coastal_elaboration = *coastal_elaboration,
    };
}

class HostBridge final {
  public:
    HostBridge(generation::GenerationConfiguration const &configuration,
               std::stop_token const& cancellation)
        : m_configuration{configuration}, m_cancellation{cancellation}
    {
    }

    [[nodiscard]]
    std::expected<core::lang::Value, BackendError>
    invoke(std::string_view const provider, std::span<core::lang::Value const> arguments)
    {
        if (m_failure) {
            return std::unexpected(*m_failure);
        }
        auto result = invokeImpl(provider, arguments);
        if (!result) {
            m_failure = result.error();
        }
        return result;
    }

  private:
    [[nodiscard]]
    std::expected<core::lang::Value, BackendError>
    invokeImpl(std::string_view const provider, std::span<core::lang::Value const> arguments)
    {
        if (m_cancellation.stop_requested()) {
            return std::unexpected(
                error(BackendErrorCode::Cancelled, "terrain plan preparation was cancelled"));
        }
        if (m_call_count >= MAX_HOST_CALLS) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "terrain manifest exceeded its host-call limit"));
        }
        ++m_call_count;

        auto const operation = findOperation(provider);
        if (!operation) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "terrain manifest requested an unknown host operation"));
        }
        if (!operation->plan) {
            auto const field = findField(operation->name);
            if (!field || !arguments.empty()) {
                return std::unexpected(error(BackendErrorCode::InvalidManifest,
                    "terrain getter has an invalid signature"));
            }
            uint8_t const index = static_cast<uint8_t>(field->field);
            if (m_read_fields[index]) {
                return std::unexpected(
                    error(BackendErrorCode::InvalidManifest,
                          "terrain manifest read a configuration field more than once"));
            }
            m_read_fields[index] = true;
            return fieldValue(*field, m_configuration);
        }

        if (m_plan_received || !allFieldsRead()) {
            return std::unexpected(
                error(BackendErrorCode::InvalidManifest,
                      "terrain plan omitted or repeated configuration reads"));
        }
        if (arguments.size() != 16U) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "terrain plan has an invalid bounded argument list"));
        }
        auto const configured = configurationFromManifest(arguments.first(10U));
        if (!configured) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest, configured.error()));
        }
        try {
            static_cast<void>(generation::configurationIdentity(*configured));
        } catch (std::exception const &exception) {
            return std::unexpected(
                error(BackendErrorCode::InvalidManifest,
                      std::string{"terrain manifest returned invalid generation configuration: "} +
                          exception.what()));
        }
        if (*configured != m_configuration) {
            return std::unexpected(
                error(BackendErrorCode::InvalidManifest,
                      "terrain plan configuration differs from the frozen world request"));
        }

        auto const pre_generate = readBoolean(arguments[10U]);
        if (!pre_generate) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "terrain plan has an invalid pre-generation flag"));
        }
        WorldGenerationPlan plan{
            .pre_generate = *pre_generate,
            .refinement_extents = {},
            .chunk_stages = {},
        };
        bool saw_empty_extent = false;
        for (uint64_t index = 11U; index < 15U; ++index) {
            auto const argument_index =
                static_cast<std::span<core::lang::Value const>::size_type>(index);
            auto const extent = readUnsigned(arguments[argument_index], core::lang::TypeKind::U32);
            if (!extent) {
                return std::unexpected(error(BackendErrorCode::InvalidManifest,
                    "terrain plan contains an invalid refinement extent"));
            }
            if (*extent == 0U) {
                saw_empty_extent = true;
                continue;
            }
            if (saw_empty_extent) {
                return std::unexpected(error(BackendErrorCode::InvalidManifest,
                    "terrain plan refinement extents are not contiguous"));
            }
            plan.refinement_extents.push_back(static_cast<uint32_t>(*extent));
        }
        auto const stage = readUnsigned(arguments[15U], core::lang::TypeKind::U8);
        if (!stage) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "terrain plan contains an invalid chunk stage"));
        }
        if (*stage == static_cast<uint8_t>(GenerationStage::HeightTile)) {
            plan.chunk_stages.push_back(GenerationStage::HeightTile);
        } else if (*stage == static_cast<uint8_t>(GenerationStage::Materialize)) {
            plan.chunk_stages.push_back(GenerationStage::Materialize);
        } else if (*stage != std::numeric_limits<uint8_t>::max()) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "terrain plan contains an unsupported chunk stage"));
        }
        if (!isValidWorldGenerationPlan(plan)) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "terrain manifest returned an invalid bounded world plan"));
        }

        m_plan_received = true;
        m_returned_configuration = *configured;
        m_returned_plan = std::move(plan);
        m_plan_ack = static_cast<uint8_t>(*stage);
        return unsignedValue(core::lang::TypeKind::U8, m_plan_ack);
    }

  public:
    [[nodiscard]]
    std::expected<std::pair<generation::GenerationConfiguration, WorldGenerationPlan>, BackendError>
    finish() const
    {
        if (m_failure) {
            return std::unexpected(*m_failure);
        }
        if (m_cancellation.stop_requested()) {
            return std::unexpected(
                error(BackendErrorCode::Cancelled, "terrain plan preparation was cancelled"));
        }
        if (m_call_count != MAX_HOST_CALLS || !allFieldsRead() || !m_plan_received ||
            !m_returned_configuration || !m_returned_plan) {
            return std::unexpected(
                error(BackendErrorCode::InvalidManifest,
                      "terrain manifest did not return a complete bounded configuration and plan"));
        }
        return std::pair{*m_returned_configuration, *m_returned_plan};
    }

    [[nodiscard]]
    std::optional<BackendError> const &failure() const noexcept
    {
        return m_failure;
    }

  private:
    [[nodiscard]]
    bool allFieldsRead() const noexcept
    {
        return std::ranges::all_of(m_read_fields, [](bool const read) { return read; });
    }

    generation::GenerationConfiguration const &m_configuration;
    std::stop_token m_cancellation;
    std::array<bool, FIELD_SPECS.size()> m_read_fields{};
    std::optional<generation::GenerationConfiguration> m_returned_configuration;
    std::optional<WorldGenerationPlan> m_returned_plan;
    std::optional<BackendError> m_failure;
    uint8_t m_call_count = 0U;
    uint8_t m_plan_ack = 0U;
    bool m_plan_received = false;
};

[[nodiscard]]
std::expected<void, BackendError> validateArtifact(core::lang::CompileResult const &compiled)
{
    if (compiled.bytes.empty() || compiled.bytes.size() > MAX_BYTECODE_BYTES) {
        return std::unexpected(error(BackendErrorCode::InvalidManifest,
            "terrain CoreBC artifact exceeds its byte bound"));
    }
    core::lang::Artifact const &artifact = compiled.artifact;
    if (artifact.modules.size() != 1U || artifact.functions.size() != 2U ||
        !artifact.static_cells.empty() || artifact.customs.size() != OPERATION_SPECS.size()) {
        return std::unexpected(error(
            BackendErrorCode::InvalidManifest,
            "terrain manifest has unsupported artifact counts (expected one module, an empty "
            "initializer, "
            "one entry and ten host imports; modules=" +
                std::to_string(static_cast<uint64_t>(artifact.modules.size())) +
                ", functions=" + std::to_string(static_cast<uint64_t>(artifact.functions.size())) +
                ", statics=" + std::to_string(static_cast<uint64_t>(artifact.static_cells.size())) +
                ", imports=" + std::to_string(static_cast<uint64_t>(artifact.customs.size())) +
                ")"));
    }
    core::lang::Module const &module = artifact.modules.front();
    if (module.id != PLAN_MODULE || !module.globals.empty() || module.init_function != 0U ||
        module.exports.size() != 1U) {
        return std::unexpected(error(BackendErrorCode::InvalidManifest,
            "terrain manifest contains globals or extra exports"));
    }
    core::lang::Export const &exported = module.exports.front();
    core::lang::Function const &initializer = artifact.functions.front();
    core::lang::Function const &function = artifact.functions[1U];
    if (!initializer.init || initializer.exported || !initializer.name.empty() ||
        initializer.module != 0U || !initializer.parameters.empty() ||
        initializer.result != coreType(core::lang::TypeKind::Unit) ||
        initializer.blocks.size() != 1U || initializer.blocks.front().instructions.size() != 1U ||
        initializer.blocks.front().instructions.front().opcode != core::lang::Op::ReturnUnit) {
        return std::unexpected(error(BackendErrorCode::InvalidManifest,
            "terrain module initializer is not an empty return"));
    }
    if (exported.name != PLAN_EXPORT || exported.kind != core::lang::ExportKind::Function ||
        exported.target != 1U || !function.exported || function.name != PLAN_EXPORT ||
        !function.parameters.empty() || function.init || function.module != 0U ||
        function.result != coreType(core::lang::TypeKind::U8) || function.blocks.size() != 1U ||
        !function.blocks.front().parameters.empty()) {
        return std::unexpected(
            error(BackendErrorCode::InvalidManifest,
                  "terrain manifest export has an unsupported signature or control flow"));
    }
    for (OperationSpec const &spec : OPERATION_SPECS) {
        core::lang::CustomManifest const expected = manifest(spec);
        auto const actual = std::ranges::find(
            artifact.customs, expected.provider_key, &core::lang::CustomManifest::provider_key);
        if (actual == artifact.customs.end()) {
            return std::unexpected(
                error(BackendErrorCode::InvalidManifest,
                      "terrain manifest omitted host contract " + expected.provider_key));
        }
        bool const same_contract =
            actual->provider_key == expected.provider_key &&
            actual->abi_major == expected.abi_major && actual->abi_minor == expected.abi_minor &&
            actual->effect == expected.effect && actual->may_suspend == expected.may_suspend &&
            actual->may_reenter == expected.may_reenter && actual->unsafe_ == expected.unsafe_ &&
            actual->arguments == expected.arguments && actual->result == expected.result &&
            actual->borrow == expected.borrow &&
            actual->clobber_regions == expected.clobber_regions &&
            actual->digest == core::lang::customManifestDigest(*actual);
        if (!same_contract) {
            return std::unexpected(error(
                BackendErrorCode::InvalidManifest,
                "terrain manifest changed host contract " + expected.provider_key +
                    " (abi=" + std::to_string(actual->abi_major == expected.abi_major) +
                    ", abi_minor=" + std::to_string(actual->abi_minor == expected.abi_minor) +
                    ", effect=" + std::to_string(actual->effect == expected.effect) +
                    ", suspend=" + std::to_string(actual->may_suspend == expected.may_suspend) +
                    ", reenter=" + std::to_string(actual->may_reenter == expected.may_reenter) +
                    ", unsafe=" + std::to_string(actual->unsafe_ == expected.unsafe_) +
                    ", arguments=" + std::to_string(actual->arguments == expected.arguments) +
                    ", result=" + std::to_string(actual->result == expected.result) +
                    ", borrow=" + std::to_string(actual->borrow == expected.borrow) + ", clobber=" +
                    std::to_string(actual->clobber_regions == expected.clobber_regions) +
                    ", digest_valid=" +
                    std::to_string(actual->digest == core::lang::customManifestDigest(*actual)) +
                    ")"));
        }
    }
    auto const &instructions = function.blocks.front().instructions;
    if (instructions.empty() || instructions.size() > MAX_INSTRUCTIONS ||
        instructions.back().opcode != core::lang::Op::ReturnValue ||
        std::ranges::count(instructions,
                           core::lang::Op::Custom,
                           &core::lang::Instruction::opcode) != MAX_HOST_CALLS) {
        return std::unexpected(
            error(BackendErrorCode::InvalidManifest,
                  "terrain manifest exceeded its fixed instruction or host-call budget"));
    }
    for (uint32_t index = 0U; index < instructions.size(); ++index) {
        core::lang::Op const opcode = instructions[index].opcode;
        if (opcode != core::lang::Op::Const && opcode != core::lang::Op::Copy &&
            opcode != core::lang::Op::Custom && opcode != core::lang::Op::ReturnValue &&
            opcode != core::lang::Op::Binary) {
            return std::unexpected(error(
                BackendErrorCode::InvalidManifest,
                "terrain manifest contains branching, looping, indirect, or stateful bytecode"));
        }
        if (opcode == core::lang::Op::Binary &&
            (instructions[index].operands.size() != 3U ||
             instructions[index].operands.front() !=
                 static_cast<uint64_t>(core::lang::BinaryOp::Add))) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "terrain manifest may only use bounded checked addition"));
        }
        if (opcode == core::lang::Op::ReturnValue && index + 1U != instructions.size()) {
            return std::unexpected(
                error(BackendErrorCode::InvalidManifest,
                      "terrain manifest contains instructions after its return"));
        }
    }
    return {};
}

[[nodiscard]]
std::expected<void, BackendError>
validateConfiguration(generation::GenerationConfiguration const &configuration)
{
    try {
        static_cast<void>(generation::configurationIdentity(configuration));
    } catch (std::exception const &exception) {
        return std::unexpected(
            error(BackendErrorCode::InvalidConfiguration,
                  std::string{"terrain plan requires a valid frozen configuration: "} +
                      exception.what()));
    }
    return {};
}

} // namespace

std::string_view terrainPlanSource() noexcept
{
    return TERRAIN_PLAN_SOURCE;
}

namespace {

std::expected<TerrainPlan, BackendError>
prepareTerrainPlanImpl(generation::GenerationConfiguration configuration,
                       Backend const backend,
                       std::stop_token const& cancellation,
                       std::string_view const manifest_source)
{
    try {
        auto const valid_configuration = validateConfiguration(configuration);
        if (!valid_configuration) {
            return std::unexpected(valid_configuration.error());
        }
        if (cancellation.stop_requested()) {
            return std::unexpected(
                error(BackendErrorCode::Cancelled, "terrain plan preparation was cancelled"));
        }
        ExecutionLimits limits{
            .max_source_bytes = MAX_MANIFEST_SOURCE_BYTES,
            .max_bytecode_bytes = MAX_BYTECODE_BYTES,
            .max_native_image_bytes = uint64_t{4U} * 1'024U * 1'024U,
            .max_instructions = MAX_INSTRUCTIONS,
            .max_execution_count = 1U,
        };
        auto compiled = compileBounded(
            {.id = std::string{PLAN_MODULE}, .text = std::string{manifest_source}},
            compilerRegistry(),
            limits,
            cancellation);
        if (!compiled) {
            return std::unexpected(compiled.error());
        }
        auto const valid_artifact = validateArtifact(compiled->compiled);
        if (!valid_artifact) {
            return std::unexpected(valid_artifact.error());
        }
        // Cancellation can be requested asynchronously during bounded compilation.
        // cppcheck-suppress identicalConditionAfterEarlyExit
        if (cancellation.stop_requested()) {
            return std::unexpected(
                error(BackendErrorCode::Cancelled, "terrain plan preparation was cancelled"));
        }

        HostBridge bridge{configuration, cancellation};
        std::vector<ExecutionHostCall> host_calls;
        host_calls.reserve(OPERATION_SPECS.size());
        for (OperationSpec const &spec : OPERATION_SPECS) {
            core::lang::CustomManifest const custom = manifest(spec);
            host_calls.push_back({
                .manifest = custom,
                .invoke = [&bridge, provider = custom.provider_key](
                              std::span<core::lang::Value const> arguments)
                    -> std::expected<core::lang::Value, std::string> {
                    auto const result = bridge.invoke(provider, arguments);
                    if (!result) {
                        return std::unexpected(result.error().message);
                    }
                    return *result;
                },
            });
        }
        auto executed = executeBounded({
            .program = *compiled,
            .module_id = PLAN_MODULE,
            .entry = PLAN_EXPORT,
            .backend = backend,
            .expected_result_type = coreType(core::lang::TypeKind::U8),
            .limits = limits,
            .execution_count = 1U,
            .host_calls = std::move(host_calls),
            .stop_token = cancellation,
            .cancellation_requested = {},
        });
        if (!executed) {
            if (bridge.failure()) {
                return std::unexpected(*bridge.failure());
            }
            return std::unexpected(executed.error());
        }
        auto result_marker = readUnsigned(executed->value, core::lang::TypeKind::U8);
        auto returned = bridge.finish();
        if (!returned) {
            return std::unexpected(returned.error());
        }
        if (!result_marker || *result_marker > std::numeric_limits<uint8_t>::max()) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "terrain plan entry returned a malformed acknowledgement"));
        }
        uint8_t const expected_marker = returned->second.chunk_stages.empty()
            ? std::numeric_limits<uint8_t>::max()
            : static_cast<uint8_t>(returned->second.chunk_stages.front());
        if (*result_marker != expected_marker) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "terrain plan acknowledgement differs from its captured plan"));
        }
        return TerrainPlan{
            .configuration = returned->first,
            .world_generation_plan = std::move(returned->second),
            .receipt = std::move(executed->receipt),
        };
    } catch (std::exception const &exception) {
        BackendErrorCode const code = cancellation.stop_requested()
            ? BackendErrorCode::Cancelled
            : BackendErrorCode::ExecutionFailed;
        return std::unexpected(
            error(code, std::string{"terrain plan preparation failed: "} + exception.what()));
    } catch (...) {
        BackendErrorCode const code = cancellation.stop_requested()
            ? BackendErrorCode::Cancelled
            : BackendErrorCode::ExecutionFailed;
        return std::unexpected(
            error(code, "terrain plan preparation failed with an unknown error"));
    }
}

} // namespace

std::expected<TerrainPlan, BackendError>
prepareTerrainPlan(generation::GenerationConfiguration configuration,
                   Backend const backend,
                   std::stop_token const& cancellation)
{
    return prepareTerrainPlanImpl(
        configuration, backend, cancellation, TERRAIN_PLAN_SOURCE);
}

std::expected<TerrainPlan, BackendError>
prepareTerrainPlan(generation::GenerationConfiguration configuration,
                   Backend const backend,
                   std::string_view const manifest_source,
                   std::stop_token const& cancellation)
{
    return prepareTerrainPlanImpl(configuration, backend, cancellation, manifest_source);
}

} // namespace shared::corelang
