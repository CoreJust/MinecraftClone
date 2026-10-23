#include <shared/corelang/Execution.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <core/lang/Bytecode.hpp>
#include <core/lang/Compiler.hpp>
#include <core/lang/Registry.hpp>
#include <core/lang/Value.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]]
core::lang::CompilerRegistry compilerRegistry()
{
    core::lang::Ruleset ruleset{
        .id = "backend-test",
        .version = 1U,
        .restrictions = {},
        .operations = {},
        .triggers = {},
        .resolve = {},
        .automaticImports = {},
    };
    return {
        .rulesets = {std::move(ruleset)},
        .defaults = {"backend-test"},
        .restrictions = {},
    };
}

[[nodiscard]]
shared::corelang::ExecutionLimits executionLimits()
{
    return {
        .max_source_bytes = uint64_t{4U} * 1'024U,
        .max_bytecode_bytes = uint64_t{64U} * 1'024U,
        .max_native_image_bytes = uint64_t{4U} * 1'024U * 1'024U,
        .max_instructions = 64U,
        .max_execution_count = 2U,
    };
}

TEST(CoreLangBackendTest, ExecutesTheRequestedBackendAndReportsItsReceipt)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() -> u8 {
    7u8
}
)core";
    static constexpr std::array<shared::corelang::Backend, 3U> BACKENDS{
        shared::corelang::Backend::Interpreter,
        shared::corelang::Backend::Jit,
        shared::corelang::Backend::Aot,
    };
    shared::corelang::ExecutionLimits const limits = executionLimits();
    core::lang::Type const result_type{
        .kind = core::lang::TypeKind::U8,
        .elements = {},
        .method_owner_name = {},
        .callable_precondition = {},
        .callable_postcondition = {},
        .struct_definition = {},
        .recursive_struct_definition = {},
    };
    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        compilerRegistry(),
        limits
    );

    ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    for (shared::corelang::Backend const backend : BACKENDS) {
        auto const executed = shared::corelang::executeBounded({
            .program = *compiled,
            .module_id = "backend-test",
            .entry = "main",
            .backend = backend,
            .expected_result_type = result_type,
            .limits = limits,
            .host_calls = {},
            .cancellation_requested = {},
        });

#if defined(__linux__) && !defined(__ANDROID__)
        if (backend != shared::corelang::Backend::Interpreter) {
            ASSERT_FALSE(executed.has_value());
            EXPECT_EQ(executed.error().code, shared::corelang::BackendErrorCode::BackendUnavailable);
            continue;
        }
#endif
        ASSERT_TRUE(executed.has_value()) << (executed ? "" : executed.error().message);
        EXPECT_EQ(executed->value.type, result_type);
        ASSERT_EQ(executed->value.bytes.size(), 1U);
        EXPECT_EQ(executed->value.bytes.front(), 7U);
        EXPECT_EQ(executed->receipt.requested_backend, backend);
        EXPECT_EQ(executed->receipt.actual_backend, backend);
        EXPECT_EQ(executed->receipt.source_sha256, compiled->sourceSha256());
        EXPECT_FALSE(executed->receipt.compiler_version.empty());
        EXPECT_FALSE(executed->receipt.target.empty());
        EXPECT_EQ(executed->receipt.native_image_sha256.has_value(),
                  backend == shared::corelang::Backend::Aot);
    }
}

TEST(CoreLangBackendTest, AotCopiesStringResultsOutOfTheNativeImage)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() -> str {
    "aot-result"
}
)core";
    shared::corelang::ExecutionLimits const limits = executionLimits();
    core::lang::Type const result_type{
        .kind = core::lang::TypeKind::Str,
        .elements = {},
        .method_owner_name = {},
        .callable_precondition = {},
        .callable_postcondition = {},
        .struct_definition = {},
        .recursive_struct_definition = {},
    };
    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        compilerRegistry(),
        limits
    );

    ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    auto const executed = shared::corelang::executeBounded({
        .program = *compiled,
        .module_id = "backend-test",
        .entry = "main",
        .backend = shared::corelang::Backend::Aot,
        .expected_result_type = result_type,
        .limits = limits,
        .host_calls = {},
        .cancellation_requested = {},
    });

