#include <client/TerrainLodPresentation.hpp>
#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <gtest/gtest.h>
#include <testsupport/ImageComparison.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void setSceneFaces(client::TerrainLodScene& scene, std::vector<client::StoneFaceInstance> faces)
{
    scene.logical_face_count = faces.size();
    if (faces.empty()) {
        return;
    }
    client::TerrainLodDrawGroup group{
        .range = {0U, static_cast<uint32_t>(faces.size())}, .extent = 256U,
    };
    group.face_storage = std::make_shared<std::vector<client::StoneFaceInstance> const>(std::move(faces));
    scene.groups.push_back(std::move(group));
}

} // namespace

TEST(CoarseClassifierDiagnostic, ConstantDryCutoffPlaneNeverBecomesDirt)
{
    client::InstalledShaderAssets const shaders;
    client::VulkanOffscreenRenderer renderer{shaders, false};
    renderer.setDebugHudEnabled(false);
    RecordProperty("validation_enabled", renderer.validationEnabled() ? "true" : "false");
    for (int32_t highest : {384, 400}) {
        auto scene = std::make_shared<client::TerrainLodScene>();
        scene->block_sea_level = 384U;
        int32_t const h = (highest + 1) * 32;
        std::array<std::array<int32_t, 3U>, 4U> const corners{{{0,0,h},{256,0,h},{256,256,h},{0,256,h}}};
        std::array<int32_t,3U> const center{128,128,h};
        std::vector<client::StoneFaceInstance> faces;
        for (uint32_t edge=0;edge<4U;++edge) {
            faces.push_back(client::StoneFaceInstance::fromTerrainSurfaceTriangle(
                {center,corners[edge],corners[(edge+1U)%4U]},{384,384,384}));
        }
        setSceneFaces(*scene, std::move(faces));
        renderer.setTerrainLodScene(scene);
        for (double offset : {3.0,3.1,4.75}) {
            renderer.setCamera({.position={128.0,-8.0,highest+1.0+offset},.angles={.pitch_degrees=-12.0}});
            auto const image=renderer.render({},std::chrono::steady_clock::now()+std::chrono::seconds{10});
            ASSERT_GT(image.width,0U);ASSERT_GT(image.height,0U);
            uint32_t dirt=0,grass=0;
            for (size_t i=0;i<image.rgba8.size();i+=4U) {
                auto r=static_cast<uint32_t>(image.rgba8[i]),
                    g=static_cast<uint32_t>(image.rgba8[i+1U]),
                    b=static_cast<uint32_t>(image.rgba8[i+2U]);
                if (r>g+4U && r>b+4U) ++dirt;
                if (g>r+4U && g>b+4U) ++grass;
            }
            auto const name="h"+std::to_string(highest)+"_offset"+std::to_string(static_cast<uint32_t>(offset*1000.0));
            RecordProperty(name+"_dirt",static_cast<int32_t>(dirt));RecordProperty(name+"_grass",static_cast<int32_t>(grass));
            EXPECT_GT(grass,100U)<<name;
            EXPECT_EQ(dirt,0U)<<name<<" one constant dry surface has no Dirt and no overlapping geometry";
            if (auto const directory=std::getenv("MC_TERRAIN_CLASSIFIER_CAPTURE_DIRECTORY")) {
                auto written=testsupport::writePpm(
                    {.width=image.width,.height=image.height,.srgb_encoded=image.srgb_encoded,.pixels=image.rgba8},
                    std::filesystem::path{directory}/(name+".ppm"),
                    "Single constant coarse surface, highest384 cutoff vs400 control");
                ASSERT_TRUE(written);
            }
        }
    }
}

