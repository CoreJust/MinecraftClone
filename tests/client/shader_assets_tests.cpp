#include <client/render/InstalledShaderAssets.hpp>

#include <core/common/Defer.hpp>
#include <core/kernel/Spirv.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string_view>

TEST(ShaderAssetsTest, LoadsEveryCompiledShaderOutsideTheBuildDirectory)
{
    static constexpr uint32_t SPIRV_MAGIC = 0x0723'0203;
    static constexpr std::array<std::string_view, 3> SHADERS{
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
