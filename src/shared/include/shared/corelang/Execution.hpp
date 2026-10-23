#pragma once

#include <shared/corelang/ExecutionTypes.hpp>

#include <core/lang/Bytecode.hpp>
#include <core/lang/Compiler.hpp>
#include <core/lang/Registry.hpp>
#include <core/lang/Value.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace shared::corelang {

class CompiledProgram;
struct ExecutionRequest;
struct ExecutionResult;

using ExecutionHostFunction = std::function<std::expected<core::lang::Value, std::string>(
    std::span<core::lang::Value const>)>;

struct ExecutionHostCall final {
    core::lang::CustomManifest manifest;
    ExecutionHostFunction invoke;
};

struct ExecutionRequest final {
    CompiledProgram const& program;
    std::string_view module_id;
    std::string_view entry;
    Backend backend;
    core::lang::Type expected_result_type;
    ExecutionLimits limits;
    uint64_t execution_count{1U};
    std::vector<ExecutionHostCall> host_calls;
    std::stop_token stop_token{};
    CancellationCheck cancellation_requested;
};

struct ExecutionResult final {
    core::lang::Value value;
    BackendReceipt receipt;
};

class CompiledProgram final {
public:
    core::lang::CompileResult compiled;

    [[nodiscard]]
    std::array<uint8_t, 32U> const& sourceSha256() const noexcept;

    [[nodiscard]]
    std::array<uint8_t, 32U> const& bytecodeSha256() const noexcept;

private:
    std::array<uint8_t, 32U> source_sha256_{};
    std::array<uint8_t, 32U> bytecode_sha256_{};
    uint64_t loop_multiplier_{1U};
    bool has_bounded_loops_{false};

    friend std::expected<CompiledProgram, BackendError> compileBounded(
        core::lang::Source,
        core::lang::CompilerRegistry const&,
        ExecutionLimits const&,
        std::stop_token const&,
        CancellationCheck const&
    );
    friend std::expected<uint64_t, BackendError> boundedInstructionCount(
        CompiledProgram const&,
        std::string_view,
        std::string_view,
        ExecutionLimits const&,
        uint64_t
    );
    friend std::expected<ExecutionResult, BackendError> executeBounded(ExecutionRequest request);
};

[[nodiscard]]
std::expected<CompiledProgram, BackendError> compileBounded(
    core::lang::Source source,
    core::lang::CompilerRegistry const& registry,
    ExecutionLimits const& limits,
    std::stop_token const& stop_token = {},
    CancellationCheck const& cancellation_requested = {}
);

[[nodiscard]]
std::expected<uint64_t, BackendError> boundedInstructionCount(
    core::lang::Artifact const& artifact,
    std::string_view module_id,
    std::string_view entry,
    ExecutionLimits const& limits,
    uint64_t execution_count
);

[[nodiscard]]
std::expected<uint64_t, BackendError> boundedInstructionCount(
    CompiledProgram const& program,
    std::string_view module_id,
    std::string_view entry,
    ExecutionLimits const& limits,
    uint64_t execution_count
);

[[nodiscard]]
std::expected<ExecutionResult, BackendError> executeBounded(ExecutionRequest request);

} // namespace shared::corelang
