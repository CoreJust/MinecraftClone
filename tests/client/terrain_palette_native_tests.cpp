#include <client/Camera.hpp>
#include <client/TerrainLodPresentation.hpp>
#include <client/render/CanonicalTerrainField.hpp>
#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/TerrainLodDrawIndex.hpp>
#include <client/render/VulkanFrustum.hpp>
#include <client/render/VulkanRenderer.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/Generation.hpp>
#include <core/platform/glfw/GlfwWindow.hpp>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <gtest/gtest.h>
#include <testsupport/ImageComparison.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

void bindSceneFaces(client::TerrainLodScene& scene, std::vector<client::StoneFaceInstance> faces)
{
    scene.logical_face_count = faces.size();
    for (client::TerrainLodDrawGroup& group : scene.groups) {
        if (group.range.instance_count == 0U) {
            continue;
        }
        auto const first = faces.begin() + group.range.first_instance;
        group.face_storage = std::make_shared<std::vector<client::StoneFaceInstance> const>(
            first, first + group.range.instance_count);
        group.face_storage_first_instance = 0U;
    }
}

} // namespace

TEST(TerrainPaletteNativeTest, MinifiedCanonicalMaterialBoundaryStaysCategoricalAndCloseAuthorityKeepsItsTexture)
{
    static constexpr uint32_t SOURCE_SIDE = 256U;
    static constexpr uint32_t CELL_EXTENT = 256U;
    static constexpr int32_t SURFACE_Z = 500;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 30U;
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V9;
    configuration.resolution = 512U;
    auto const atlas = generation::prepareAtlas(configuration);
    shared::TerrainWorld const world{atlas};
    auto const fields = client::render::buildV9CanonicalTerrainFieldValues(*atlas);
    std::optional<std::array<uint32_t, 3U>> minority_corner;
    for (uint32_t y = 1U; y + 1U < SOURCE_SIDE && !minority_corner; ++y) {
        for (uint32_t x = 1U; x + 1U < SOURCE_SIDE; ++x) {
            std::array<uint32_t, 4U> const materials{
                static_cast<uint32_t>(fields[y * SOURCE_SIDE + x][2]),
                static_cast<uint32_t>(fields[y * SOURCE_SIDE + x + 1U][2]),
                static_cast<uint32_t>(fields[(y + 1U) * SOURCE_SIDE + x][2]),
                static_cast<uint32_t>(fields[(y + 1U) * SOURCE_SIDE + x + 1U][2]),
            };
            if (std::ranges::count(materials, 2U) == 3 && std::ranges::count(materials, 7U) == 1) {
                auto const sand = std::ranges::find(materials, 7U);
                minority_corner = std::array{x, y, static_cast<uint32_t>(sand - materials.begin())};
                break;
            }
        }
    }
    ASSERT_TRUE(minority_corner) << "seed42 must retain a three-Grass/one-Sand material contour";
    std::optional<std::array<uint32_t, 2U>> boundary;
    for (uint32_t y = 1U; y + 1U < SOURCE_SIDE && !boundary; ++y) {
        for (uint32_t x = 1U; x + 2U < SOURCE_SIDE; ++x) {
            auto const a = static_cast<uint32_t>(fields[y * SOURCE_SIDE + x][2]);
            auto const b = static_cast<uint32_t>(fields[y * SOURCE_SIDE + x + 1U][2]);
            if ((a == 7U && b == 2U) || (a == 2U && b == 7U)) {
                boundary = std::array{x, y};
                break;
            }
        }
    }
    ASSERT_TRUE(boundary) << "seed42 must retain an adjacent Sand/Grass source pair";
    int32_t const px = static_cast<int32_t>((*boundary)[0] * CELL_EXTENT);
    int32_t const py = static_cast<int32_t>((*boundary)[1] * CELL_EXTENT);
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = 320U, .height = 240U, .title = "V9 coarse albedo and close authority",
        },
    };
    client::InstalledShaderAssets const shaders;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window, {.require_validation = true, .enable_frame_capture = true}),
        shaders, {.require_validation = true, .enable_frame_capture = true},
    };
    renderer.setDebugHudEnabled(false);
    renderer.setFallbackSceneEnabled(false);
    auto capture = [&](std::shared_ptr<client::TerrainLodScene const> scene, client::Camera const& camera,
                       char const* name) -> std::optional<client::RendererFrameCapture> {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        renderer.setCamera(camera.pose(), camera.projection());
        while (!renderer.upsertTerrainLodScene(scene, deadline)) {
            if (std::chrono::steady_clock::now() >= deadline) { return std::nullopt; }
        }
        renderer.requestFrameCapture();
        for (uint32_t frame = 0U; frame < MAX_CAPTURE_FRAMES; ++frame) {
            if (!window.nextFrame()) { return std::nullopt; }
            static_cast<void>(renderer.render({}, deadline));
            if (auto image = renderer.takeFrameCapture()) {
                if (auto const directory = std::getenv("MC_TERRAIN_PALETTE_CAPTURE_DIRECTORY")) {
                    auto const written = testsupport::writePpm(
                        {.width = image->width, .height = image->height, .srgb_encoded = image->srgb_encoded,
                            .pixels = image->rgba8},
                        std::filesystem::path{directory} / name, "V9 canonical palette; synthetic geometry");
                    if (!written) { return std::nullopt; }
                }
                return image;
            }
        }
        return std::nullopt;
    };
    auto coarse = std::make_shared<client::TerrainLodScene>();
    coarse->identity = world.identity();
    coarse->shading_atlas = atlas;
    coarse->block_sea_level = atlas->block_sea_level;
    std::vector<client::StoneFaceInstance> coarse_faces{
        client::StoneFaceInstance::fromTerrainSurfaceTriangle(
            {{{px - 128, py - 128, SURFACE_Z * 32}, {px + 384, py - 128, SURFACE_Z * 32},
                 {px + 384, py + 128, SURFACE_Z * 32}}}, {384, 384, 384}),
        client::StoneFaceInstance::fromTerrainSurfaceTriangle(
            {{{px - 128, py - 128, SURFACE_Z * 32}, {px + 384, py + 128, SURFACE_Z * 32},
                 {px - 128, py + 128, SURFACE_Z * 32}}}, {384, 384, 384}),
    };
    coarse->groups = {{
        .range = {.first_instance = 0U, .instance_count = 2U},
        .origin_x = static_cast<uint32_t>(px - 128),
        .origin_y = static_cast<uint32_t>(py - 128),
        .extent = 512U,
        .minimum_z = SURFACE_Z - 1,
        .maximum_z = SURFACE_Z + 1,
    }};
    bindSceneFaces(*coarse, std::move(coarse_faces));
    client::Camera const far_camera{
        {.position = {px + 128.0, static_cast<double>(py), SURFACE_Z + 4'096.0},
            .angles = {.pitch_degrees = -89.0}},
        {.far_plane = 32'768.0},
    };
    auto const distant = capture(coarse, far_camera, "palette-far.ppm");
    ASSERT_TRUE(distant);
    auto const projection = far_camera.projectionMatrix(distant->width, distant->height);
    ASSERT_TRUE(projection);
    int32_t maximum_jump = 0;
    std::array<int32_t, 3U> previous{};
    for (uint32_t sample = 0U; sample <= 16U; ++sample) {
        glm::vec3 position{
            static_cast<float>(px) + static_cast<float>(sample) * 16.0F,
            static_cast<float>(py + 16),
            static_cast<float>(SURFACE_Z),
        };
        glm::vec2 const offset{position.x - static_cast<float>(far_camera.pose().position.x),
            position.y - static_cast<float>(far_camera.pose().position.y)};
        position.z -= glm::dot(offset, offset) / (2.0F * 131'072.0F);
        glm::vec4 const clip = *projection * far_camera.viewMatrix() * glm::vec4{position, 1.0F};
        ASSERT_GT(clip.w, 0.0F);
        auto const x = static_cast<uint32_t>((clip.x / clip.w * 0.5F + 0.5F) * static_cast<float>(distant->width));
        auto const y = static_cast<uint32_t>((clip.y / clip.w * 0.5F + 0.5F) * static_cast<float>(distant->height));
        ASSERT_LT(x, distant->width);
        ASSERT_LT(y, distant->height);
        size_t const pixel = (static_cast<size_t>(y) * distant->width + x) * 4U;
        std::array<int32_t, 3U> const rgb{
            distant->rgba8[pixel], distant->rgba8[pixel + 1U], distant->rgba8[pixel + 2U]};
        if (sample != 0U) {
            for (uint32_t channel = 0U; channel < 3U; ++channel) {
                maximum_jump = std::max(maximum_jump, std::abs(rgb[channel] - previous[channel]));
            }
        }
        previous = rgb;
    }
    RecordProperty("coarse_palette_maximum_step", maximum_jump);
    EXPECT_GT(maximum_jump, 35);

    // Reconstruct a categorical contour instead of painting the nearest source cell's square.
    int32_t const contour_x = static_cast<int32_t>((*minority_corner)[0] * CELL_EXTENT);
    int32_t const contour_y = static_cast<int32_t>((*minority_corner)[1] * CELL_EXTENT);
    double const fraction_x = ((*minority_corner)[2] & 1U) != 0U ? 0.6 : 0.4;
    double const fraction_y = ((*minority_corner)[2] & 2U) != 0U ? 0.6 : 0.4;
    glm::vec3 const contour_position{static_cast<float>(contour_x + fraction_x * CELL_EXTENT),
        static_cast<float>(contour_y + fraction_y * CELL_EXTENT), static_cast<float>(SURFACE_Z)};
    auto contour = std::make_shared<client::TerrainLodScene>();
    contour->identity = world.identity();
    contour->shading_atlas = atlas;
    contour->block_sea_level = atlas->block_sea_level;
    std::vector<client::StoneFaceInstance> contour_faces{
        client::StoneFaceInstance::fromTerrainSurfaceTriangle(
            {{{contour_x - 128, contour_y - 128, SURFACE_Z * 32},
                 {contour_x + 384, contour_y - 128, SURFACE_Z * 32},
                 {contour_x + 384, contour_y + 384, SURFACE_Z * 32}}}, {384, 384, 384}),
        client::StoneFaceInstance::fromTerrainSurfaceTriangle(
            {{{contour_x - 128, contour_y - 128, SURFACE_Z * 32},
                 {contour_x + 384, contour_y + 384, SURFACE_Z * 32},
                 {contour_x - 128, contour_y + 384, SURFACE_Z * 32}}}, {384, 384, 384}),
    };
    contour->groups = {{
        .range = {.first_instance = 0U, .instance_count = 2U},
        .origin_x = static_cast<uint32_t>(contour_x - 128),
        .origin_y = static_cast<uint32_t>(contour_y - 128),
        .extent = 512U,
        .minimum_z = SURFACE_Z - 1,
        .maximum_z = SURFACE_Z + 1,
    }};
    bindSceneFaces(*contour, std::move(contour_faces));
    client::Camera const contour_camera{
        {.position = {contour_position.x, contour_position.y, SURFACE_Z + 512.0},
            .angles = {.pitch_degrees = -89.0}}, {.far_plane = 32'768.0}};
    auto const contour_capture = capture(contour, contour_camera, "palette-categorical-contour.ppm");
    ASSERT_TRUE(contour_capture);
    auto const contour_projection = contour_camera.projectionMatrix(contour_capture->width, contour_capture->height);
    ASSERT_TRUE(contour_projection);
    glm::vec4 const contour_clip = *contour_projection * contour_camera.viewMatrix()
        * glm::vec4{contour_position, 1.0F};
    auto const contour_pixel_x = static_cast<uint32_t>((contour_clip.x / contour_clip.w * 0.5F + 0.5F)
        * static_cast<float>(contour_capture->width));
    auto const contour_pixel_y = static_cast<uint32_t>((contour_clip.y / contour_clip.w * 0.5F + 0.5F)
        * static_cast<float>(contour_capture->height));
    ASSERT_GE(contour_pixel_x, 2U);
    ASSERT_GE(contour_pixel_y, 2U);
    ASSERT_LT(contour_pixel_x + 2U, contour_capture->width);
    ASSERT_LT(contour_pixel_y + 2U, contour_capture->height);
    uint64_t contour_red = 0U, contour_green = 0U;
    for (uint32_t y = contour_pixel_y - 2U; y <= contour_pixel_y + 2U; ++y) {
        for (uint32_t x = contour_pixel_x - 2U; x <= contour_pixel_x + 2U; ++x) {
            size_t const pixel = (static_cast<size_t>(y) * contour_capture->width + x) * 4U;
            contour_red += contour_capture->rgba8[pixel];
            contour_green += contour_capture->rgba8[pixel + 1U];
        }
    }
    EXPECT_GT(contour_green, contour_red + 25U * 5U)
        << "Grass covers 64% of this interpolated material footprint; the minority Sand must not form a square";

    // A nearby authoritative Sand block overrides the global Grass source cell.
    int32_t const grass_x = px + (fields[(*boundary)[1] * SOURCE_SIDE + (*boundary)[0]][2] == 2.0F ? 0 : 256);
    auto exact = std::make_shared<client::TerrainLodScene>();
    exact->identity = world.identity();
    exact->shading_atlas = atlas;
    std::vector<client::StoneFaceInstance> exact_faces{{.x = grass_x, .y = py, .z = SURFACE_Z - 1, .direction = 5U,
        .u_extent = 16U, .v_extent = 16U, .material_id = 7U}};
    exact->groups = {{
        .range = {.first_instance = 0U, .instance_count = 1U},
        .origin_x = static_cast<uint32_t>(grass_x),
        .origin_y = static_cast<uint32_t>(py),
        .extent = 16U,
        .minimum_z = SURFACE_Z - 1,
        .maximum_z = SURFACE_Z,
    }};
    bindSceneFaces(*exact, std::move(exact_faces));
    auto const close = capture(exact, client::Camera{
        {.position = {grass_x + 8.0, py + 8.0, SURFACE_Z + 3.0}, .angles = {.pitch_degrees = -89.0}}},
        "palette-close-authority.ppm");
    ASSERT_TRUE(close);
    uint32_t red_min = 255U, red_max = 0U;
    uint64_t red_sum = 0U, green_sum = 0U;
    for (uint32_t y = close->height / 2U - 40U; y < close->height / 2U + 40U; ++y) {
        for (uint32_t x = close->width / 2U - 40U; x < close->width / 2U + 40U; ++x) {
            size_t const pixel = (static_cast<size_t>(y) * close->width + x) * 4U;
            uint32_t const red = close->rgba8[pixel];
            red_min = std::min(red_min, red);
            red_max = std::max(red_max, red);
            red_sum += red;
            green_sum += close->rgba8[pixel + 1U];
        }
    }
    EXPECT_GT(red_max - red_min, 20U);
    EXPECT_GT(red_sum, green_sum + 6'400U * 5U);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(TerrainPaletteNativeTest, CapturesCanonicalLodAtOwnerPlaytestPositions)
{
    static constexpr uint32_t WIDTH = 1'280U;
    static constexpr uint32_t HEIGHT = 720U;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 30U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 30 };
    // HUD Y is internal Z (vertical), and HUD Z is internal Y (the second horizontal).
    static constexpr std::array<std::array<double, 5U>, 3U> OWNER_CASES{{
        {37'294.5, 40'640.4, 2'189.1, 234.3, -52.8},
        {20'167.7, 43'565.5, 867.1, 252.9, -49.4},
        {20'167.7, 4'365.5, 867.1, 252.9, -49.4},
    }};
    static constexpr std::array<char const*, OWNER_CASES.size()> OWNER_CASE_NAMES{
        "owner-2-assigned", "owner-3-assigned", "owner-3-hud-literal",
    };
    char const* const capture_directory = std::getenv("MC_TERRAIN_OWNER_CASE_CAPTURE_DIRECTORY");
    if (capture_directory == nullptr || capture_directory[0] == '\0') {
        GTEST_SKIP() << "set MC_TERRAIN_OWNER_CASE_CAPTURE_DIRECTORY for same-position terrain captures";
    }

    generation::GenerationConfiguration configuration;
    char const* const requested_model = std::getenv("MC_TERRAIN_OWNER_MODEL");
    bool const use_v21 = requested_model != nullptr && std::string{requested_model} == "v21";
    configuration.model_tag = use_v21 ? generation::TerrainModelTag::V21 : generation::TerrainModelTag::V9;
    configuration.resolution = 256U;
    RecordProperty("terrain_model", use_v21 ? "v21" : "v9");
    auto const atlas = generation::prepareAtlas(configuration);
    shared::TerrainWorld const world{atlas};
    std::filesystem::path const output_directory{capture_directory};
    std::filesystem::create_directories(output_directory);
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = WIDTH, .height = HEIGHT, .title = "Owner-position canonical terrain LOD capture",
        },
    };
    client::InstalledShaderAssets const shaders;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window, {.require_validation = true, .enable_frame_capture = true}),
        shaders, {.require_validation = true, .enable_frame_capture = true},
    };
    renderer.setDebugHudEnabled(false);
    renderer.setFallbackSceneEnabled(false);

    for (uint32_t case_index = 0U; case_index < OWNER_CASES.size(); ++case_index) {
        std::array<double, 5U> const& owner_case = OWNER_CASES[case_index];
        auto const full_scene = client::buildTerrainLodScene(
            world,
            static_cast<uint32_t>(owner_case[0]),
            static_cast<uint32_t>(owner_case[1])
        );
        ASSERT_GT(client::terrainLodSceneFaceCount(*full_scene), 0U);
        ASSERT_NE(full_scene->draw_index, nullptr);
        client::Camera const camera{
            {
                .position = {owner_case[0], owner_case[1], owner_case[2]},
                .angles = {.yaw_degrees = owner_case[3], .pitch_degrees = owner_case[4]},
            },
            {.vertical_fov_degrees = 70.0, .near_plane = 0.1, .far_plane = 32'768.0},
        };
        auto const projection = camera.projectionMatrix(WIDTH, HEIGHT);
        ASSERT_TRUE(projection);
        glm::mat4 const camera_relative_view = glm::lookAtRH(
            glm::vec3{0.0F}, glm::vec3{camera.forward()}, glm::vec3{camera.up()}
        );
        client::VulkanFrustum const frustum{*projection * camera_relative_view};
        std::vector<uint32_t> visible_groups;
        static_cast<void>(full_scene->draw_index->collect(frustum, camera.pose().position, visible_groups));
        auto scene = std::make_shared<client::TerrainLodScene>();
        scene->identity = full_scene->identity;
        scene->session_join_epoch = full_scene->session_join_epoch;
        scene->coverage_epoch = full_scene->coverage_epoch;
        scene->center_x = full_scene->center_x;
        scene->center_y = full_scene->center_y;
        scene->shading_atlas = full_scene->shading_atlas;
        scene->root_extent = full_scene->root_extent;
        scene->block_sea_level = full_scene->block_sea_level;
        scene->actual_column_count = full_scene->actual_column_count;
        scene->chunk_sources = full_scene->chunk_sources;
        scene->authority_sources = full_scene->authority_sources;
        scene->groups.reserve(visible_groups.size());
        auto visible_draw_index = std::make_shared<client::TerrainLodDrawIndex>();
        for (uint32_t const group_index : visible_groups) {
            client::TerrainLodDrawGroup group = full_scene->groups[group_index];
            group.range.first_instance = static_cast<uint32_t>(scene->logical_face_count);
            scene->logical_face_count += group.range.instance_count;
            scene->groups.push_back(group);
        }
        visible_draw_index->reset(scene->groups);
        scene->draw_index = std::move(visible_draw_index);
        ASSERT_GT(client::terrainLodSceneFaceCount(*scene), 0U) << OWNER_CASE_NAMES[case_index];
        renderer.clearTerrainLodScene();
        renderer.setCamera(camera.pose(), camera.projection());
        auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
        bool uploaded = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (renderer.upsertTerrainLodScene(scene, deadline)) {
                uploaded = true;
                break;
            }
        }
        ASSERT_TRUE(uploaded) << OWNER_CASE_NAMES[case_index] << " visible faces="
                              << client::terrainLodSceneFaceCount(*scene);
        renderer.requestFrameCapture();
        std::optional<client::RendererFrameCapture> capture;
        for (uint32_t frame = 0U; frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline; ++frame) {
            if (!window.nextFrame()) {
                break;
            }
            static_cast<void>(renderer.render({}, deadline));
            capture = renderer.takeFrameCapture();
            if (capture.has_value()) {
                break;
            }
        }
        ASSERT_TRUE(capture.has_value()) << OWNER_CASE_NAMES[case_index];
        ASSERT_GT(capture->width, 0U);
        ASSERT_GT(capture->height, 0U);
        ASSERT_EQ(capture->rgba8.size(), static_cast<size_t>(capture->width) * capture->height * 4U);
        EXPECT_EQ(static_cast<uint64_t>(capture->width) * HEIGHT,
            static_cast<uint64_t>(capture->height) * WIDTH);
        auto const output_path = output_directory / (std::string{OWNER_CASE_NAMES[case_index]} + ".ppm");
        auto const written = testsupport::writePpm(
            {.width = capture->width, .height = capture->height, .srgb_encoded = capture->srgb_encoded,
                .pixels = capture->rgba8},
            output_path,
            use_v21 ? "V21 canonical TerrainLodScene at an owner playtest position"
                : "V9 canonical TerrainLodScene at an owner playtest position"
        );
        ASSERT_TRUE(written.has_value()) << written.error();
        RecordProperty(
            (std::string{OWNER_CASE_NAMES[case_index]} + "_capture_width").c_str(),
            static_cast<int64_t>(capture->width)
        );
        RecordProperty(
            (std::string{OWNER_CASE_NAMES[case_index]} + "_capture_height").c_str(),
            static_cast<int64_t>(capture->height)
        );
        RecordProperty(
            (std::string{OWNER_CASE_NAMES[case_index]} + "_face_count").c_str(),
            static_cast<int64_t>(client::terrainLodSceneFaceCount(*scene))
        );
        RecordProperty(
            (std::string{OWNER_CASE_NAMES[case_index]} + "_visible_group_count").c_str(),
            static_cast<int64_t>(scene->groups.size())
        );
        RecordProperty(
            (std::string{OWNER_CASE_NAMES[case_index]} + "_leaf_count").c_str(),
            static_cast<int64_t>(scene->leaves.size())
        );
    }
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}