#if defined(__linux__) && !defined(__ANDROID__)
    ASSERT_FALSE(executed.has_value());
    EXPECT_EQ(executed.error().code, shared::corelang::BackendErrorCode::BackendUnavailable);
#else
    ASSERT_TRUE(executed.has_value()) << (executed ? "" : executed.error().message);
    EXPECT_EQ((std::string{executed->value.bytes.begin(), executed->value.bytes.end()}), "aot-result");
#endif
}

TEST(CoreLangBackendTest, RejectsSourceBeforeCompilationWhenItExceedsTheBound)
{
    shared::corelang::ExecutionLimits limits = executionLimits();
    limits.max_source_bytes = 1U;

    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = "source"},
        compilerRegistry(),
        limits
    );

    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code, shared::corelang::BackendErrorCode::SourceTooLarge);
}

TEST(CoreLangBackendTest, RejectsCallerLimitsAboveThePublicFixedCeilings)
{
    shared::corelang::ExecutionLimits limits = executionLimits();
    limits.max_instructions = std::numeric_limits<uint64_t>::max();

    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = "source"},
        compilerRegistry(),
        limits
    );

    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code, shared::corelang::BackendErrorCode::InvalidConfiguration);
}

TEST(CoreLangBackendTest, CancellationPredicateIsObservedDuringCompilation)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() -> u8 {
    7u8
}
)core";
    shared::corelang::ExecutionLimits const limits = executionLimits();

    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        compilerRegistry(),
        limits,
        {},
        [] { return true; }
    );

    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code, shared::corelang::BackendErrorCode::Cancelled);
}

TEST(CoreLangBackendTest, CompileFailurePreservesTheCoreLangSourceOffset)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() -> u8 {
    missing
}
)core";
    shared::corelang::ExecutionLimits const limits = executionLimits();

    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        compilerRegistry(),
        limits
    );

    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code, shared::corelang::BackendErrorCode::CompileFailed);
    ASSERT_TRUE(compiled.error().source_offset.has_value());
    EXPECT_EQ(*compiled.error().source_offset, SOURCE.find("missing"));
}

TEST(CoreLangBackendTest, ExecutesFiniteSignedForLoopsOnEveryRequestedBackend)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() -> u8 {
    let mut total = 0u8
    for index in 0i32..2i32 {
        total = total + 1u8
    }
    total
}
)core";
    static constexpr std::array<shared::corelang::Backend, 3U> BACKENDS{
        shared::corelang::Backend::Interpreter,
        shared::corelang::Backend::Jit,
        shared::corelang::Backend::Aot,
    };
    shared::corelang::ExecutionLimits limits = executionLimits();
    limits.max_instructions = 1'024U;
    core::lang::Type const result_type{
        .kind = core::lang::TypeKind::U8,
        .elements = {},
        .method_owner_name = {},
        .callable_precondition = {},
        .callable_postcondition = {},
        .struct_definition = {},
        .recursive_struct_definition = {},
    };
    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        compilerRegistry(),
        limits
    );

    ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    for (shared::corelang::Backend const backend : BACKENDS) {
        auto const executed = shared::corelang::executeBounded({
            .program = *compiled,
            .module_id = "backend-test",
            .entry = "main",
            .backend = backend,
            .expected_result_type = result_type,
            .limits = limits,
            .host_calls = {},
            .cancellation_requested = {},
        });

#if defined(__linux__) && !defined(__ANDROID__)
        if (backend != shared::corelang::Backend::Interpreter) {
            ASSERT_FALSE(executed.has_value());
            EXPECT_EQ(executed.error().code, shared::corelang::BackendErrorCode::BackendUnavailable);
            continue;
        }
#endif
        ASSERT_TRUE(executed.has_value()) << (executed ? "" : executed.error().message);
        ASSERT_EQ(executed->value.bytes.size(), 1U);
        EXPECT_EQ(executed->value.bytes.front(), 2U);
        EXPECT_EQ(executed->receipt.requested_backend, backend);
        EXPECT_EQ(executed->receipt.actual_backend, backend);
    }
}

