#include <shared/corelang/Execution.hpp>

#include <shared/corelang/ExecutionTypes.hpp>
#include <core/lang/Compiler.hpp>
#include <core/lang/CoreAbi.hpp>
#include <core/lang/CoreAOT.hpp>
#include <core/lang/CoreAotToolchain.hpp>
#include <core/lang/CoreNativeImage.hpp>
#include <core/lang/CoreRT.hpp>
#include <core/lang/Diagnostic.hpp>
#include <core/lang/Optimize.hpp>
#include <core/lang/Runtime.hpp>
#include <core/lang/Syntax.hpp>
#include <core/lang/Value.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace shared::corelang {

namespace {

namespace syntax = core::lang::syntax;
namespace lang = core::lang;

constexpr uint64_t MAX_SOURCE_BYTES = uint64_t{64U} * 1'024U;
constexpr uint64_t MAX_BYTECODE_BYTES = uint64_t{1U} * 1'024U * 1'024U;
constexpr uint64_t MAX_NATIVE_IMAGE_BYTES = uint64_t{4U} * 1'024U * 1'024U;
constexpr uint64_t MAX_INSTRUCTIONS = 65'536U;
constexpr uint64_t MAX_EXECUTION_COUNT = 2U;
constexpr size_t MAX_HOST_CALLS = 64U;
constexpr size_t MAX_SYMBOL_BYTES = 256U;

struct AotStringArena final {
    std::vector<uint8_t> bytes;
    uint64_t used{0U};
};

[[nodiscard]]
BackendError error(
    BackendErrorCode const code,
    std::string message,
    std::optional<uint64_t> const source_offset = std::nullopt
)
{
    return {
        .code = code,
        .message = std::move(message),
        .source_offset = source_offset,
    };
}

[[nodiscard]]
bool isCancelled(std::stop_token const& stop_token,
                 CancellationCheck const& cancellation_requested) noexcept
{
    if (stop_token.stop_requested()) {
        return true;
    }
    if (!cancellation_requested) {
        return false;
    }
    try {
        return cancellation_requested();
    } catch (...) {
        return true;
    }
}

[[nodiscard]]
std::expected<void, BackendError> validateLimits(ExecutionLimits const& limits)
{
    if (limits.max_source_bytes == 0U || limits.max_source_bytes > MAX_SOURCE_BYTES ||
        limits.max_bytecode_bytes == 0U || limits.max_bytecode_bytes > MAX_BYTECODE_BYTES ||
        limits.max_native_image_bytes == 0U || limits.max_native_image_bytes > MAX_NATIVE_IMAGE_BYTES ||
        limits.max_instructions == 0U || limits.max_instructions > MAX_INSTRUCTIONS ||
        limits.max_execution_count == 0U || limits.max_execution_count > MAX_EXECUTION_COUNT) {
        return std::unexpected(error(BackendErrorCode::InvalidConfiguration,
            "CoreLang execution limits exceed the public fixed bounds"));
    }
    return {};
}

[[nodiscard]]
std::vector<uint8_t> bytesOf(std::string_view const text)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size());
    for (char const value : text) {
        bytes.push_back(static_cast<uint8_t>(static_cast<unsigned char>(value)));
    }
    return bytes;
}

[[nodiscard]]
std::string diagnosticsMessage(core::lang::Diagnostics const& diagnostics)
{
    if (diagnostics.empty()) {
        return "CoreLang compilation failed without diagnostics";
    }
    return diagnostics.front().message;
}

struct FunctionTarget final {
    std::string_view module_id;
    std::string_view entry;
};

struct InstructionBudget final {
    uint64_t execution_count;
    uint64_t loop_multiplier;
    bool allow_verified_cycles;
};

[[nodiscard]]
std::optional<core::lang::Type> exportedResultType(
    core::lang::Artifact const& artifact,
    FunctionTarget const& target)
{
    auto const module = std::ranges::find(artifact.modules, target.module_id, &core::lang::Module::id);
    if (module == artifact.modules.end()) {
        return std::nullopt;
    }
    auto const exported = std::ranges::find_if(module->exports, [target](core::lang::Export const& candidate) {
        return candidate.kind == core::lang::ExportKind::Function && candidate.name == target.entry;
    });
    if (exported == module->exports.end() || exported->target >= artifact.functions.size()) {
        return std::nullopt;
    }
    return artifact.functions[static_cast<std::vector<core::lang::Function>::size_type>(exported->target)].result;
}

[[nodiscard]]
bool isStringType(core::lang::Type const& type) noexcept
{
    return type.kind == core::lang::TypeKind::Str || type.kind == core::lang::TypeKind::Str8 ||
           type.kind == core::lang::TypeKind::Str16 || type.kind == core::lang::TypeKind::Str32;
}

[[nodiscard]]
uint64_t stringUnitSize(core::lang::TypeKind kind) noexcept;

[[nodiscard]]
std::expected<core::lang::CoreAbiType, BackendError> coreAbiType(core::lang::Type const& type)
{
    using core::lang::CoreAbiType;
    using core::lang::TypeKind;
    switch (type.kind) {
    case TypeKind::Unit:
        return CoreAbiType::unit();
    case TypeKind::Bool:
        return CoreAbiType::boolean();
    case TypeKind::I8:
        return CoreAbiType::signedInteger(8U);
    case TypeKind::I16:
        return CoreAbiType::signedInteger(16U);
    case TypeKind::I32:
        return CoreAbiType::signedInteger(32U);
    case TypeKind::I64:
        return CoreAbiType::signedInteger(64U);
    case TypeKind::I128:
        return CoreAbiType::signedInteger(128U);
    case TypeKind::U8:
    case TypeKind::C8:
        return CoreAbiType::unsignedInteger(8U);
    case TypeKind::U16:
    case TypeKind::C16:
        return CoreAbiType::unsignedInteger(16U);
    case TypeKind::U32:
    case TypeKind::C32:
        return CoreAbiType::unsignedInteger(32U);
    case TypeKind::U64:
        return CoreAbiType::unsignedInteger(64U);
    case TypeKind::U128:
        return CoreAbiType::unsignedInteger(128U);
    case TypeKind::F32:
        return CoreAbiType::floatingPoint(32U);
    case TypeKind::F64:
        return CoreAbiType::floatingPoint(64U);
    case TypeKind::Str:
    case TypeKind::Str8:
    case TypeKind::Str16:
    case TypeKind::Str32:
        return CoreAbiType::slice(
            CoreAbiType::unsignedInteger(static_cast<uint16_t>(stringUnitSize(type.kind) * 8U)),
            core::lang::CoreAbiBorrow::Shared);
    default:
        return std::unexpected(error(BackendErrorCode::BackendUnavailable,
            "AOT host calls do not support this CoreLang value type"));
    }
}

[[nodiscard]]
uint64_t stringUnitSize(core::lang::TypeKind const kind) noexcept
{
    switch (kind) {
    case core::lang::TypeKind::Str16:
        return 2U;
    case core::lang::TypeKind::Str32:
        return 4U;
    default:
        return 1U;
    }
}

[[nodiscard]]
std::expected<void, BackendError> validateValue(core::lang::Value const& value,
    core::lang::Type const& expected_type,
    ExecutionLimits const& limits)
{
    if (value.type != expected_type || !value.elements.empty()) {
        return std::unexpected(error(BackendErrorCode::InvalidResultType,
            "CoreLang entry or host call returned the wrong value type"));
    }
    if (value.type.kind == core::lang::TypeKind::Unit) {
        if (!value.bytes.empty()) {
            return std::unexpected(error(BackendErrorCode::InvalidResultType,
                "unit CoreLang value contains bytes"));
        }
        return {};
    }
    if (value.type.kind == core::lang::TypeKind::Bool) {
        if (value.bytes.size() != 1U || value.bytes.front() > 1U) {
            return std::unexpected(error(BackendErrorCode::InvalidResultType,
                "CoreLang boolean value is malformed"));
        }
        return {};
    }
    uint64_t const expected_size = core::lang::layoutSize(value.type);
    if (value.type.kind == core::lang::TypeKind::Str ||
        (value.type.kind >= core::lang::TypeKind::Str8 && value.type.kind <= core::lang::TypeKind::Str32)) {
        if (value.bytes.size() > limits.max_source_bytes ||
            value.bytes.size() % stringUnitSize(value.type.kind) != 0U) {
            return std::unexpected(error(BackendErrorCode::InvalidResultType,
                "CoreLang text value is malformed or exceeds its bound"));
        }
        return {};
    }
    if (expected_size == 0U || value.bytes.size() != expected_size) {
        return std::unexpected(error(BackendErrorCode::InvalidResultType,
            "CoreLang scalar value has the wrong byte width"));
    }
    return {};
}

