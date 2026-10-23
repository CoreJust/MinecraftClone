#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/TerrainTextures.hpp>

#include <core/common/Defer.hpp>
#include <core/kernel/Program.hpp>
#include <core/kernel/Spirv.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] core::kernel::ProgramStage programStage(
    std::shared_ptr<core::kernel::SpirvModule const> module,
    std::string_view const entrypoint = "main"
)
{
    return {
        .module = std::move(module),
        .entrypoint = std::string{ entrypoint },
        .required_bindings = {},
    };
}

[[nodiscard]] core::kernel::GraphicsProgram graphicsProgram(
    std::shared_ptr<core::kernel::SpirvModule const> vertex,
    std::shared_ptr<core::kernel::SpirvModule const> fragment,
    std::string_view const vertex_entrypoint = "main",
    std::string_view const fragment_entrypoint = "main"
)
{
    return core::kernel::GraphicsProgram::create(
        programStage(std::move(vertex), vertex_entrypoint),
        programStage(std::move(fragment), fragment_entrypoint)
    );
}

[[nodiscard]] std::string graphicsProgramDiagnostic(
    core::kernel::ProgramStage vertex,
    core::kernel::ProgramStage fragment
)
{
    try {
        static_cast<void>(core::kernel::GraphicsProgram::create(std::move(vertex), std::move(fragment)));
    } catch (core::kernel::SpirvError const& error) {
        return error.what();
    }
    ADD_FAILURE() << "Graphics program creation unexpectedly succeeded";
    return {};
}

} // namespace

TEST(ShaderAssetsTest, LoadsEveryCompiledShaderOutsideTheBuildDirectory)
{
    static constexpr uint32_t SPIRV_MAGIC = 0x0723'0203;
    static constexpr std::array<std::string_view, 8> SHADERS{
        "text.frag.spv",
        "text.vert.spv",
        "grid.vert.spv",
        "player.vert.spv",
        "player.frag.spv",
        "map.vert.spv",
        "map.frag.spv",
        "trivial.frag.spv",
    };
    std::filesystem::path const previous_directory = std::filesystem::current_path();
    defer { std::filesystem::current_path(previous_directory); };
    std::filesystem::current_path(std::filesystem::temp_directory_path());
    client::InstalledShaderAssets const assets;

    for (std::string_view const shader : SHADERS) {
        core::kernel::SpirvModule const spirv = assets.load(shader);
        ASSERT_FALSE(spirv.words().empty()) << shader;
        EXPECT_EQ(spirv.words().front(), SPIRV_MAGIC) << shader;
    }
}

