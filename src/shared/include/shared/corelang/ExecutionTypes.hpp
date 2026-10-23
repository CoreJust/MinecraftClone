#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace shared::corelang {

enum class Backend : uint8_t {
    Interpreter,
    Jit,
    Aot,
};

enum class BackendErrorCode : uint8_t {
    Cancelled,
    InvalidConfiguration,
    SourceTooLarge,
    BytecodeTooLarge,
    NativeImageTooLarge,
    CompileFailed,
    InvalidArtifact,
    InvalidHostCalls,
    InvalidManifest,
    UnboundedExecution,
    InstructionLimitExceeded,
    BackendUnavailable,
    InvalidResultType,
    ExecutionFailed,
};

struct BackendError final {
    BackendErrorCode code;
    std::string message;
    std::optional<uint64_t> source_offset{};
};

struct ExecutionLimits final {
    uint64_t max_source_bytes{uint64_t{4U} * 1'024U};
    uint64_t max_bytecode_bytes{uint64_t{64U} * 1'024U};
    uint64_t max_native_image_bytes{uint64_t{4U} * 1'024U * 1'024U};
    uint64_t max_instructions{65'536U};
    uint64_t max_execution_count{2U};
};

struct BackendReceipt final {
    Backend requested_backend;
    Backend actual_backend;
    std::array<uint8_t, 32U> source_sha256{};
    std::array<uint8_t, 32U> bytecode_sha256{};
    std::optional<std::array<uint8_t, 32U>> native_image_sha256;
    std::string compiler_version;
    std::string target;
};

using CancellationCheck = std::function<bool()>;

} // namespace shared::corelang