[[nodiscard]]
std::expected<core::lang::CoreAbiValue, BackendError> abiValue(core::lang::Value const& value,
    ExecutionLimits const& limits,
    std::shared_ptr<AotStringArena> const& string_arena)
{
    auto type = coreAbiType(value.type);
    if (!type) {
        return std::unexpected(type.error());
    }
    auto const valid = validateValue(value, value.type, limits);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    core::lang::CoreAbiValue result{
        .type = std::move(*type),
        .bytes = {},
    };
    if (isStringType(value.type)) {
        if (!string_arena) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "AOT string callback has no bounded backing storage"));
        }
        uint64_t const unit_size = stringUnitSize(value.type.kind);
        uint64_t const aligned_offset = (string_arena->used + unit_size - 1U) & ~(unit_size - 1U);
        if (aligned_offset > string_arena->bytes.size() ||
            value.bytes.size() > string_arena->bytes.size() - aligned_offset) {
            return std::unexpected(error(BackendErrorCode::InvalidResultType,
                "AOT host string results exceed their shared byte bound"));
        }
        auto const data_offset = static_cast<std::vector<uint8_t>::size_type>(aligned_offset);
        std::copy(value.bytes.begin(), value.bytes.end(), string_arena->bytes.data() + data_offset);
        string_arena->used = aligned_offset + value.bytes.size();
        std::array<uint64_t, 3U> descriptor{
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(string_arena->bytes.data() + data_offset)),
            value.bytes.size() / unit_size,
            value.bytes.size(),
        };
        result.bytes.resize(sizeof(descriptor));
        std::memcpy(result.bytes.data(), descriptor.data(), sizeof(descriptor));
    } else {
        result.bytes = value.bytes;
    }
    auto const abi_valid = core::lang::validate(result);
    if (!abi_valid) {
        return std::unexpected(error(BackendErrorCode::InvalidManifest,
            "CoreLang value is invalid for the AOT host ABI: " +
            abi_valid.error().message));
    }
    return result;
}

[[nodiscard]]
std::expected<core::lang::Value, BackendError> langValue(core::lang::CoreAbiValue const& value,
    core::lang::Type const& expected_type,
    ExecutionLimits const& limits)
{
    auto type = coreAbiType(expected_type);
    if (!type) {
        return std::unexpected(type.error());
    }
    if (value.type != *type) {
        return std::unexpected(error(BackendErrorCode::InvalidManifest,
            "AOT host call received an unexpected ABI value type"));
    }
    core::lang::Value result{
        .type = expected_type,
        .bytes = {},
        .elements = {},
        .literal_lease = {},
        .runtime_lease = {},
    };
    if (isStringType(expected_type)) {
        std::array<uint64_t, 3U> descriptor{};
        if (value.bytes.size() != sizeof(descriptor)) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "AOT string value has an invalid slice descriptor"));
        }
        std::memcpy(descriptor.data(), value.bytes.data(), sizeof(descriptor));
        uint64_t const unit_size = stringUnitSize(expected_type.kind);
        if (descriptor[1U] > (std::numeric_limits<uint64_t>::max)() / unit_size) {
            return std::unexpected(error(BackendErrorCode::InvalidResultType,
                "AOT string slice length overflows its byte width"));
        }
        uint64_t const byte_count = descriptor[1U] * unit_size;
        uintptr_t const data = static_cast<uintptr_t>(descriptor[0U]);
        if (byte_count > limits.max_source_bytes || descriptor[2U] > limits.max_source_bytes ||
            byte_count > descriptor[2U] ||
            (byte_count > 0U && (data == 0U || data % unit_size != 0U ||
                byte_count > (std::numeric_limits<uintptr_t>::max)() - data))) {
            return std::unexpected(error(BackendErrorCode::InvalidResultType,
                "AOT string slice exceeds its checked bounds"));
        }
        result.bytes.resize(static_cast<std::vector<uint8_t>::size_type>(byte_count));
        if (byte_count > 0U) {
            // AOT bridge string descriptors store native addresses in uintptr_t form.
            // NOLINTNEXTLINE(performance-no-int-to-ptr): the checked descriptor must be converted for memcpy.
            std::memcpy(result.bytes.data(), reinterpret_cast<void const*>(data),
                        static_cast<size_t>(byte_count));
        }
    } else {
        result.bytes = value.bytes;
    }
    auto const valid = validateValue(result, expected_type, limits);
    if (!valid) {
        return std::unexpected(error(BackendErrorCode::InvalidManifest, valid.error().message));
    }
    return result;
}

void addStringBorrowContract(core::lang::CoreAbiSignature& signature,
                             std::span<core::lang::Type const> parameters,
                             core::lang::Type const& result);

[[nodiscard]]
std::expected<core::lang::CoreAbiCallbackContract, BackendError> callbackContract(
    core::lang::CustomManifest const& manifest)
{
    if (manifest.provider_key.empty() || manifest.abi_major == 0U || manifest.may_suspend ||
        manifest.may_reenter) {
        return std::unexpected(error(BackendErrorCode::InvalidManifest,
            "AOT host manifest must name a synchronous non-reentrant call"));
    }
    core::lang::CoreAbiCallbackContract contract{
        .id = manifest.provider_key,
        .signature = {
            .parameters = {},
            .result = {},
            .effects = 0U,
            .may_fail = true,
            .reentrant = false,
            .parameter_borrows = {},
            .return_borrows = {},
        },
    };
    contract.signature.parameters.reserve(manifest.arguments.size());
    contract.signature.parameter_borrows.reserve(manifest.arguments.size());
    for (uint64_t index = 0U; index < manifest.arguments.size(); ++index) {
        core::lang::Type const& argument =
            manifest.arguments[static_cast<std::vector<core::lang::Type>::size_type>(index)];
        auto type = coreAbiType(argument);
        if (!type) {
            return std::unexpected(type.error());
        }
        contract.signature.parameters.push_back(std::move(*type));
        core::lang::BorrowAccess const borrow =
            index < manifest.borrow.parameters.size()
                ? manifest.borrow.parameters[static_cast<std::vector<core::lang::BorrowAccess>::size_type>(index)]
                : core::lang::BorrowAccess::None;
        contract.signature.parameter_borrows.push_back(
            borrow == core::lang::BorrowAccess::Mutable ? core::lang::CoreAbiBorrow::Mutable
            : borrow == core::lang::BorrowAccess::Shared ? core::lang::CoreAbiBorrow::Shared
                : core::lang::CoreAbiBorrow::None);
    }
    auto result_type = coreAbiType(manifest.result);
    if (!result_type) {
        return std::unexpected(result_type.error());
    }
    contract.signature.result = std::move(*result_type);
    switch (manifest.effect) {
    case core::lang::CustomEffect::Pure:
        break;
    case core::lang::CustomEffect::Observable:
        contract.signature.effects = static_cast<uint32_t>(core::lang::CoreAbiEffect::HostIO);
        break;
    case core::lang::CustomEffect::Opaque:
        contract.signature.effects =
            static_cast<uint32_t>(core::lang::CoreAbiEffect::ReadMemory) |
            static_cast<uint32_t>(core::lang::CoreAbiEffect::WriteMemory) |
            static_cast<uint32_t>(core::lang::CoreAbiEffect::HostIO);
        break;
    }
    contract.signature.reentrant = manifest.may_reenter;
    if (manifest.may_reenter) {
        contract.signature.effects |= static_cast<uint32_t>(core::lang::CoreAbiEffect::Reenter);
    }
    contract.signature.return_borrows.reserve(manifest.borrow.returns.size());
    for (core::lang::ReturnBorrow const& returned : manifest.borrow.returns) {
        core::lang::CoreAbiReturnBorrow result{
            .result_path = returned.result_path,
            .origins = {},
        };
        for (core::lang::BorrowOrigin const& origin : returned.origins) {
            if (origin.kind == core::lang::BorrowOriginKind::Unknown) {
                return std::unexpected(error(BackendErrorCode::InvalidManifest,
                    "AOT host manifest has an unknown return borrow"));
            }
            result.origins.push_back({
                .kind = origin.kind == core::lang::BorrowOriginKind::Parameter
                            ? core::lang::CoreAbiBorrowOriginKind::Parameter
                            : core::lang::CoreAbiBorrowOriginKind::Persistent,
                .index = origin.index,
            });
        }
        contract.signature.return_borrows.push_back(std::move(result));
    }
    addStringBorrowContract(contract.signature, manifest.arguments, manifest.result);
    auto const valid = core::lang::validate(contract);
    if (!valid) {
        return std::unexpected(error(BackendErrorCode::InvalidManifest,
            "AOT host manifest has an invalid ABI: " + valid.error().message));
    }
    return contract;
}

[[nodiscard]]
bool sameManifest(core::lang::CustomManifest const& left, core::lang::CustomManifest const& right)
{
    return left.provider_key == right.provider_key && left.abi_major == right.abi_major &&
           left.abi_minor == right.abi_minor && left.effect == right.effect &&
           left.may_suspend == right.may_suspend && left.may_reenter == right.may_reenter &&
           left.unsafe_ == right.unsafe_ && left.arguments == right.arguments &&
           left.result == right.result && left.borrow == right.borrow &&
           left.clobber_regions == right.clobber_regions && left.digest == right.digest;
}

void addStringBorrowContract(core::lang::CoreAbiSignature& signature,
                             std::span<core::lang::Type const> const parameters,
                             core::lang::Type const& result)
{
    for (uint64_t index = 0U; index < parameters.size(); ++index) {
        if (isStringType(parameters[static_cast<std::span<core::lang::Type const>::size_type>(index)]) &&
            signature.parameter_borrows[static_cast<std::vector<core::lang::CoreAbiBorrow>::size_type>(index)] ==
                core::lang::CoreAbiBorrow::None) {
            signature.parameter_borrows[static_cast<std::vector<core::lang::CoreAbiBorrow>::size_type>(index)] =
                core::lang::CoreAbiBorrow::Shared;
        }
    }
    if (!isStringType(result)) {
        return;
    }
    auto returned = std::ranges::find_if(signature.return_borrows, [](core::lang::CoreAbiReturnBorrow const& value) {
        return value.result_path.empty();
    });
    if (returned == signature.return_borrows.end()) {
        signature.return_borrows.push_back(core::lang::CoreAbiReturnBorrow{});
        returned = std::prev(signature.return_borrows.end());
    }
    for (uint64_t index = 0U; index < parameters.size(); ++index) {
        if (isStringType(parameters[static_cast<std::span<core::lang::Type const>::size_type>(index)])) {
            returned->origins.push_back({
                .kind = core::lang::CoreAbiBorrowOriginKind::Parameter,
                .index = index,
            });
        }
    }
    returned->origins.push_back({
        .kind = core::lang::CoreAbiBorrowOriginKind::Persistent,
        .index = 0U,
    });
    std::ranges::sort(returned->origins, [](core::lang::CoreAbiBorrowOrigin const& left,
        core::lang::CoreAbiBorrowOrigin const& right) {
        return std::pair{left.kind, left.index} < std::pair{right.kind, right.index};
    });
    auto const duplicates = std::ranges::unique(returned->origins);
    returned->origins.erase(duplicates.begin(), duplicates.end());
    std::ranges::sort(signature.return_borrows, {}, &core::lang::CoreAbiReturnBorrow::result_path);
}