TEST(CoarseClassifierDiagnostic, CenteredCutoffsMatchExplicitMaterialsOnBothSides)
{
    client::InstalledShaderAssets const shaders;
    client::VulkanOffscreenRenderer renderer{shaders, false};
    renderer.setDebugHudEnabled(false);
    struct Case { int32_t highest; int32_t temperature; shared::Block material; bool slope; };
    std::array const cases{
        Case{384*32-1,384,shared::Block::Dirt,false},
        Case{384*32,384,shared::Block::Grass,false},
        Case{384*32+1,384,shared::Block::Grass,false},
        Case{380*32-1,384,shared::Block::Stone,false},
        Case{380*32,384,shared::Block::Dirt,false},
        Case{380*32+1,384,shared::Block::Dirt,false},
        Case{384*32,-65,shared::Block::Snow,false},
        Case{384*32,-64,shared::Block::Snow,false},
        Case{384*32,-63,shared::Block::Grass,false},
        Case{382*32,-64,shared::Block::Ice,false},
        Case{383*32,384,shared::Block::Dirt,true},
        Case{385*32,384,shared::Block::Grass,true},
        Case{379*32,384,shared::Block::Stone,true},
        Case{381*32,384,shared::Block::Dirt,true}
    };
    for (size_t index=0;index<cases.size();++index) {
        auto const value=cases[index];
        auto make_scene=[&](bool explicit_material) {
            auto scene=std::make_shared<client::TerrainLodScene>();
            scene->block_sea_level=384U;
            int32_t const h=value.highest+32;
            int32_t const rise=value.slope?16:0;
            std::array<std::array<int32_t,3U>,4U> const corners{
                {{0,0,h-rise},{256,0,h+rise},{256,256,h+rise},{0,256,h-rise}}};
            std::array<int32_t,3U> const center{128,128,h};
            std::vector<client::StoneFaceInstance> faces;
            for(uint32_t edge=0;edge<4U;++edge) {
                std::array<std::array<int32_t,3U>,3U> const vertices{center,corners[edge],corners[(edge+1U)%4U]};
                faces.push_back(explicit_material
                    ?client::StoneFaceInstance::fromTerrainTriangle(vertices,value.material)
                    :client::StoneFaceInstance::fromTerrainSurfaceTriangle(
                        vertices,{value.temperature,value.temperature,value.temperature}));
            }
            setSceneFaces(*scene, std::move(faces));
            return scene;
        };
        renderer.setCamera({.position={128.0,-8.0,static_cast<double>(value.highest)/32.0+4.1},
            .angles={.pitch_degrees=-12.0}});
        renderer.setTerrainLodScene(make_scene(true));
        auto const reference=renderer.render({},std::chrono::steady_clock::now()+std::chrono::seconds{10});
        renderer.setTerrainLodScene(make_scene(false));
        auto const classified=renderer.render({},std::chrono::steady_clock::now()+std::chrono::seconds{10});
        ASSERT_EQ(reference.rgba8.size(),classified.rgba8.size());
        uint32_t changed=0;
        for(size_t channel=0;channel<reference.rgba8.size();++channel) {
            if(reference.rgba8[channel]!=classified.rgba8[channel]) ++changed;
        }
        RecordProperty("case"+std::to_string(index)+"_changed_channels",static_cast<int32_t>(changed));
        EXPECT_EQ(changed,0U)<<"case "<<index<<" centered classification must match exact material without moving geometry";
    }
}