TEST(ShaderAssetsTest, ReportsMissingAssetsAndRejectsPaths)
{
    client::InstalledShaderAssets const assets;

    EXPECT_THROW(static_cast<void>(assets.load("missing.spv")), std::runtime_error);
    EXPECT_THROW(static_cast<void>(assets.load("../grid.vert.spv")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(assets.load("grid.vert")), std::invalid_argument);
}

TEST(ShaderAssetsTest, CreatesStableDistinctGraphicsProgramsFromInstalledShaders)
{
    client::InstalledShaderAssets const assets;
    auto const grid = std::make_shared<core::kernel::SpirvModule const>(assets.load("grid.vert.spv"));
    auto const player = std::make_shared<core::kernel::SpirvModule const>(assets.load("player.vert.spv"));
    auto const text_vertex = std::make_shared<core::kernel::SpirvModule const>(
        assets.load("text.vert.spv")
    );
    auto const player_fragment = std::make_shared<core::kernel::SpirvModule const>(assets.load("player.frag.spv"));
    auto const fragment = std::make_shared<core::kernel::SpirvModule const>(assets.load("trivial.frag.spv"));
    auto const text_fragment = std::make_shared<core::kernel::SpirvModule const>(
        assets.load("text.frag.spv")
    );

    ASSERT_TRUE(grid->hasEntrypoint(core::kernel::ShaderStage::Vertex, "main"));
    ASSERT_TRUE(player->hasEntrypoint(core::kernel::ShaderStage::Vertex, "main"));
    ASSERT_TRUE(fragment->hasEntrypoint(core::kernel::ShaderStage::Fragment, "main"));
    ASSERT_TRUE(text_vertex->hasEntrypoint(core::kernel::ShaderStage::Vertex, "main"));
    ASSERT_TRUE(text_fragment->hasEntrypoint(core::kernel::ShaderStage::Fragment, "main"));
    EXPECT_TRUE(grid->bindings().empty());
    EXPECT_TRUE(player->bindings().empty());
    EXPECT_TRUE(fragment->bindings().empty());
    ASSERT_EQ(text_vertex->bindings().size(), 1U);
    EXPECT_TRUE(text_fragment->bindings().empty());

    core::kernel::GraphicsProgram const grid_program = graphicsProgram(grid, fragment);
    core::kernel::GraphicsProgram const repeated_grid_program = graphicsProgram(grid, fragment);
    core::kernel::GraphicsProgram const player_program = graphicsProgram(player, player_fragment);
    core::kernel::GraphicsProgram const text_program = core::kernel::GraphicsProgram::create(
        { .module = text_vertex, .entrypoint = "main", .required_bindings = { { .set = 0U, .binding = 0U } } },
        { .module = text_fragment, .entrypoint = "main", .required_bindings = {} }
    );

    EXPECT_EQ(grid_program.fingerprint(), repeated_grid_program.fingerprint());
    EXPECT_NE(grid_program.fingerprint(), player_program.fingerprint());
    EXPECT_NE(grid_program.fingerprint(), text_program.fingerprint());
    EXPECT_EQ(grid_program.vertex().entrypoint, "main");
    EXPECT_EQ(grid_program.fragment().entrypoint, "main");
    EXPECT_TRUE(grid_program.vertex().required_bindings.empty());
    EXPECT_TRUE(grid_program.fragment().required_bindings.empty());
}

TEST(ShaderAssetsTest, MapImageShaderInterfaceRequiresOneCombinedSampler)
{
    client::InstalledShaderAssets const assets;
    auto const vertex = std::make_shared<core::kernel::SpirvModule const>(assets.load("map.vert.spv"));
    auto const fragment = std::make_shared<core::kernel::SpirvModule const>(assets.load("map.frag.spv"));

    ASSERT_TRUE(vertex->bindings().empty());
    ASSERT_EQ(fragment->bindings().size(), 1U);
    EXPECT_EQ(fragment->bindings()[0].set, 0U);
    EXPECT_EQ(fragment->bindings()[0].binding, 0U);

    core::kernel::GraphicsProgram const program = core::kernel::GraphicsProgram::create(
        { .module = vertex, .entrypoint = "main", .required_bindings = {} },
        { .module = fragment, .entrypoint = "main", .required_bindings = { { .set = 0U, .binding = 0U } } }
    );
    EXPECT_EQ(program.vertex().entrypoint, "main");
    ASSERT_EQ(program.fragment().required_bindings.size(), 1U);
    EXPECT_EQ(program.fragment().required_bindings[0].binding, 0U);
}

TEST(ShaderAssetsTest, StoneMaterialInterfaceLinksBothVertexPathsToBothFragments)
{
    client::InstalledShaderAssets const assets;
    std::array<std::string_view, 2> const vertex_names{
        "stone.vert.spv",
        "stone_indexed_block.vert.spv",
    };
    std::array<std::string_view, 2> const fragment_names{
        "stone.frag.spv",
        "stone_solid.frag.spv",
    };
    for (std::string_view const vertex_name : vertex_names) {
        auto const vertex = std::make_shared<core::kernel::SpirvModule const>(assets.load(vertex_name));
        for (std::string_view const fragment_name : fragment_names) {
            auto const fragment = std::make_shared<core::kernel::SpirvModule const>(assets.load(fragment_name));
            std::vector<core::kernel::DescriptorBinding> fragment_bindings;
            fragment_bindings.push_back({ .set = 0U, .binding = 1U });
            fragment_bindings.push_back({ .set = 0U, .binding = 2U });
            ASSERT_EQ(fragment->bindings().size(), 2U) << fragment_name;
            EXPECT_EQ(fragment->bindings()[1].binding, 2U) << fragment_name;
            core::kernel::GraphicsProgram const program = core::kernel::GraphicsProgram::create(
                {
                    .module = vertex,
                    .entrypoint = "main",
                    .required_bindings = { { .set = 0U, .binding = 0U } },
                },
                {
                    .module = fragment,
                    .entrypoint = "main",
                    .required_bindings = std::move(fragment_bindings),
                }
            );
            EXPECT_NE(program.fingerprint(), 0U) << vertex_name << " + " << fragment_name;
        }
    }
}

TEST(ShaderAssetsTest, TerrainTextureAtlasPreservesGrassFaceMappingAndOverlayAlpha)
{
    namespace terrain_textures = client::render::terrain_textures;

    EXPECT_EQ(terrain_textures::tileIndex(2U, 4U), 16U);
    EXPECT_EQ(terrain_textures::tileIndex(2U, 5U), 17U);
    EXPECT_EQ(terrain_textures::GRASS_SIDE_OVERLAY_TILE_INDEX, 42U);
    EXPECT_EQ(terrain_textures::GRASS_TINT_RGBA8, 0xFF59'BD91U);
    EXPECT_EQ(terrain_textures::tileIndex(terrain_textures::MATERIAL_COUNT, 0U),
        terrain_textures::INVALID_TILE_INDEX);
    EXPECT_EQ(terrain_textures::tileIndex(2U, terrain_textures::FACE_COUNT),
        terrain_textures::INVALID_TILE_INDEX);

    EXPECT_EQ(terrain_textures::RGBA8[0U], 0U);
    uint32_t const overlay_first_pixel = terrain_textures::RGBA8[
        static_cast<uint64_t>(terrain_textures::GRASS_SIDE_OVERLAY_TILE_INDEX)
            * terrain_textures::PIXELS_PER_TILE
    ];
    uint32_t const overlay_transparent_pixel = terrain_textures::RGBA8[
        terrain_textures::GRASS_SIDE_OVERLAY_TILE_INDEX * terrain_textures::PIXELS_PER_TILE + 21U
    ];
    EXPECT_EQ(static_cast<uint8_t>(overlay_first_pixel >> 24U), 255U);
    EXPECT_EQ(static_cast<uint8_t>(overlay_transparent_pixel >> 24U), 0U);
}

TEST(ShaderAssetsTest, PropagatesDeterministicKernelProgramDiagnostics)
{
    static constexpr std::array<uint8_t, 5> MALFORMED_BYTES{};
    client::InstalledShaderAssets const assets;
    auto const grid = std::make_shared<core::kernel::SpirvModule const>(assets.load("grid.vert.spv"));
    auto const fragment = std::make_shared<core::kernel::SpirvModule const>(assets.load("trivial.frag.spv"));

    auto const malformedDiagnostic = [] {
        try {
            static_cast<void>(core::kernel::SpirvModule::fromBytes(MALFORMED_BYTES));
        } catch (core::kernel::SpirvError const& error) {
            return std::string{ error.what() };
        }
        ADD_FAILURE() << "Malformed SPIR-V module unexpectedly loaded";
        return std::string{};
    };
    EXPECT_EQ(malformedDiagnostic(), "SPIR-V byte size must be a multiple of four");
    EXPECT_EQ(malformedDiagnostic(), "SPIR-V byte size must be a multiple of four");

    EXPECT_EQ(
        graphicsProgramDiagnostic(programStage(fragment), programStage(fragment)),
        "Program stage entry point is missing from its SPIR-V module"
    );
    EXPECT_EQ(
        graphicsProgramDiagnostic(programStage(grid, "missing"), programStage(fragment)),
        "Program stage entry point is missing from its SPIR-V module"
    );
}