[[nodiscard]]
std::expected<std::vector<ExecutionHostCall>, BackendError> validateHostCalls(
    std::span<ExecutionHostCall const> host_calls,
    std::span<core::lang::CustomManifest const> artifact_manifests)
{
    if (host_calls.size() > MAX_HOST_CALLS || artifact_manifests.size() != host_calls.size()) {
        return std::unexpected(error(BackendErrorCode::InvalidHostCalls,
            "CoreLang host call set exceeds its fixed bound or does not match imports"));
    }
    std::vector<ExecutionHostCall> result;
    result.reserve(host_calls.size());
    for (ExecutionHostCall const& host_call : host_calls) {
        if (!host_call.invoke || host_call.manifest.provider_key.empty() || host_call.manifest.abi_major == 0U ||
            host_call.manifest.may_suspend || host_call.manifest.may_reenter ||
            host_call.manifest.unsafe_ ||
            host_call.manifest.digest != core::lang::customManifestDigest(host_call.manifest)) {
            return std::unexpected(error(BackendErrorCode::InvalidHostCalls,
                "CoreLang host call manifest or handler is invalid"));
        }
        if (std::ranges::any_of(result, [&host_call](ExecutionHostCall const& existing) {
                return existing.manifest.provider_key == host_call.manifest.provider_key;
            })) {
            return std::unexpected(error(BackendErrorCode::InvalidHostCalls,
                "CoreLang host call provider keys must be unique"));
        }
        result.push_back(host_call);
    }
    for (core::lang::CustomManifest const& artifact_manifest : artifact_manifests) {
        auto const host_call = std::ranges::find_if(result, [&artifact_manifest](
            ExecutionHostCall const& candidate) {
            return candidate.manifest.provider_key == artifact_manifest.provider_key;
        });
        if (host_call == result.end() ||
            std::ranges::count_if(artifact_manifests, [&artifact_manifest](
                core::lang::CustomManifest const& candidate) {
                return candidate.provider_key == artifact_manifest.provider_key;
            }) != 1) {
            return std::unexpected(error(BackendErrorCode::InvalidHostCalls,
                "CoreLang host handlers do not match the compiled imports"));
        }
        core::lang::CustomManifest canonical = host_call->manifest;
        if (canonical.effect != core::lang::CustomEffect::Pure && canonical.clobber_regions.empty()) {
            canonical.clobber_regions = {0U};
            canonical.digest = core::lang::customManifestDigest(canonical);
        }
        if (!sameManifest(canonical, artifact_manifest)) {
            return std::unexpected(error(BackendErrorCode::InvalidHostCalls,
                "CoreLang host handlers do not match the compiled imports"));
        }
        host_call->manifest = artifact_manifest;
    }
    for (ExecutionHostCall const& host_call : result) {
        if (std::ranges::count_if(artifact_manifests, [&host_call](
            core::lang::CustomManifest const& candidate) {
                return candidate.provider_key == host_call.manifest.provider_key;
            }) != 1) {
            return std::unexpected(error(BackendErrorCode::InvalidHostCalls,
                "CoreLang compiled imports do not match host handlers"));
        }
    }
    return result;
}

[[nodiscard]]
std::expected<core::lang::CoreAbiCallbackRegistry, BackendError> aotCallbacks(
    core::lang::cni::CniImage const& image,
    std::span<ExecutionHostCall const* const> host_calls,
    ExecutionLimits const& limits,
    std::stop_token const& stop_token,
    CancellationCheck const& cancellation_requested)
{
    core::lang::CoreAbiCallbackRegistry callbacks;
    auto string_arena = std::make_shared<AotStringArena>();
    string_arena->bytes.resize(static_cast<std::vector<uint8_t>::size_type>(limits.max_source_bytes));
    for (core::lang::cni::CniImport const& import : image.imports) {
        auto const host_call = std::ranges::find_if(host_calls, [&import](ExecutionHostCall const* current) {
            return current->manifest.provider_key == import.contract.id;
        });
        if (host_call == host_calls.end()) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "AOT image requested an unknown CoreLang host call"));
        }
        auto expected_contract = callbackContract((*host_call)->manifest);
        if (!expected_contract || import.contract != *expected_contract) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "AOT host import differs from its CoreLang manifest"));
        }
        auto const invoke = (*host_call)->invoke;
        core::lang::CustomManifest const manifest = (*host_call)->manifest;
        auto binding = core::lang::makeCoreAbiCallback(
            import.contract,
            // NOLINTNEXTLINE(bugprone-exception-escape): makeCoreAbiCallback catches and translates throws.
            [invoke, manifest, limits, stop_token, cancellation_requested, string_arena](
                core::lang::CoreAbiInvocation const& invocation) -> core::lang::CoreAbiResult {
                if (isCancelled(stop_token, cancellation_requested)) {
                    return std::unexpected(core::lang::CoreAbiFailure{
                        .code = 1U,
                        .message = "CoreLang execution was cancelled",
                    });
                }
                if (invocation.callback_id != manifest.provider_key ||
                    invocation.arguments.size() != manifest.arguments.size()) {
                    return std::unexpected(core::lang::CoreAbiFailure{
                        .code = 1U,
                        .message = "AOT host call received an invalid id or argument count",
                    });
                }
                std::vector<core::lang::Value> arguments;
                arguments.reserve(manifest.arguments.size());
                for (uint64_t index = 0U; index < manifest.arguments.size(); ++index) {
                    auto value = langValue(
                        invocation.arguments[static_cast<std::span<core::lang::CoreAbiValue const>::size_type>(index)],
                        manifest.arguments[static_cast<std::vector<core::lang::Type>::size_type>(index)],
                        limits);
                    if (!value) {
                        return std::unexpected(core::lang::CoreAbiFailure{
                            .code = 1U,
                            .message = value.error().message,
                        });
                    }
                    arguments.push_back(std::move(*value));
                }
                auto result = invoke(arguments);
                if (!result) {
                    return std::unexpected(core::lang::CoreAbiFailure{
                        .code = 1U,
                        .message = std::move(result.error()),
                    });
                }
                auto const valid = validateValue(*result, manifest.result, limits);
                if (!valid) {
                    return std::unexpected(core::lang::CoreAbiFailure{
                        .code = 1U,
                        .message = valid.error().message,
                    });
                }
                if (isCancelled(stop_token, cancellation_requested)) {
                    return std::unexpected(core::lang::CoreAbiFailure{
                        .code = 1U,
                        .message = "CoreLang execution was cancelled",
                    });
                }
                auto converted = abiValue(*result, limits, string_arena);
                if (!converted) {
                    return std::unexpected(core::lang::CoreAbiFailure{
                        .code = 1U,
                        .message = converted.error().message,
                    });
                }
                return std::move(*converted);
            });
        if (!expected_contract->signature.return_borrows.empty() &&
            std::ranges::any_of(expected_contract->signature.return_borrows, [](
                core::lang::CoreAbiReturnBorrow const& returned) {
                return std::ranges::any_of(returned.origins, [](core::lang::CoreAbiBorrowOrigin const& origin) {
                    return origin.kind == core::lang::CoreAbiBorrowOriginKind::Persistent;
                });
            })) {
            binding.lifetime = string_arena;
            binding.persistent_regions.push_back({
                .data = string_arena->bytes.data(),
                .size = string_arena->bytes.size(),
                .alignment = 1U,
                .mutable_ = false,
                .lifetime = string_arena,
            });
        }
        auto const bound = callbacks.bind(binding);
        if (!bound) {
            return std::unexpected(error(BackendErrorCode::InvalidManifest,
                "AOT callback registration failed: " + bound.error().message));
        }
    }
    if (callbacks.bindings().size() != image.imports.size()) {
        return std::unexpected(error(BackendErrorCode::InvalidManifest,
            "AOT callback set does not match image imports"));
    }
    return callbacks;
}

struct SourceBoundProof final {
    uint64_t loop_multiplier{1U};
    bool has_bounded_loops{false};
};

[[nodiscard]]
bool isSupportedLoopInteger(core::lang::TypeKind const kind) noexcept
{
    switch (kind) {
    case core::lang::TypeKind::I8:
    case core::lang::TypeKind::I16:
    case core::lang::TypeKind::I32:
    case core::lang::TypeKind::I64:
    case core::lang::TypeKind::U8:
    case core::lang::TypeKind::U16:
    case core::lang::TypeKind::U32:
    case core::lang::TypeKind::U64:
        return true;
    default:
        return false;
    }
}

[[nodiscard]]
std::optional<uint64_t> nonnegativeInteger(core::lang::Value const& value)
{
    if (!isSupportedLoopInteger(value.type.kind) || value.bytes.empty() || value.bytes.size() > sizeof(uint64_t)) {
        return std::nullopt;
    }
    uint64_t bits = 0U;
    for (uint64_t index = 0U; index < value.bytes.size(); ++index) {
        bits |= static_cast<uint64_t>(value.bytes[static_cast<std::vector<uint8_t>::size_type>(index)])
            << (index * 8U);
    }
    bool const signed_type = value.type.kind >= core::lang::TypeKind::I8 &&
                             value.type.kind <= core::lang::TypeKind::I64;
    uint64_t const sign_bit = uint64_t{1U} << (value.bytes.size() * 8U - 1U);
    if (signed_type && (bits & sign_bit) != 0U) {
        return std::nullopt;
    }
    return bits;
}