TEST(CoarseClassifierDiagnostic, NativeMaterialTrianglesPreserveCategoriesAcrossLegacyHeightCutoffs)
{
    client::InstalledShaderAssets const shaders;
    client::VulkanOffscreenRenderer renderer{shaders, false};
    renderer.setDebugHudEnabled(false);
    for (int32_t const highest : {379, 382, 384, 400}) {
        int32_t const z = (highest + 1) * 32;
        std::array<std::array<int32_t, 3>, 3> const vertices{{{0, 0, z}, {256, 0, z}, {0, 256, z}}};
        renderer.setCamera({.position = {80.0, 80.0, highest + 100.0},
            .angles = {.pitch_degrees = -89.0}});
        auto scene = std::make_shared<client::TerrainLodScene>();
        scene->block_sea_level = 384U;
        setSceneFaces(*scene, {client::StoneFaceInstance::fromTerrainTriangle(vertices, shared::Block::Grass)});
        renderer.setTerrainLodScene(scene);
        auto const reference = renderer.render({}, std::chrono::steady_clock::now() + std::chrono::seconds{10});
        scene = std::make_shared<client::TerrainLodScene>();
        scene->block_sea_level = 384U;
        setSceneFaces(*scene, {client::StoneFaceInstance::fromTerrainMaterialTriangle(vertices,
            {shared::Block::Grass, shared::Block::Grass, shared::Block::Grass})});
        renderer.setTerrainLodScene(scene);
        auto const classified = renderer.render({}, std::chrono::steady_clock::now() + std::chrono::seconds{10});
        ASSERT_EQ(reference.rgba8.size(), classified.rgba8.size());
        size_t changed_channels = 0U;
        for (size_t channel = 0U; channel < reference.rgba8.size(); ++channel) {
            changed_channels += reference.rgba8[channel] != classified.rgba8[channel] ? 1U : 0U;
        }
        EXPECT_EQ(changed_channels, 0U) << "same supplied material at highest " << highest;
    }
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(CoarseClassifierDiagnostic, NativeTriangleCoverageChoosesARealMajorityCategoryWithoutAlbedoBlending)
{
    client::InstalledShaderAssets const shaders;
    client::VulkanOffscreenRenderer renderer{shaders, false};
    renderer.setDebugHudEnabled(false);
    std::array<std::array<int32_t, 3>, 3> const vertices{{{0, 0, 12'832}, {256, 0, 12'832}, {0, 256, 12'832}}};
    renderer.setCamera({.position = {80.0, 80.0, 500.0}, .angles = {.pitch_degrees = -89.0}});
    auto capture = [&](bool const categories) {
        auto scene = std::make_shared<client::TerrainLodScene>();
        setSceneFaces(*scene, {categories
            ? client::StoneFaceInstance::fromTerrainMaterialTriangle(vertices,
                {shared::Block::Grass, shared::Block::Grass, shared::Block::Sand})
            : client::StoneFaceInstance::fromTerrainTriangle(vertices, shared::Block::Grass)});
        renderer.setTerrainLodScene(scene);
        return renderer.render({}, std::chrono::steady_clock::now() + std::chrono::seconds{10});
    };
    auto const reference = capture(false);
    auto const classified = capture(true);
    ASSERT_EQ(reference.width, classified.width);
    ASSERT_EQ(reference.height, classified.height);
    // Near XY(80,80), the two Grass vertices cover about69%; a numeric material-ID blend is invalid.
    for (uint32_t y = reference.height / 2U - 4U; y <= reference.height / 2U + 4U; ++y) {
        for (uint32_t x = reference.width / 2U - 4U; x <= reference.width / 2U + 4U; ++x) {
            size_t const pixel = (static_cast<size_t>(y) * reference.width + x) * 4U;
            for (uint32_t channel = 0U; channel < 4U; ++channel) {
                EXPECT_EQ(reference.rgba8[pixel + channel], classified.rgba8[pixel + channel]);
            }
        }
    }
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(CoarseClassifierDiagnostic, FullHeightTriangleDecodeMatchesAnExactCameraRelativeReference)
{
    client::InstalledShaderAssets const shaders;
    client::VulkanOffscreenRenderer renderer{shaders, false};
    renderer.setDebugHudEnabled(false);
    std::array<std::array<int32_t, 3>, 7> const cases{{
        {32'767, 32'767, 32'767}, {32'768, 32'768, 32'768},
        {65'535, 65'535, 65'535}, {65'536, 65'536, 65'536},
        {65'536, 65'535, 32'768}, {32'768, 65'536, 65'535}, {65'535, 32'768, 65'536},
    }};
    for (auto const& heights : cases) {
        for (uint32_t format = 0U; format < 3U; ++format) {
            auto capture = [&](bool reference) {
                int32_t const bias = reference && *std::min_element(heights.begin(), heights.end()) >= 32'768
                    ? 32'768 : 0;
                std::array<std::array<int32_t, 3>, 3> const vertices{{
                    {0, 0, heights[0] - bias}, {256, 0, heights[1] - bias}, {0, 256, heights[2] - bias}}};
                auto scene = std::make_shared<client::TerrainLodScene>();
                scene->block_sea_level = 384U;
                setSceneFaces(*scene, {reference || format == 0U
                    ? client::StoneFaceInstance::fromTerrainTriangle(vertices, shared::Block::Grass)
                    : (format == 1U
                        ? client::StoneFaceInstance::fromTerrainSurfaceTriangle(vertices, {384, 384, 384})
                        : client::StoneFaceInstance::fromTerrainMaterialTriangle(vertices,
                            {shared::Block::Grass, shared::Block::Grass, shared::Block::Grass}))});
                renderer.setTerrainLodScene(scene);
                renderer.setCamera({.position = {80.0, 80.0,
                    static_cast<double>(*std::max_element(heights.begin(), heights.end()) - bias) / 32.0 + 256.0},
                    .angles = {.pitch_degrees = -89.0}});
                return renderer.render({}, std::chrono::steady_clock::now() + std::chrono::seconds{10});
            };
            // Translating both camera and vertices by1024 blocks preserves every view-space coordinate.
            auto const reference = capture(true);
            auto const actual = capture(false);
            ASSERT_EQ(reference.rgba8.size(), actual.rgba8.size());
            size_t changed_channels = 0U;
            uint32_t visible_grass = 0U;
            for (size_t channel = 0U; channel < reference.rgba8.size(); ++channel) {
                changed_channels += reference.rgba8[channel] != actual.rgba8[channel] ? 1U : 0U;
            }
            for (size_t pixel = 0U; pixel < reference.rgba8.size(); pixel += 4U) {
                uint32_t const green = reference.rgba8[pixel + 1U];
                visible_grass += green > static_cast<uint32_t>(reference.rgba8[pixel]) + 4U
                    && green > static_cast<uint32_t>(reference.rgba8[pixel + 2U]) + 4U ? 1U : 0U;
            }
            ASSERT_GT(visible_grass, 100U);
            EXPECT_EQ(changed_channels, 0U) << "height " << heights[0] << " format " << format;
        }
    }
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}