TEST(CoreLangBackendTest, RejectsAForLoopThatResetsItsCounter)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() {
    for index in 0i32..2i32 {
        index = 0i32
    }
}
)core";

    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        compilerRegistry(),
        executionLimits()
    );

    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code, shared::corelang::BackendErrorCode::UnboundedExecution);
}

TEST(CoreLangBackendTest, RejectsAnAddressEscapeOfTheForLoopIndex)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() {
    for index in 0i32..2i32 {
        let alias = &index
    }
}
)core";

    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        compilerRegistry(),
        executionLimits()
    );

    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code, shared::corelang::BackendErrorCode::UnboundedExecution);
}

TEST(CoreLangBackendTest, RejectsDynamicWhileLoopsBeforeCompilation)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() {
    while true { }
}
)core";

    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        compilerRegistry(),
        executionLimits()
    );

    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code, shared::corelang::BackendErrorCode::UnboundedExecution);
}

TEST(CoreLangBackendTest, CompiledProofRejectsArtifactMutation)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() -> u8 {
    7u8
}
)core";
    shared::corelang::ExecutionLimits const limits = executionLimits();
    auto compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        compilerRegistry(),
        limits
    );
    ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    compiled->compiled.artifact.modules.front().id = "mutated";

    auto const cost = shared::corelang::boundedInstructionCount(
        *compiled,
        "backend-test",
        "main",
        limits,
        1U
    );

    ASSERT_FALSE(cost.has_value());
    EXPECT_EQ(cost.error().code, shared::corelang::BackendErrorCode::InvalidArtifact);
}

TEST(CoreLangBackendTest, RejectsCyclicBytecodeBeforeExecution)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() -> u8 {
    7u8
}
)core";
    shared::corelang::ExecutionLimits const limits = executionLimits();
    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        compilerRegistry(),
        limits
    );
    ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);

    core::lang::Artifact artifact = compiled->compiled.artifact;
    auto const module = std::ranges::find(artifact.modules, "backend-test", &core::lang::Module::id);
    ASSERT_NE(module, artifact.modules.end());
    auto const exported = std::ranges::find(module->exports, "main", &core::lang::Export::name);
    ASSERT_NE(exported, module->exports.end());
    ASSERT_LT(exported->target, artifact.functions.size());
    core::lang::Function& function = artifact.functions[exported->target];
    ASSERT_FALSE(function.blocks.empty());
    ASSERT_FALSE(function.blocks.front().instructions.empty());
    function.blocks.front().instructions.back() = {
        .opcode = core::lang::Op::Branch,
        .operands = {0U, 0U},
    };

    auto const cost = shared::corelang::boundedInstructionCount(
        artifact,
        "backend-test",
        "main",
        limits,
        1U
    );

    ASSERT_FALSE(cost.has_value());
    EXPECT_EQ(cost.error().code, shared::corelang::BackendErrorCode::UnboundedExecution);
}