[[nodiscard]]
bool containsName(core::lang::syntax::Expr const& expression,
                 std::string_view const name,
                 uint32_t const depth = 0U)
{
    if (depth >= 512U || (expression.kind == syntax::ExprKind::Name && !expression.path.empty() &&
                          expression.path.front() == name)) {
        return true;
    }
    auto const child_contains = [name, depth](syntax::ExprPtr const& child) {
        return child && containsName(*child, name, depth + 1U);
    };
    if (std::ranges::any_of(expression.children, child_contains) || child_contains(expression.callee)) {
        return true;
    }
    for (syntax::Item const& item : expression.items) {
        if ((item.expression && containsName(*item.expression, name, depth + 1U)) ||
            (item.declaration && item.declaration->initializer &&
             containsName(*item.declaration->initializer, name, depth + 1U)) ||
            (item.function && item.function->body &&
             containsName(*item.function->body, name, depth + 1U))) {
            return true;
        }
    }
    for (syntax::Parameter const& parameter : expression.parameters) {
        if (parameter.defaultValue && containsName(*parameter.defaultValue, name, depth + 1U)) {
            return true;
        }
    }
    for (syntax::Attribute const& attribute : expression.attributes) {
        if (std::ranges::any_of(attribute.arguments, child_contains)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]]
bool indexCanEscapeOrChange(core::lang::syntax::Expr const& expression,
                            std::string_view const name,
                            uint32_t const depth = 0U)
{
    if (depth >= 512U) {
        return true;
    }
    if ((expression.kind == syntax::ExprKind::Assignment || expression.kind == syntax::ExprKind::Exchange) &&
        (!expression.children.empty() && containsName(*expression.children.front(), name))) {
        return true;
    }
    if (expression.kind == syntax::ExprKind::Exchange && expression.children.size() > 1U &&
        containsName(*expression.children[1U], name)) {
        return true;
    }
    if (expression.kind == syntax::ExprKind::Unary &&
        (expression.text == "&" || expression.text == "&mut" || expression.text == "&raw") &&
        !expression.children.empty() && containsName(*expression.children.front(), name)) {
        return true;
    }
    if ((expression.kind == syntax::ExprKind::FunctionLiteral ||
         expression.kind == syntax::ExprKind::MethodLiteral) &&
        containsName(expression, name)) {
        return true;
    }
    auto const child_escapes = [name, depth](syntax::ExprPtr const& child) {
        return child && indexCanEscapeOrChange(*child, name, depth + 1U);
    };
    if (std::ranges::any_of(expression.children, child_escapes) || child_escapes(expression.callee)) {
        return true;
    }
    for (syntax::Item const& item : expression.items) {
        if ((item.expression && indexCanEscapeOrChange(*item.expression, name, depth + 1U)) ||
            (item.declaration && item.declaration->initializer &&
             indexCanEscapeOrChange(*item.declaration->initializer, name, depth + 1U)) ||
            (item.function && item.function->body &&
             indexCanEscapeOrChange(*item.function->body, name, depth + 1U))) {
            return true;
        }
    }
    for (syntax::Parameter const& parameter : expression.parameters) {
        if (parameter.defaultValue && indexCanEscapeOrChange(*parameter.defaultValue, name, depth + 1U)) {
            return true;
        }
    }
    return false;
}

class SourceBoundScanner final {
public:
    SourceBoundScanner(ExecutionLimits const& limits,
                       std::stop_token const& stop_token,
                       CancellationCheck const& cancellation_requested)
        : m_limits(limits), m_stop_token(stop_token), m_cancellation_requested(cancellation_requested)
    {
    }

    [[nodiscard]]
    std::expected<SourceBoundProof, BackendError> scan(core::lang::syntax::Module const& module)
    {
        if (!module.imports.empty()) {
            return std::unexpected(error(BackendErrorCode::UnboundedExecution,
                "CoreLang source imports are outside the bounded source proof"));
        }
        auto const result = scanItems(module.items, 0U);
        if (!result) {
            return std::unexpected(result.error());
        }
        return SourceBoundProof{
            .loop_multiplier = m_loop_multiplier,
            .has_bounded_loops = m_has_bounded_loops,
        };
    }

private:
    [[nodiscard]]
    std::expected<void, BackendError> poll()
    {
        if (isCancelled(m_stop_token, m_cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled,
                "CoreLang bounded-source analysis was cancelled"));
        }
        ++m_nodes;
        if (m_nodes > MAX_INSTRUCTIONS) {
            return reject("CoreLang source exceeds static bounded-analysis complexity");
        }
        return {};
    }

    [[nodiscard]]
    std::expected<void, BackendError> reject(std::string message) const
    {
        return std::unexpected(error(BackendErrorCode::UnboundedExecution, std::move(message)));
    }

    [[nodiscard]]
    std::expected<void, BackendError> scanContract(syntax::FunctionContract const& contract,
        uint32_t const depth)
    {
        for (syntax::ExprPtr const* expression :
             {&contract.precondition, &contract.postcondition}) {
            if (*expression) {
                auto const result = scanExpr(**expression, depth + 1U);
                if (!result) {
                    return result;
                }
            }
        }
        return {};
    }

    [[nodiscard]]
    std::expected<void, BackendError> scanType(syntax::TypeExpressionPtr const& type,
        uint32_t const depth)
    {
        if (!type) {
            return {};
        }
        auto const checked = poll();
        if (!checked) {
            return checked;
        }
        if (depth >= 512U) {
            return reject("CoreLang source exceeds static bounded-analysis depth");
        }
        if (!m_seen_types.insert(type.get()).second) {
            return {};
        }
        if (type->value) {
            auto const result = scanExpr(*type->value, depth + 1U);
            if (!result) {
                return result;
            }
        }
        for (syntax::TypeExpressionPtr const& child : type->children) {
            auto const result = scanType(child, depth + 1U);
            if (!result) {
                return result;
            }
        }
        for (syntax::TypeParameter const& parameter : type->parameters) {
            if (parameter.defaultValue) {
                auto const result = scanExpr(*parameter.defaultValue, depth + 1U);
                if (!result) {
                    return result;
                }
            }
        }
        return scanContract(type->contract, depth + 1U);
    }

    [[nodiscard]]
    std::expected<void, BackendError> scanFunction(syntax::Function const& function,
        uint32_t const depth)
    {
        auto const contract = scanContract(function.contract, depth + 1U);
        if (!contract) {
            return contract;
        }
        auto const declaring_type = scanType(function.declaring_type, depth + 1U);
        if (!declaring_type) {
            return declaring_type;
        }
        auto const result_type = scanType(function.result_expression, depth + 1U);
        if (!result_type) {
            return result_type;
        }
        for (syntax::Parameter const& parameter : function.parameters) {
            auto const type = scanType(parameter.type_expression, depth + 1U);
            if (!type) {
                return type;
            }
            if (parameter.defaultValue) {
                auto const value = scanExpr(*parameter.defaultValue, depth + 1U);
                if (!value) {
                    return value;
                }
            }
        }
        if (function.body) {
            return scanExpr(*function.body, depth + 1U);
        }
        return {};
    }

    [[nodiscard]]
    std::expected<void, BackendError> scanAttributes(std::span<syntax::Attribute const> attributes,
        uint32_t const depth)
    {
        for (syntax::Attribute const& attribute : attributes) {
            for (syntax::ExprPtr const& argument : attribute.arguments) {
                if (argument) {
                    auto const result = scanExpr(*argument, depth + 1U);
                    if (!result) {
                        return result;
                    }
                }
            }
        }
        return {};
    }

    [[nodiscard]]
    std::expected<void, BackendError> scanItems(std::span<syntax::Item const> items,
        uint32_t const depth)
    {
        if (depth >= 512U) {
            return reject("CoreLang source exceeds static bounded-analysis depth");
        }
        for (syntax::Item const& item : items) {
            auto const checked = poll();
            if (!checked) {
                return checked;
            }
            auto const attributes = scanAttributes(item.attributes, depth + 1U);
            if (!attributes) {
                return attributes;
            }
            if (item.declaration && item.declaration->initializer) {
                auto const value = scanExpr(*item.declaration->initializer, depth + 1U);
                if (!value) {
                    return value;
                }
            }
            if (item.function) {
                auto const function = scanFunction(*item.function, depth + 1U);
                if (!function) {
                    return function;
                }
            }
            if (item.expression) {
                auto const expression = scanExpr(*item.expression, depth + 1U);
                if (!expression) {
                    return expression;
                }
            }
            if (item.typeAlias) {
                auto const type = scanType(item.typeAlias->expression, depth + 1U);
                if (!type) {
                    return type;
                }
            }
            if (item.structDeclaration) {
                auto const members = scanItems(item.structDeclaration->members, depth + 1U);
                if (!members) {
                    return members;
                }
            }
            if (item.property) {
                for (syntax::PropertyAccessor const& accessor : item.property->accessors) {
                    auto const accessor_attributes = scanAttributes(accessor.attributes, depth + 1U);
                    if (!accessor_attributes) {
                        return accessor_attributes;
                    }
                    if (accessor.body) {
                        auto const body = scanExpr(*accessor.body, depth + 1U);
                        if (!body) {
                            return body;
                        }
                    }
                }
            }
        }
        return {};
    }

    [[nodiscard]]
    std::expected<uint64_t, BackendError> tripCount(syntax::Expr const& loop)
    {
        if (loop.children.size() != 2U || !loop.pattern || loop.pattern->kind != syntax::PatternKind::Name ||
            loop.pattern->name.empty() || loop.pattern->mutableBinding || !loop.children[0U] ||
            loop.children[0U]->kind != syntax::ExprKind::Range || loop.children[0U]->children.size() != 2U ||
            !loop.children[0U]->children[0U] || !loop.children[0U]->children[1U] ||
            loop.children[0U]->children[0U]->kind != syntax::ExprKind::Literal ||
            loop.children[0U]->children[1U]->kind != syntax::ExprKind::Literal) {
            return std::unexpected(error(BackendErrorCode::UnboundedExecution,
                "CoreLang for-loop requires an immutable named literal range"));
        }
        auto const first = lang::parseLiteral(loop.children[0U]->children[0U]->text);
        auto const end = lang::parseLiteral(loop.children[0U]->children[1U]->text);
        if (!first || !end || first->type != end->type) {
            return std::unexpected(error(BackendErrorCode::UnboundedExecution,
                "CoreLang for-loop bounds must be matching integer literals"));
        }
        auto const first_value = nonnegativeInteger(*first);
        auto const end_value = nonnegativeInteger(*end);
        if (!first_value || !end_value) {
            return std::unexpected(error(BackendErrorCode::UnboundedExecution,
                "CoreLang for-loop bounds must be nonnegative fixed-width integers"));
        }
        uint64_t const trips = *end_value > *first_value ? *end_value - *first_value : 0U;
        if (trips == std::numeric_limits<uint64_t>::max()) {
            return std::unexpected(error(BackendErrorCode::InstructionLimitExceeded,
                "CoreLang for-loop exceeds its static instruction bound"));
        }
        return trips;
    }

    [[nodiscard]]
    std::expected<void, BackendError> addLoopFactor(syntax::Expr const& loop)
    {
        auto const trips = tripCount(loop);
        if (!trips) {
            return std::unexpected(trips.error());
        }
        uint64_t const factor = std::max(uint64_t{1U}, *trips + 1U);
        if (factor > m_limits.max_instructions ||
            m_loop_multiplier > m_limits.max_instructions / factor) {
            return std::unexpected(error(BackendErrorCode::InstructionLimitExceeded,
                "CoreLang finite loops exceed the static instruction bound"));
        }
        m_loop_multiplier *= factor;
        m_has_bounded_loops = true;
        return {};
    }

    [[nodiscard]]
    std::expected<void, BackendError> scanExpr(syntax::Expr const& expression,
        uint32_t const depth)
    {
        auto const checked = poll();
        if (!checked) {
            return checked;
        }
        if (depth >= 512U) {
            return reject("CoreLang source exceeds static bounded-analysis depth");
        }
        if (!m_seen_expressions.insert(&expression).second) {
            return {};
        }
        if (expression.kind == syntax::ExprKind::While || expression.kind == syntax::ExprKind::DoWhile ||
            expression.kind == syntax::ExprKind::Loop || expression.kind == syntax::ExprKind::Custom) {
            return reject("CoreLang source contains unsupported dynamic control flow");
        }
        if (expression.kind == syntax::ExprKind::For) {
            auto const factor = addLoopFactor(expression);
            if (!factor) {
                return factor;
            }
            if (!expression.pattern || !expression.children[1U] ||
                indexCanEscapeOrChange(*expression.children[1U], expression.pattern->name)) {
                return reject("CoreLang for-loop induction variable is written, aliased, or captured");
            }
        }
        if (expression.callee) {
            auto const callee = scanExpr(*expression.callee, depth + 1U);
            if (!callee) {
                return callee;
            }
        }
        for (syntax::ExprPtr const& child : expression.children) {
            if (child) {
                auto const result = scanExpr(*child, depth + 1U);
                if (!result) {
                    return result;
                }
            }
        }
        auto const items = scanItems(expression.items, depth + 1U);
        if (!items) {
            return items;
        }
        auto const attributes = scanAttributes(expression.attributes, depth + 1U);
        if (!attributes) {
            return attributes;
        }
        auto const type = scanType(expression.type_expression, depth + 1U);
        if (!type) {
            return type;
        }
        for (syntax::TypeExpressionPtr const& argument : expression.type_arguments) {
            auto const result = scanType(argument, depth + 1U);
            if (!result) {
                return result;
            }
        }
        for (syntax::Parameter const& parameter : expression.parameters) {
            auto const parameter_type = scanType(parameter.type_expression, depth + 1U);
            if (!parameter_type) {
                return parameter_type;
            }
            if (parameter.defaultValue) {
                auto const value = scanExpr(*parameter.defaultValue, depth + 1U);
                if (!value) {
                    return value;
                }
            }
        }
        return {};
    }

    ExecutionLimits const& m_limits;
    std::stop_token const& m_stop_token;
    CancellationCheck const& m_cancellation_requested;
    uint64_t m_loop_multiplier{1U};
    uint64_t m_nodes{0U};
    bool m_has_bounded_loops{false};
    std::unordered_set<syntax::Expr const*> m_seen_expressions;
    std::unordered_set<syntax::TypeExpression const*> m_seen_types;
};

