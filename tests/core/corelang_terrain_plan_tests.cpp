#include <shared/corelang/Backend.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

TEST(CoreLangTerrainPlanTest, BackendsReturnTheSameFrozenConfigurationAndPlan)
{
    static constexpr std::array<shared::corelang::Backend, 3U> BACKENDS{
        shared::corelang::Backend::Interpreter,
        shared::corelang::Backend::Jit,
        shared::corelang::Backend::Aot,
    };
    generation::GenerationConfiguration const configuration{
        .seed = 42U,
        .resolution = 256U,
    };
    std::string const expected_identity = generation::configurationIdentity(configuration);
    std::optional<shared::corelang::BackendReceipt> first_receipt;

    for (shared::corelang::Backend const backend : BACKENDS) {
        auto const prepared = shared::corelang::prepareTerrainPlan(configuration, backend);

#if defined(__linux__) && !defined(__ANDROID__)
        if (backend != shared::corelang::Backend::Interpreter) {
            ASSERT_FALSE(prepared.has_value());
            EXPECT_EQ(prepared.error().code, shared::corelang::BackendErrorCode::BackendUnavailable);
            continue;
        }
#endif
        ASSERT_TRUE(prepared.has_value()) << (prepared ? "" : prepared.error().message);
        EXPECT_EQ(prepared->configuration, configuration);
        EXPECT_EQ(generation::configurationIdentity(prepared->configuration), expected_identity);
        EXPECT_TRUE(prepared->world_generation_plan.pre_generate);
        EXPECT_EQ(prepared->world_generation_plan.refinement_extents,
                  (std::vector<uint32_t>{4096U, 1024U, 256U, 64U}));
        ASSERT_EQ(prepared->world_generation_plan.chunk_stages.size(), 1U);
        EXPECT_EQ(prepared->world_generation_plan.chunk_stages.front(),
                  shared::GenerationStage::Materialize);
        EXPECT_EQ(prepared->receipt.requested_backend, backend);
        EXPECT_EQ(prepared->receipt.actual_backend, backend);
        EXPECT_FALSE(prepared->receipt.compiler_version.empty());
        EXPECT_FALSE(prepared->receipt.target.empty());
        if (first_receipt) {
            EXPECT_EQ(prepared->receipt.source_sha256, first_receipt->source_sha256);
            EXPECT_EQ(prepared->receipt.bytecode_sha256, first_receipt->bytecode_sha256);
        } else {
            first_receipt = prepared->receipt;
        }
        if (backend == shared::corelang::Backend::Aot) {
            EXPECT_TRUE(prepared->receipt.native_image_sha256.has_value());
        } else {
            EXPECT_FALSE(prepared->receipt.native_image_sha256.has_value());
        }
    }
}

TEST(CoreLangTerrainPlanTest, RejectsManifestConfigurationThatDoesNotMatchTheFrozenRequest)
{
    generation::GenerationConfiguration const configuration{
        .seed = 42U,
        .resolution = 256U,
    };
    std::string source{shared::corelang::terrainPlanSource()};
    std::string const resolution_field = "terrain_resolution(),";
    auto const resolution_position = source.find(resolution_field);
    ASSERT_NE(resolution_position, std::string::npos);
    uint32_t const resolution_offset = static_cast<uint32_t>(resolution_position);
    source.replace(static_cast<std::string::size_type>(resolution_offset),
                   resolution_field.size(),
                   "terrain_resolution() + 1u32,");

    auto const prepared = shared::corelang::prepareTerrainPlan(
        configuration, shared::corelang::Backend::Interpreter, source);

    ASSERT_FALSE(prepared.has_value());
    EXPECT_EQ(prepared.error().code, shared::corelang::BackendErrorCode::InvalidManifest);
}

TEST(CoreLangTerrainPlanTest, RejectsARefinementExtentOutsideTheRegisteredHierarchy)
{
    generation::GenerationConfiguration const configuration{
        .seed = 42U,
        .resolution = 256U,
    };
    std::string source{shared::corelang::terrainPlanSource()};
    std::string const extent = "4096u32";
    auto const extent_position = source.find(extent);
    ASSERT_NE(extent_position, std::string::npos);
    source.replace(extent_position, extent.size(), "2048u32");

    auto const prepared = shared::corelang::prepareTerrainPlan(
        configuration, shared::corelang::Backend::Interpreter, source);

    ASSERT_FALSE(prepared.has_value());
    EXPECT_EQ(prepared.error().code, shared::corelang::BackendErrorCode::InvalidManifest);
}

TEST(CoreLangTerrainPlanTest, RejectsAnInvalidFrozenConfigurationBeforeCompilation)
{
    generation::GenerationConfiguration const configuration{
        .seed = 42U,
        .resolution = 1U,
    };

    auto const prepared =
        shared::corelang::prepareTerrainPlan(configuration, shared::corelang::Backend::Interpreter);

    ASSERT_FALSE(prepared.has_value());
    EXPECT_EQ(prepared.error().code, shared::corelang::BackendErrorCode::InvalidConfiguration);
}

TEST(CoreLangTerrainPlanTest, EnforcesTheSourceByteBound)
{
    static constexpr uint32_t SOURCE_LIMIT_PLUS_ONE = 4U * 1'024U + 1U;
    generation::GenerationConfiguration const configuration{
        .seed = 42U,
        .resolution = 256U,
    };
    std::string source(SOURCE_LIMIT_PLUS_ONE, 'x');

    auto const prepared = shared::corelang::prepareTerrainPlan(
        configuration, shared::corelang::Backend::Interpreter, source);

    ASSERT_FALSE(prepared.has_value());
    EXPECT_EQ(prepared.error().code, shared::corelang::BackendErrorCode::SourceTooLarge);
}

TEST(CoreLangTerrainPlanTest, RejectsAnExplicitlyEmptyManifest)
{
    generation::GenerationConfiguration const configuration{
        .seed = 42U,
        .resolution = 256U,
    };

    auto const prepared = shared::corelang::prepareTerrainPlan(
        configuration, shared::corelang::Backend::Interpreter, std::string_view{});

    ASSERT_FALSE(prepared.has_value());
    EXPECT_EQ(prepared.error().code, shared::corelang::BackendErrorCode::SourceTooLarge);
}

TEST(CoreLangTerrainPlanTest, ObservesCancellationBeforeCompiling)
{
    generation::GenerationConfiguration const configuration{
        .seed = 42U,
        .resolution = 256U,
    };
    std::stop_source cancellation;
    cancellation.request_stop();

    auto const prepared = shared::corelang::prepareTerrainPlan(
        configuration, shared::corelang::Backend::Interpreter, cancellation.get_token());

    ASSERT_FALSE(prepared.has_value());
    EXPECT_EQ(prepared.error().code, shared::corelang::BackendErrorCode::Cancelled);
}

} // namespace