TEST(CoreLangBackendTest, AotHostCallsUseTheSameTypedManifestAndHandler)
{
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
pub fn main() -> u8 {
    let echoed = echo("bounded")
    1u8
}
)core";
    core::lang::Type const string_type{
        .kind = core::lang::TypeKind::Str,
        .elements = {},
        .method_owner_name = {},
        .callable_precondition = {},
        .callable_postcondition = {},
        .struct_definition = {},
        .recursive_struct_definition = {},
    };
    core::lang::Type const result_type{
        .kind = core::lang::TypeKind::U8,
        .elements = {},
        .method_owner_name = {},
        .callable_precondition = {},
        .callable_postcondition = {},
        .struct_definition = {},
        .recursive_struct_definition = {},
    };
    core::lang::CustomManifest manifest{
        .provider_key = "backend-test.echo",
        .abi_major = 1U,
        .abi_minor = 0U,
        .effect = core::lang::CustomEffect::Observable,
        .may_suspend = false,
        .may_reenter = false,
        .unsafe_ = false,
        .arguments = {string_type},
        .result = string_type,
        .borrow = {.parameters = {core::lang::BorrowAccess::None}, .returns = {}},
        .clobber_regions = {},
    };
    manifest.digest = core::lang::customManifestDigest(manifest);
    core::lang::Ruleset ruleset{
        .id = "backend-test",
        .version = 1U,
        .restrictions = {},
        .operations = {
            {
                .name = "echo",
                .kind = core::lang::ExtensionKind::Builtin,
                .arguments = {string_type},
                .result = string_type,
                .effect = core::lang::CustomEffect::Observable,
                .custom = manifest,
                .expand = {},
                .compile_time_effect = std::nullopt,
                .compile_time = {},
                .borrow = manifest.borrow,
            },
        },
        .triggers = {},
        .resolve = {},
        .automaticImports = {},
    };
    core::lang::CompilerRegistry const registry{
        .rulesets = {std::move(ruleset)},
        .defaults = {"backend-test"},
        .restrictions = {},
    };
    shared::corelang::ExecutionLimits const limits = executionLimits();
    auto const compiled = shared::corelang::compileBounded(
        {.id = "backend-test", .text = std::string{SOURCE}},
        registry,
        limits
    );
    ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    ASSERT_EQ(compiled->compiled.artifact.customs.size(), 1U);
    EXPECT_EQ(compiled->compiled.artifact.customs.front().provider_key, manifest.provider_key);
    EXPECT_EQ(compiled->compiled.artifact.customs.front().clobber_regions, (std::vector<uint64_t>{0U}));
    EXPECT_EQ(compiled->compiled.artifact.customs.front().digest,
              core::lang::customManifestDigest(compiled->compiled.artifact.customs.front()));
    shared::corelang::ExecutionHostCall host_call{
        .manifest = std::move(manifest),
        .invoke = [](std::span<core::lang::Value const> arguments)
            -> std::expected<core::lang::Value, std::string> {
            if (arguments.size() != 1U || arguments.front().type.kind != core::lang::TypeKind::Str ||
                std::string{arguments.front().bytes.begin(), arguments.front().bytes.end()} != "bounded") {
                return std::unexpected("host call received malformed string data");
            }
            return core::lang::Value{
                .type = {
                    .kind = core::lang::TypeKind::Str,
                    .elements = {},
                    .method_owner_name = {},
                    .callable_precondition = {},
                    .callable_postcondition = {},
                    .struct_definition = {},
                    .recursive_struct_definition = {},
                },
                .bytes = {'r', 'e', 't', 'u', 'r', 'n', 'e', 'd'},
                .elements = {},
                .literal_lease = {},
                .runtime_lease = {},
            };
        },
    };

    auto const executed = shared::corelang::executeBounded({
        .program = *compiled,
        .module_id = "backend-test",
        .entry = "main",
        .backend = shared::corelang::Backend::Aot,
        .expected_result_type = result_type,
        .limits = limits,
        .host_calls = {std::move(host_call)},
        .cancellation_requested = {},
    });

#if defined(__linux__) && !defined(__ANDROID__)
    ASSERT_FALSE(executed.has_value());
    EXPECT_EQ(executed.error().code, shared::corelang::BackendErrorCode::BackendUnavailable);
#else
    ASSERT_TRUE(executed.has_value()) << (executed ? "" : executed.error().message);
    EXPECT_EQ(executed->value.type, result_type);
    ASSERT_EQ(executed->value.bytes.size(), 1U);
    EXPECT_EQ(executed->value.bytes.front(), 1U);
    EXPECT_EQ(executed->receipt.actual_backend, shared::corelang::Backend::Aot);
#endif
}

} // namespace