[[nodiscard]]
std::expected<SourceBoundProof, BackendError> inspectSourceBounds(
    core::lang::Source const& source,
    core::lang::CompilerRegistry const& registry,
    ExecutionLimits const& limits,
    std::stop_token const& stop_token,
    CancellationCheck const& cancellation_requested)
{
    auto const header = lang::syntax::parseImportHeader(source.id, source.text);
    if (!header) {
        return std::unexpected(error(BackendErrorCode::CompileFailed,
            header.error().empty() ? "CoreLang source header is malformed"
            : header.error().front().message,
            header.error().empty()
            ? std::nullopt
            : std::optional<uint64_t>{header.error().front().offset}));
    }
    auto active = registry.activate(header->uses);
    if (!active) {
        return std::unexpected(error(BackendErrorCode::InvalidConfiguration,
            "CoreLang bounded source could not activate its rulesets: " + active.error()));
    }
    for (lang::Ruleset const* ruleset : active->rulesets) {
        if (!ruleset->automaticImports.empty() || !ruleset->triggers.empty() ||
            std::ranges::any_of(ruleset->operations, [](lang::Operation const& operation) {
                return static_cast<bool>(operation.expand);
            })) {
            return std::unexpected(error(BackendErrorCode::UnboundedExecution,
                "CoreLang automatic imports or source expansion are not bounded"));
        }
    }
    auto module = lang::syntax::parse(source.id, source.text, active->parseHooks());
    if (!module) {
        return std::unexpected(error(BackendErrorCode::CompileFailed,
            module.error().empty() ? "CoreLang source is malformed"
            : module.error().front().message,
            module.error().empty()
            ? std::nullopt
            : std::optional<uint64_t>{module.error().front().offset}));
    }
    if (isCancelled(stop_token, cancellation_requested)) {
        return std::unexpected(error(BackendErrorCode::Cancelled,
            "CoreLang bounded-source analysis was cancelled"));
    }
    SourceBoundScanner scanner{limits, stop_token, cancellation_requested};
    return scanner.scan(*module);
}

std::expected<uint64_t, BackendError> boundedInstructionCountImpl(
    core::lang::Artifact const& artifact,
    FunctionTarget const& target,
    ExecutionLimits const& limits,
    InstructionBudget const& budget);

} // namespace

std::expected<CompiledProgram, BackendError> compileBounded(
    core::lang::Source source,
    core::lang::CompilerRegistry const& registry,
    ExecutionLimits const& limits,
    std::stop_token const& stop_token,
    CancellationCheck const& cancellation_requested)
{
    try {
        auto const valid_limits = validateLimits(limits);
        if (!valid_limits) {
            return std::unexpected(valid_limits.error());
        }
        if (isCancelled(stop_token, cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled,
                "CoreLang compilation was cancelled"));
        }
        if (source.id.empty() || source.id.size() > MAX_SYMBOL_BYTES) {
            return std::unexpected(error(BackendErrorCode::InvalidConfiguration,
                "CoreLang source identifier is empty or exceeds its bound"));
        }
        if (source.text.empty() || source.text.size() > limits.max_source_bytes) {
            return std::unexpected(error(BackendErrorCode::SourceTooLarge,
                "CoreLang source is empty or exceeds its byte bound"));
        }
        auto const source_proof = inspectSourceBounds(
            source, registry, limits, stop_token, cancellation_requested);
        if (!source_proof) {
            return std::unexpected(source_proof.error());
        }
        std::vector<uint8_t> const source_bytes = bytesOf(source.text);
        core::lang::CompileOptions options{
            .optimization_configuration = {},
            .compilationInputs = {},
            .cancelled = [stop_token, cancellation_requested] {
                return isCancelled(stop_token, cancellation_requested);
            },
        };
        auto compiled = core::lang::compile(std::move(source), registry, std::move(options));
        if (!compiled) {
            if (isCancelled(stop_token, cancellation_requested)) {
                return std::unexpected(error(BackendErrorCode::Cancelled,
                    "CoreLang compilation was cancelled"));
            }
            std::optional<uint64_t> const source_offset = compiled.error().empty()
                ? std::nullopt
                : std::optional<uint64_t>{compiled.error().front().offset};
            return std::unexpected(error(BackendErrorCode::CompileFailed,
                diagnosticsMessage(compiled.error()),
                source_offset));
        }
        if (isCancelled(stop_token, cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled,
                "CoreLang compilation was cancelled"));
        }
        if (compiled->bytes.empty() || compiled->bytes.size() > limits.max_bytecode_bytes) {
            return std::unexpected(error(BackendErrorCode::BytecodeTooLarge,
                "CoreLang bytecode is empty or exceeds its byte bound"));
        }
        CompiledProgram result;
        result.compiled = std::move(*compiled);
        result.source_sha256_ = core::lang::sha256(source_bytes);
        result.bytecode_sha256_ = core::lang::sha256(result.compiled.bytes);
        result.loop_multiplier_ = source_proof->loop_multiplier;
        result.has_bounded_loops_ = source_proof->has_bounded_loops;
        return result;
    } catch (std::exception const& exception) {
        if (isCancelled(stop_token, cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled,
                "CoreLang compilation was cancelled"));
        }
        return std::unexpected(error(BackendErrorCode::CompileFailed,
            std::string{"CoreLang compilation failed: "} + exception.what()));
    } catch (...) {
        if (isCancelled(stop_token, cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled,
                "CoreLang compilation was cancelled"));
        }
        return std::unexpected(error(BackendErrorCode::CompileFailed,
            "CoreLang compilation failed with an unknown error"));
    }
}

