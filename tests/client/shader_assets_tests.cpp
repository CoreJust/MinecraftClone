#include <client/render/InstalledShaderAssets.hpp>

#include <core/common/Defer.hpp>
#include <core/kernel/Program.hpp>
#include <core/kernel/Spirv.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

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
    static constexpr std::array<std::string_view, 5> SHADERS{
        "debug_hud.frag.spv",
        "debug_hud.vert.spv",
        "grid.vert.spv",
        "player.vert.spv",
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
    auto const debug_hud_vertex = std::make_shared<core::kernel::SpirvModule const>(
        assets.load("debug_hud.vert.spv")
    );
    auto const fragment = std::make_shared<core::kernel::SpirvModule const>(assets.load("trivial.frag.spv"));
    auto const debug_hud_fragment = std::make_shared<core::kernel::SpirvModule const>(
        assets.load("debug_hud.frag.spv")
    );

    ASSERT_TRUE(grid->hasEntrypoint(core::kernel::ShaderStage::Vertex, "main"));
    ASSERT_TRUE(player->hasEntrypoint(core::kernel::ShaderStage::Vertex, "main"));
    ASSERT_TRUE(fragment->hasEntrypoint(core::kernel::ShaderStage::Fragment, "main"));
    ASSERT_TRUE(debug_hud_vertex->hasEntrypoint(core::kernel::ShaderStage::Vertex, "main"));
    ASSERT_TRUE(debug_hud_fragment->hasEntrypoint(core::kernel::ShaderStage::Fragment, "main"));
    EXPECT_TRUE(grid->bindings().empty());
    EXPECT_TRUE(player->bindings().empty());
    EXPECT_TRUE(fragment->bindings().empty());
    EXPECT_TRUE(debug_hud_vertex->bindings().empty());
    EXPECT_TRUE(debug_hud_fragment->bindings().empty());

    core::kernel::GraphicsProgram const grid_program = graphicsProgram(grid, fragment);
    core::kernel::GraphicsProgram const repeated_grid_program = graphicsProgram(grid, fragment);
    core::kernel::GraphicsProgram const player_program = graphicsProgram(player, fragment);
    core::kernel::GraphicsProgram const debug_hud_program = graphicsProgram(
        debug_hud_vertex,
        debug_hud_fragment
    );

    EXPECT_EQ(grid_program.fingerprint(), repeated_grid_program.fingerprint());
    EXPECT_NE(grid_program.fingerprint(), player_program.fingerprint());
    EXPECT_NE(grid_program.fingerprint(), debug_hud_program.fingerprint());
    EXPECT_EQ(grid_program.vertex().entrypoint, "main");
    EXPECT_EQ(grid_program.fragment().entrypoint, "main");
    EXPECT_TRUE(grid_program.vertex().required_bindings.empty());
    EXPECT_TRUE(grid_program.fragment().required_bindings.empty());
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
        "Graphics program stage entry point is missing from its SPIR-V module"
    );
    EXPECT_EQ(
        graphicsProgramDiagnostic(programStage(grid, "missing"), programStage(fragment)),
        "Graphics program stage entry point is missing from its SPIR-V module"
    );
}