std::array<uint8_t, 32U> const& CompiledProgram::sourceSha256() const noexcept
{
    return source_sha256_;
}

std::array<uint8_t, 32U> const& CompiledProgram::bytecodeSha256() const noexcept
{
    return bytecode_sha256_;
}

std::expected<uint64_t, BackendError> boundedInstructionCount(
    core::lang::Artifact const& artifact,
    std::string_view const module_id,
    std::string_view const entry,
    ExecutionLimits const& limits,
    uint64_t const execution_count)
{
    return boundedInstructionCountImpl(
        artifact,
        FunctionTarget{.module_id = module_id, .entry = entry},
        limits,
        InstructionBudget{
            .execution_count = execution_count,
            .loop_multiplier = 1U,
            .allow_verified_cycles = false,
        });
}

std::expected<uint64_t, BackendError> boundedInstructionCount(
    CompiledProgram const& program,
    std::string_view const module_id,
    std::string_view const entry,
    ExecutionLimits const& limits,
    uint64_t const execution_count)
{
    if (program.compiled.verifiedArtifact == nullptr ||
        core::lang::sha256(program.compiled.bytes) != program.bytecode_sha256_) {
        return std::unexpected(error(BackendErrorCode::InvalidArtifact,
            "CoreLang bounded proof no longer matches the compiled bytecode"));
    }
    auto const serialized = core::lang::serialize(program.compiled.artifact);
    if (!serialized || *serialized != program.compiled.bytes) {
        return std::unexpected(error(BackendErrorCode::InvalidArtifact,
            "CoreLang artifact changed after bounded-source validation"));
    }
    return boundedInstructionCountImpl(program.compiled.artifact,
        FunctionTarget{.module_id = module_id, .entry = entry},
        limits,
        InstructionBudget{
            .execution_count = execution_count,
            .loop_multiplier = program.loop_multiplier_,
            .allow_verified_cycles = program.has_bounded_loops_,
        });
}

namespace {

std::expected<uint64_t, BackendError> boundedInstructionCountImpl(
    core::lang::Artifact const& artifact,
    FunctionTarget const& target,
    ExecutionLimits const& limits,
    InstructionBudget const& budget)
{
    auto const valid_limits = validateLimits(limits);
    if (!valid_limits) {
        return std::unexpected(valid_limits.error());
    }
    if (budget.execution_count == 0U || budget.execution_count > limits.max_execution_count) {
        return std::unexpected(error(BackendErrorCode::InvalidConfiguration,
            "CoreLang execution count exceeds its configured bound"));
    }
    if (artifact.modules.empty() || artifact.functions.empty() ||
        artifact.modules.size() > limits.max_bytecode_bytes ||
        artifact.functions.size() > limits.max_bytecode_bytes) {
        return std::unexpected(error(BackendErrorCode::InvalidArtifact,
            "CoreLang artifact exceeds static-validation structure bounds"));
    }

    std::vector<uint8_t> function_state(artifact.functions.size(), 0U);
    std::vector<uint64_t> function_costs(artifact.functions.size(), 0U);
    uint64_t analyzed_blocks = 0U;
    uint64_t analyzed_instructions = 0U;

    struct FunctionVisit final {
        uint64_t function_index;
        uint32_t call_depth;
    };
    struct BlockVisit final {
        uint64_t block_index;
        uint32_t branch_depth;
    };

    auto count_function = [&](auto&& self, FunctionVisit const visit)
        -> std::expected<uint64_t, BackendError> {
        if (visit.function_index >= artifact.functions.size()) {
            return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                "CoreLang call references an invalid function"));
        }
        if (visit.call_depth >= 512U) {
            return std::unexpected(error(BackendErrorCode::UnboundedExecution,
                "CoreLang call graph exceeds static-validation depth"));
        }
        uint8_t& state = function_state[
            static_cast<std::vector<uint8_t>::size_type>(visit.function_index)];
        if (state == 1U) {
            return std::unexpected(error(BackendErrorCode::UnboundedExecution,
                "CoreLang bytecode contains recursive calls"));
        }
        if (state == 2U) {
            return function_costs[
                static_cast<std::vector<uint64_t>::size_type>(visit.function_index)];
        }
        state = 1U;
        core::lang::Function const& function =
            artifact.functions[
                static_cast<std::vector<core::lang::Function>::size_type>(visit.function_index)];
        if (function.blocks.empty() || function.blocks.size() > limits.max_bytecode_bytes) {
            return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                "CoreLang function has no bounded executable blocks"));
        }
        std::vector<uint8_t> block_state(function.blocks.size(), 0U);
        std::vector<uint64_t> block_costs(function.blocks.size(), 0U);

        auto count_block = [&](auto&& count_self, BlockVisit const block_visit)
            -> std::expected<uint64_t, BackendError> {
            if (block_visit.block_index >= function.blocks.size()) {
                return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                    "CoreLang branch references an invalid block"));
            }
            if (block_visit.branch_depth >= 512U) {
                return std::unexpected(error(BackendErrorCode::UnboundedExecution,
                    "CoreLang control-flow depth exceeds static-validation bound"));
            }
            uint8_t& mark = block_state[
                static_cast<std::vector<uint8_t>::size_type>(block_visit.block_index)];
            if (mark == 1U) {
                if (budget.allow_verified_cycles) {
                    return uint64_t{0U};
                }
                return std::unexpected(error(BackendErrorCode::UnboundedExecution,
                    "CoreLang bytecode contains a control-flow cycle"));
            }
            if (mark == 2U) {
                return block_costs[
                    static_cast<std::vector<uint64_t>::size_type>(block_visit.block_index)];
            }
            mark = 1U;
            core::lang::Block const& block =
                function.blocks[
                    static_cast<std::vector<core::lang::Block>::size_type>(block_visit.block_index)];
            if (block.instructions.empty()) {
                return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                    "CoreLang bytecode contains an empty block"));
            }
            analyzed_blocks += 1U;
            if (analyzed_blocks > limits.max_bytecode_bytes) {
                return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                    "CoreLang artifact exceeds static-validation block bound"));
            }
            uint64_t local_cost = 0U;
            for (core::lang::Instruction const& instruction : block.instructions) {
                analyzed_instructions += 1U;
                if (analyzed_instructions > limits.max_bytecode_bytes) {
                    return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                        "CoreLang artifact exceeds static-validation instruction bound"));
                }
                if (instruction.opcode == core::lang::Op::IndirectCall ||
                    instruction.opcode == core::lang::Op::StaticBegin) {
                    return std::unexpected(error(BackendErrorCode::UnboundedExecution,
                        "CoreLang entry uses an unsupported indirect call or static init"));
                }
                if (local_cost >= limits.max_instructions) {
                    return std::unexpected(error(BackendErrorCode::InstructionLimitExceeded,
                        "CoreLang entry exceeds its instruction-step bound"));
                }
                ++local_cost;
                if (instruction.opcode == core::lang::Op::Call) {
                    if (instruction.operands.size() < 2U ||
                        instruction.operands[1U] != instruction.operands.size() - 2U) {
                        return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                            "CoreLang bytecode contains a malformed call"));
                    }
                    auto const callee = self(self, FunctionVisit{
                        .function_index = instruction.operands.front(),
                        .call_depth = visit.call_depth + 1U,
                    });
                    if (!callee) {
                        return std::unexpected(callee.error());
                    }
                    if (*callee > limits.max_instructions - local_cost) {
                        return std::unexpected(error(BackendErrorCode::InstructionLimitExceeded,
                            "CoreLang entry exceeds its instruction-step bound"));
                    }
                    local_cost += *callee;
                }
            }

            std::vector<uint64_t> successors;
            core::lang::Instruction const& terminator = block.instructions.back();
            if (terminator.opcode == core::lang::Op::Branch) {
                if (terminator.operands.size() < 2U ||
                    terminator.operands[1U] != terminator.operands.size() - 2U) {
                    return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                        "CoreLang bytecode contains a malformed branch"));
                }
                successors.push_back(terminator.operands.front());
            } else if (terminator.opcode == core::lang::Op::ConditionalBranch) {
                if (terminator.operands.size() < 5U) {
                    return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                        "CoreLang bytecode contains a malformed conditional branch"));
                }
                uint64_t const true_argument_count = terminator.operands[2U];
                if (true_argument_count > terminator.operands.size() - 3U) {
                    return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                        "CoreLang bytecode contains a malformed conditional branch"));
                }
                uint64_t const false_target_offset = 3U + true_argument_count;
                if (false_target_offset + 2U > terminator.operands.size() ||
                    terminator.operands[false_target_offset + 1U] !=
                        terminator.operands.size() - (false_target_offset + 2U)) {
                    return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                        "CoreLang bytecode contains a malformed conditional branch"));
                }
                successors.push_back(terminator.operands[1U]);
                successors.push_back(terminator.operands[false_target_offset]);
            } else if (terminator.opcode != core::lang::Op::ReturnValue &&
                       terminator.opcode != core::lang::Op::ReturnUnit &&
                       terminator.opcode != core::lang::Op::Halt) {
                return std::unexpected(error(BackendErrorCode::InvalidArtifact,
                    "CoreLang basic block has no supported terminator"));
            }

            uint64_t longest_successor = 0U;
            for (uint64_t const successor : successors) {
                auto const successor_cost = count_self(count_self, BlockVisit{
                    .block_index = successor,
                    .branch_depth = block_visit.branch_depth + 1U,
                });
                if (!successor_cost) {
                    return std::unexpected(successor_cost.error());
                }
                longest_successor = std::max(longest_successor, *successor_cost);
            }
            if (local_cost > limits.max_instructions ||
                longest_successor > limits.max_instructions - local_cost) {
                return std::unexpected(error(BackendErrorCode::InstructionLimitExceeded,
                    "CoreLang entry exceeds its instruction-step bound"));
            }
            uint64_t const total = local_cost + longest_successor;
            mark = 2U;
            block_costs[
                static_cast<std::vector<uint64_t>::size_type>(block_visit.block_index)] = total;
            return total;
        };

        auto const cost = count_block(count_block, BlockVisit{
            .block_index = 0U,
            .branch_depth = 0U,
        });
        if (!cost) {
            return std::unexpected(cost.error());
        }
        state = 2U;
        function_costs[
            static_cast<std::vector<uint64_t>::size_type>(visit.function_index)] = *cost;
        return *cost;
    };

    uint64_t total = 0U;
    auto add_function = [&](uint64_t const function_index) -> std::expected<void, BackendError> {
        if (function_index == core::lang::NO_OPERAND) {
            return {};
        }
        auto const cost = count_function(count_function, FunctionVisit{
            .function_index = function_index,
            .call_depth = 0U,
        });
        if (!cost) {
            return std::unexpected(cost.error());
        }
        if (*cost > limits.max_instructions - std::min(total, limits.max_instructions)) {
            return std::unexpected(error(BackendErrorCode::InstructionLimitExceeded,
                "CoreLang entry exceeds its instruction-step bound"));
        }
        total += *cost;
        return {};
    };
    for (core::lang::Module const& module : artifact.modules) {
        auto const added = add_function(module.init_function);
        if (!added) {
            return std::unexpected(added.error());
        }
    }

    auto const module = std::ranges::find(artifact.modules, target.module_id, &core::lang::Module::id);
    if (module == artifact.modules.end() ||
        std::ranges::count(artifact.modules, target.module_id, &core::lang::Module::id) != 1) {
        return std::unexpected(error(BackendErrorCode::InvalidArtifact,
            "CoreLang entry module is missing or ambiguous"));
    }
    auto const exported = std::ranges::find_if(module->exports, [target](core::lang::Export const& candidate) {
        return candidate.kind == core::lang::ExportKind::Function && candidate.name == target.entry;
    });
    if (exported == module->exports.end() ||
        std::ranges::count_if(module->exports, [target](core::lang::Export const& candidate) {
            return candidate.kind == core::lang::ExportKind::Function && candidate.name == target.entry;
        }) != 1) {
        return std::unexpected(error(BackendErrorCode::InvalidArtifact,
            "CoreLang entry export is missing or ambiguous"));
    }
    auto const added = add_function(exported->target);
    if (!added) {
        return std::unexpected(added.error());
    }
    if (budget.loop_multiplier == 0U || total > limits.max_instructions / budget.loop_multiplier) {
        return std::unexpected(error(BackendErrorCode::InstructionLimitExceeded,
            "CoreLang finite-loop execution exceeds its static instruction bound"));
    }
    total *= budget.loop_multiplier;
    if (total > limits.max_instructions / budget.execution_count) {
        return std::unexpected(error(BackendErrorCode::InstructionLimitExceeded,
            "CoreLang execution exceeds its total instruction-step bound"));
    }
    return total * budget.execution_count;
}

[[nodiscard]]
std::expected<void, BackendError> registerRuntimeProviders(
    core::lang::Runtime& runtime,
    std::span<ExecutionHostCall const* const> host_calls,
    ExecutionLimits const& limits,
    std::stop_token const& stop_token,
    CancellationCheck const& cancellation_requested)
{
    for (ExecutionHostCall const* host_call : host_calls) {
        core::lang::CustomProvider provider{
            .manifest = host_call->manifest,
            .invoke = [invoke = host_call->invoke,
                       manifest = host_call->manifest,
                       limits,
                       stop_token,
                       cancellation_requested](
                          core::lang::CustomContext&,
                          std::span<core::lang::Value const> arguments) -> core::lang::CustomOutcome {
                try {
                    if (isCancelled(stop_token, cancellation_requested)) {
                        return core::lang::CustomFail{.code = 1U, .message = "CoreLang execution was cancelled"};
                    }
                    if (arguments.size() != manifest.arguments.size()) {
                        return core::lang::CustomFail{
                            .code = 1U,
                            .message = "CoreLang host call received an invalid argument count",
                        };
                    }
                    for (uint64_t index = 0U; index < arguments.size(); ++index) {
                        auto const valid = validateValue(
                            arguments[static_cast<std::span<core::lang::Value const>::size_type>(index)],
                            manifest.arguments[static_cast<std::vector<core::lang::Type>::size_type>(index)],
                            limits);
                        if (!valid) {
                            return core::lang::CustomFail{.code = 1U, .message = valid.error().message};
                        }
                    }
                    auto result = invoke(arguments);
                    if (!result) {
                        return core::lang::CustomFail{.code = 1U, .message = std::move(result.error())};
                    }
                    auto const valid = validateValue(*result, manifest.result, limits);
                    if (!valid) {
                        return core::lang::CustomFail{.code = 1U, .message = valid.error().message};
                    }
                    if (isCancelled(stop_token, cancellation_requested)) {
                        return core::lang::CustomFail{
                            .code = 1U,
                            .message = "CoreLang execution was cancelled",
                        };
                    }
                    return core::lang::CustomComplete{.value = std::move(*result)};
                } catch (std::exception const& exception) {
                    return core::lang::CustomFail{
                        .code = 1U,
                        .message = std::string{"CoreLang host call failed: "} + exception.what(),
                    };
                } catch (...) {
                    return core::lang::CustomFail{
                        .code = 1U,
                        .message = "CoreLang host call failed with an unknown error",
                    };
                }
            },
            .cancel = {},
        };
        auto const registered = runtime.registerProvider(std::move(provider));
        if (!registered) {
            return std::unexpected(error(BackendErrorCode::InvalidHostCalls,
                "CoreLang runtime provider registration failed: " +
                registered.error().message));
        }
    }
    return {};
}

[[nodiscard]]
std::expected<core::lang::Value, BackendError> runRuntimeBackend(
    CompiledProgram const& program,
    ExecutionRequest const& request,
    std::span<ExecutionHostCall const* const> host_calls)
{
    core::lang::RuntimeOptions options{
        .jit_mode = request.backend == Backend::Jit ? core::lang::JitMode::Required
            : core::lang::JitMode::Disabled,
        .jit_coverage = request.backend == Backend::Jit ? core::lang::JitCoverage::Full
            : core::lang::JitCoverage::Annotated,
        .jit_diagnostic = {},
        .optimization_configuration = {},
        .native_artifact = {},
    };
    core::lang::Runtime runtime{options};
    auto const registered = registerRuntimeProviders(runtime,
        host_calls,
        request.limits,
        request.stop_token,
        request.cancellation_requested);
    if (!registered) {
        return std::unexpected(registered.error());
    }
    if (isCancelled(request.stop_token, request.cancellation_requested)) {
        return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
    }
    auto const loaded = runtime.loadCompiled(program.compiled);
    if (!loaded) {
        BackendErrorCode const code = request.backend == Backend::Jit ? BackendErrorCode::BackendUnavailable
            : BackendErrorCode::ExecutionFailed;
        return std::unexpected(error(code, "CoreLang runtime load failed: " + loaded.error().message));
    }
    if (isCancelled(request.stop_token, request.cancellation_requested)) {
        return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
    }
    auto executed = runtime.execute(*loaded, request.module_id, request.entry, {}, {});
    if (!executed) {
        if (isCancelled(request.stop_token, request.cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
        }
        return std::unexpected(error(BackendErrorCode::ExecutionFailed,
            "CoreLang runtime execution failed: " + executed.error().message));
    }
    if (!std::holds_alternative<core::lang::Completed>(*executed)) {
        if (isCancelled(request.stop_token, request.cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
        }
        if (auto const* const failed = std::get_if<core::lang::Failed>(&*executed)) {
            return std::unexpected(error(BackendErrorCode::ExecutionFailed, failed->failure.message));
        }
        return std::unexpected(error(BackendErrorCode::ExecutionFailed,
            "CoreLang entry did not complete synchronously"));
    }
    core::lang::Value result = std::move(std::get<core::lang::Completed>(*executed).value);
    auto const valid = validateValue(result, request.expected_result_type, request.limits);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    if (isCancelled(request.stop_token, request.cancellation_requested)) {
        return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
    }
    return result;
}

[[nodiscard]]
std::expected<std::pair<core::lang::Value, BackendReceipt>, BackendError> runAotBackend(
    CompiledProgram const& program,
    ExecutionRequest const& request,
    std::span<ExecutionHostCall const* const> host_calls)
{
    if (isCancelled(request.stop_token, request.cancellation_requested)) {
        return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
    }
    auto const compiler_host = core::lang::CoreAOT::compilerHostTarget();
    auto const runtime_host = core::lang::CoreRT::hostTarget();
    if (!compiler_host || !runtime_host || *compiler_host != *runtime_host ||
        compiler_host->endianness != core::lang::CoreAbiEndianness::Little) {
        return std::unexpected(error(BackendErrorCode::BackendUnavailable,
            "CoreLang AOT compiler and runtime do not share a supported host target"));
    }
    auto emitted = core::lang::CoreAOT::compile(
        program.compiled.artifact,
        {
            .compiler_host = *compiler_host,
            .targets = {
                core::lang::CoreAotTarget{
                    .target = *compiler_host,
                    .variants = {
                        core::lang::CoreAbiCpuVariant{
                            .id = "baseline",
                            .baseline = true,
                            .required_features = {},
                        },
                    },
                    .toolchain = {},
                },
            },
            .producer = "MinecraftClone.CoreLang.BoundedExecutor/0.1.5",
            .optimization_configuration = {},
        });
    if (!emitted || emitted->images.size() != 1U) {
        return std::unexpected(error(BackendErrorCode::BackendUnavailable,
            emitted ? "CoreLang AOT did not emit exactly one host image"
            : "CoreLang AOT compilation failed: " + emitted.error().message));
    }
    if (isCancelled(request.stop_token, request.cancellation_requested)) {
        return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
    }
    core::lang::CoreAotImage const& output = emitted->images.front();
    if (output.target != *compiler_host) {
        return std::unexpected(error(BackendErrorCode::BackendUnavailable,
            "CoreLang AOT output target differs from the requested host"));
    }
    if (output.binary.empty() || output.binary.size() > request.limits.max_native_image_bytes ||
        output.text.size() > request.limits.max_native_image_bytes) {
        return std::unexpected(error(BackendErrorCode::NativeImageTooLarge,
            "CoreLang AOT output is empty, mismatched, or exceeds its byte bound"));
    }
    auto const entry = std::ranges::find(output.image.entries, request.entry, &core::lang::cni::CniEntry::name);
    auto const initializer = std::ranges::find(output.image.entries, true, &core::lang::cni::CniEntry::initializer);
    auto expected_entry_type = coreAbiType(request.expected_result_type);
    if (!expected_entry_type) {
        return std::unexpected(expected_entry_type.error());
    }
    if (entry == output.image.entries.end() || entry->initializer || !entry->signature.parameters.empty() ||
        entry->signature.result != *expected_entry_type || initializer == output.image.entries.end() ||
        initializer->signature.result != core::lang::CoreAbiType::unit() ||
        !initializer->signature.parameters.empty() ||
        std::ranges::count_if(output.image.entries, [request](core::lang::cni::CniEntry const& candidate) {
            return candidate.name == request.entry && !candidate.initializer;
        }) != 1 ||
        std::ranges::count_if(output.image.entries, [](core::lang::cni::CniEntry const& candidate) {
            return candidate.initializer;
        }) != 1) {
        return std::unexpected(error(BackendErrorCode::InvalidManifest,
            "CoreLang AOT image has an invalid entry or initializer contract"));
    }
    auto callbacks = aotCallbacks(output.image,
        host_calls,
        request.limits,
        request.stop_token,
        request.cancellation_requested);
    if (!callbacks) {
        return std::unexpected(callbacks.error());
    }
    auto runtime = core::lang::CoreRT::load(output.image,
        core::lang::ArtifactTrust::Trusted,
        core::lang::CoreRtEnvironment{
            .target = *runtime_host,
            .cpu_features = {},
            .producer_verifier = {},
        },
        *callbacks);
    if (!runtime) {
        return std::unexpected(error(BackendErrorCode::BackendUnavailable,
            "CoreLang AOT image could not be loaded: " + runtime.error().message));
    }
    if (isCancelled(request.stop_token, request.cancellation_requested)) {
        return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
    }
    auto const prepared = runtime->prepareForExecution();
    if (!prepared) {
        return std::unexpected(error(BackendErrorCode::ExecutionFailed,
            "CoreLang AOT image preparation failed: " + prepared.error().message));
    }
    if (isCancelled(request.stop_token, request.cancellation_requested)) {
        return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
    }
    auto result = runtime->invokeEntry(request.entry, {});
    if (!result) {
        if (isCancelled(request.stop_token, request.cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
        }
        return std::unexpected(error(BackendErrorCode::ExecutionFailed,
            "CoreLang AOT invocation failed: " + result.error().message));
    }
    auto value = langValue(*result, request.expected_result_type, request.limits);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (isCancelled(request.stop_token, request.cancellation_requested)) {
        return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
    }
    BackendReceipt receipt{
        .requested_backend = Backend::Aot,
        .actual_backend = Backend::Aot,
        .source_sha256 = program.sourceSha256(),
        .bytecode_sha256 = program.bytecodeSha256(),
        .native_image_sha256 = core::lang::sha256(output.binary),
        .compiler_version = "CoreLang/" + core::lang::toString(core::lang::CORE_LANGUAGE_015),
        .target = core::lang::toString(*compiler_host),
    };
    return std::pair<core::lang::Value, BackendReceipt>{std::move(*value), std::move(receipt)};
}

[[nodiscard]]
std::string runtimeTarget(Backend const backend)
{
    if (backend == Backend::Interpreter) {
        return "CoreBC";
    }
    auto const target = core::lang::CoreRT::hostTarget();
    if (!target) {
        return {};
    }
    return core::lang::toString(*target);
}

} // namespace

std::expected<ExecutionResult, BackendError> executeBounded(ExecutionRequest request)
{
    try {
        auto const valid_limits = validateLimits(request.limits);
        if (!valid_limits) {
            return std::unexpected(valid_limits.error());
        }
        if (isCancelled(request.stop_token, request.cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
        }
        if (request.execution_count == 0U || request.execution_count > request.limits.max_execution_count) {
            return std::unexpected(error(BackendErrorCode::InvalidConfiguration,
                "CoreLang execution count exceeds its configured bound"));
        }
        if (request.module_id.empty() || request.module_id.size() > MAX_SYMBOL_BYTES ||
            request.entry.empty() || request.entry.size() > MAX_SYMBOL_BYTES) {
            return std::unexpected(error(BackendErrorCode::InvalidConfiguration,
                "CoreLang module or entry name exceeds its bound"));
        }
        if (request.program.compiled.bytes.empty() ||
            request.program.compiled.bytes.size() > request.limits.max_bytecode_bytes) {
            return std::unexpected(error(BackendErrorCode::BytecodeTooLarge,
                "CoreLang bytecode is empty or exceeds its byte bound"));
        }
        auto const declared_result_type = exportedResultType(
            request.program.compiled.artifact,
            FunctionTarget{
                .module_id = request.module_id,
                .entry = request.entry,
            });
        if (!declared_result_type || *declared_result_type != request.expected_result_type) {
            return std::unexpected(error(BackendErrorCode::InvalidResultType,
                "CoreLang entry does not declare the requested result type"));
        }
        auto const instruction_count = boundedInstructionCount(
            request.program,
            request.module_id,
            request.entry,
            request.limits,
            request.execution_count);
        if (!instruction_count) {
            return std::unexpected(instruction_count.error());
        }
        auto host_calls = validateHostCalls(
            request.host_calls, request.program.compiled.artifact.customs);
        if (!host_calls) {
            return std::unexpected(host_calls.error());
        }
        std::vector<ExecutionHostCall const*> host_call_refs;
        host_call_refs.reserve(host_calls->size());
        for (ExecutionHostCall const& host_call : *host_calls) {
            host_call_refs.push_back(&host_call);
        }
        if (request.backend == Backend::Aot) {
            auto result = runAotBackend(request.program, request, host_call_refs);
            if (!result) {
                return std::unexpected(result.error());
            }
            return ExecutionResult{
                .value = std::move(result->first),
                .receipt = std::move(result->second),
            };
        }
        if (request.backend != Backend::Interpreter && request.backend != Backend::Jit) {
            return std::unexpected(error(BackendErrorCode::BackendUnavailable,
                "Requested CoreLang backend is unknown"));
        }
        auto result = runRuntimeBackend(request.program, request, host_call_refs);
        if (!result) {
            return std::unexpected(result.error());
        }
        std::string target = runtimeTarget(request.backend);
        if (target.empty()) {
            return std::unexpected(error(BackendErrorCode::BackendUnavailable,
                "CoreLang runtime host target is unsupported"));
        }
        BackendReceipt receipt{
            .requested_backend = request.backend,
            .actual_backend = request.backend,
            .source_sha256 = request.program.sourceSha256(),
            .bytecode_sha256 = request.program.bytecodeSha256(),
            .native_image_sha256 = std::nullopt,
            .compiler_version = "CoreLang/" + core::lang::toString(core::lang::CORE_LANGUAGE_015),
            .target = std::move(target),
        };
        return ExecutionResult{
            .value = std::move(*result),
            .receipt = std::move(receipt),
        };
    } catch (std::exception const& exception) {
        if (isCancelled(request.stop_token, request.cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
        }
        return std::unexpected(error(BackendErrorCode::ExecutionFailed,
            std::string{"CoreLang execution failed: "} + exception.what()));
    } catch (...) {
        if (isCancelled(request.stop_token, request.cancellation_requested)) {
            return std::unexpected(error(BackendErrorCode::Cancelled, "CoreLang execution was cancelled"));
        }
        return std::unexpected(error(BackendErrorCode::ExecutionFailed,
            "CoreLang execution failed with an unknown error"));
    }
}

} // namespace shared::corelang
