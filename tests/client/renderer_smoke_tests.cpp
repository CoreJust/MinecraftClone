#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <core/vulkan/GlfwSurfaceProvider.hpp>
#include <core/window/Window.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <volk.h>

#include <array>
#include <chrono>

namespace {

PFN_vkAcquireNextImageKHR original_acquire_next_image = nullptr;
uint32_t queue_submit_count = 0;
uint32_t queue_present_count = 0;

VkResult VKAPI_PTR acquireNextImageNotReady(
    VkDevice,
    VkSwapchainKHR,
    uint64_t,
    VkSemaphore,
    VkFence,
    uint32_t*
)
{
    return VK_NOT_READY;
}

class ScopedNotReadyAcquire final {
public:
    ScopedNotReadyAcquire()
    {
        original_acquire_next_image = vkAcquireNextImageKHR;
        vkAcquireNextImageKHR = acquireNextImageNotReady;
    }

    ~ScopedNotReadyAcquire()
    {
        vkAcquireNextImageKHR = original_acquire_next_image;
    }
};

VkResult VKAPI_PTR acquireNextImageSuboptimal(
    VkDevice const device,
    VkSwapchainKHR const swapchain,
    uint64_t const timeout,
    VkSemaphore const semaphore,
    VkFence const fence,
    uint32_t* const image_index
)
{
    VkResult const result = original_acquire_next_image(
        device,
        swapchain,
        timeout,
        semaphore,
        fence,
        image_index
    );
    return result == VK_SUCCESS ? VK_SUBOPTIMAL_KHR : result;
}

VkResult VKAPI_PTR countQueueSubmit(VkQueue, uint32_t, VkSubmitInfo const*, VkFence)
{
    ++queue_submit_count;
    return VK_SUCCESS;
}

VkResult VKAPI_PTR countQueuePresent(VkQueue, VkPresentInfoKHR const*)
{
    ++queue_present_count;
    return VK_SUCCESS;
}

class ScopedSuboptimalAcquire final {
public:
    ScopedSuboptimalAcquire()
        : m_queue_submit(vkQueueSubmit)
        , m_queue_present(vkQueuePresentKHR)
    {
        original_acquire_next_image = vkAcquireNextImageKHR;
        queue_submit_count = 0;
        queue_present_count = 0;
        vkAcquireNextImageKHR = acquireNextImageSuboptimal;
        vkQueueSubmit = countQueueSubmit;
        vkQueuePresentKHR = countQueuePresent;
    }

    ~ScopedSuboptimalAcquire()
    {
        vkAcquireNextImageKHR = original_acquire_next_image;
        vkQueueSubmit = m_queue_submit;
        vkQueuePresentKHR = m_queue_present;
    }

private:
    PFN_vkQueueSubmit m_queue_submit;
    PFN_vkQueuePresentKHR m_queue_present;
};

[[nodiscard]] bool hasBackgroundPixel(client::RendererFrameCapture const& capture)
{
    if (capture.rgba8.size() % 4U != 0U) {
        return false;
    }
    for (uint64_t offset = 0; offset < capture.rgba8.size(); offset += 4U) {
        uint8_t const background_max = capture.srgb_encoded ? 90U : 28U;
        if (capture.rgba8[offset] < background_max
            && capture.rgba8[offset + 1U] < background_max
            && capture.rgba8[offset + 2U] < background_max
        ) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool hasGridPixel(client::RendererFrameCapture const& capture)
{
    if (capture.rgba8.size() % 4U != 0U) {
        return false;
    }
    for (uint64_t offset = 0; offset < capture.rgba8.size(); offset += 4U) {
        uint8_t const red = capture.rgba8[offset];
        uint8_t const green = capture.rgba8[offset + 1U];
        uint8_t const blue = capture.rgba8[offset + 2U];
        uint8_t const grid_min = capture.srgb_encoded ? 90U : 32U;
        uint8_t const grid_max = capture.srgb_encoded ? 160U : 64U;
        if (red > grid_min && red < grid_max
            && green > grid_min && green < grid_max
            && blue > grid_min && blue < grid_max
        ) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool hasPlayerPixel(
    client::RendererFrameCapture const& capture,
    uint8_t const channel
)
{
    if (capture.rgba8.size() % 4U != 0U) {
        return false;
    }
    for (uint64_t offset = 0; offset < capture.rgba8.size(); offset += 4U) {
        uint8_t const red = capture.rgba8[offset];
        uint8_t const green = capture.rgba8[offset + 1U];
        uint8_t const blue = capture.rgba8[offset + 2U];
        if (channel == 0U && red > 200U && green < 80U && blue < 80U) {
            return true;
        }
        if (channel == 1U && red < 80U && green > 200U && blue < 80U) {
            return true;
        }
    }
    return false;
}

void renderSmoke(bool const prefer_mesh_shaders)
{
    static constexpr uint32_t FRAME_COUNT = 8;
    static constexpr int32_t RESIZED_WIDTH = 400;
    static constexpr int32_t RESIZED_HEIGHT = 300;
    static constexpr uint32_t RESIZE_FRAME = 2;
    static constexpr uint32_t RELOAD_FRAME = 4;
    static constexpr uint32_t CAPTURE_FRAME = 5;
    static constexpr uint32_t MAX_ATTEMPT_COUNT = 1'000;
    static constexpr auto SMOKE_TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr double RETRY_EVENT_WAIT_SECONDS = 0.01;
    core::Window const window{ "MinecraftClone renderer smoke", 320, 240 };
    core::vk::GlfwSurfaceProvider const surface_provider{ window };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        surface_provider,
        shader_assets,
        {
            .require_validation = true,
            .prefer_mesh_shaders = prefer_mesh_shaders,
            .enable_frame_capture = true,
        },
    };
    std::array<client::PlayerRenderData, 2> players{
        client::PlayerRenderData{ .x = 2, .y = 3, .color = { 1.f, 0.f, 0.f, 1.f } },
        client::PlayerRenderData{ .x = 29, .y = 28, .color = { 0.f, 1.f, 0.f, 1.f } },
    };
    uint32_t resized_width = 0;
    uint32_t resized_height = 0;
    uint32_t frame = 0;
    uint32_t attempt = 0;
    bool frame_prepared = false;
    bool resize_requested = false;
    bool resize_completed = false;
    bool reload_completed = false;
    bool capture_requested = false;
    auto const deadline = std::chrono::steady_clock::now() + SMOKE_TIMEOUT;
    while (
        frame < FRAME_COUNT
        && attempt < MAX_ATTEMPT_COUNT
        && std::chrono::steady_clock::now() < deadline
    ) {
        ++attempt;
        ASSERT_TRUE(window.nextFrame());
        ASSERT_FALSE(window.isFramebufferSizeZero());
        if (frame == RESIZE_FRAME && !resize_requested) {
            glfwSetWindowSize(window.nativeHandle(), RESIZED_WIDTH, RESIZED_HEIGHT);
            resize_requested = true;
        }
        if (frame == RESIZE_FRAME && !resize_completed) {
            auto const framebuffer_size = window.framebufferSize();
            if (framebuffer_size.first == 320U && framebuffer_size.second == 240U) {
                glfwWaitEventsTimeout(RETRY_EVENT_WAIT_SECONDS);
                continue;
            }
            resized_width = framebuffer_size.first;
            resized_height = framebuffer_size.second;
            resize_completed = true;
        }
        if (frame == RELOAD_FRAME && !reload_completed) {
            renderer.hotReload();
            reload_completed = true;
        }
        if (frame == CAPTURE_FRAME && !capture_requested) {
            renderer.requestFrameCapture();
            capture_requested = true;
        }
        if (!frame_prepared) {
            players.front().x += 1;
            frame_prepared = true;
        }
        if (renderer.render(
            frame == 0 ? std::span<client::PlayerRenderData const>{} : players,
            deadline
        )) {
            ++frame;
            frame_prepared = false;
        } else {
            glfwWaitEventsTimeout(RETRY_EVENT_WAIT_SECONDS);
        }
    }
    ASSERT_EQ(frame, FRAME_COUNT);

    client::RendererRuntimeInfo const runtime = renderer.runtimeInfo();
    EXPECT_EQ(runtime.width, resized_width);
    EXPECT_EQ(runtime.height, resized_height);
    if (prefer_mesh_shaders) {
        EXPECT_TRUE(
            runtime.pipeline_path == client::RendererPipelinePath::Vertex
            || runtime.pipeline_path == client::RendererPipelinePath::Mesh
        );
    } else {
        EXPECT_EQ(runtime.pipeline_path, client::RendererPipelinePath::Vertex);
    }
    EXPECT_GT(runtime.cpu_frame_duration.count(), 0);
    EXPECT_FALSE(runtime.gpu_frame_duration.has_value());
    EXPECT_EQ(runtime.submitted_frame_count, FRAME_COUNT);

    ASSERT_EQ(renderer.captureState(), client::FrameCaptureState::Completed);
    std::optional<client::RendererFrameCapture> const capture = renderer.takeFrameCapture();
    ASSERT_TRUE(capture.has_value());
    EXPECT_EQ(capture->width, resized_width);
    EXPECT_EQ(capture->height, resized_height);
    ASSERT_EQ(capture->rgba8.size(), static_cast<uint64_t>(capture->width) * capture->height * 4U);
    EXPECT_TRUE(hasBackgroundPixel(*capture));
    EXPECT_TRUE(hasGridPixel(*capture));
    EXPECT_TRUE(hasPlayerPixel(*capture, 0U));
    EXPECT_TRUE(hasPlayerPixel(*capture, 1U));

    uint64_t const submitted_before_close = renderer.runtimeInfo().submitted_frame_count;
    glfwSetWindowShouldClose(window.nativeHandle(), GLFW_TRUE);
    bool const should_render = window.nextFrame();
    EXPECT_FALSE(should_render);
    if (should_render) {
        static_cast<void>(renderer.render(players));
    }
    EXPECT_EQ(renderer.runtimeInfo().submitted_frame_count, submitted_before_close);
    renderer.waitIdle();
}

} // namespace

TEST(RendererSmokeTest, AutomaticPipelineDrawsAndReloads)
{
    renderSmoke(true);
}

TEST(RendererSmokeTest, VertexFallbackDrawsAndReloads)
{
    renderSmoke(false);
}

TEST(RendererSmokeTest, NotReadyAcquireSkipsFrameAndRecovers)
{
    core::Window const window{ "MinecraftClone not-ready acquire smoke", 320, 240 };
    core::vk::GlfwSurfaceProvider const surface_provider{ window };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        surface_provider,
        shader_assets,
        { .require_validation = true, .prefer_mesh_shaders = false },
    };

    {
        ScopedNotReadyAcquire const not_ready_acquire;
        EXPECT_FALSE(renderer.render(
            {},
            std::chrono::steady_clock::now() + std::chrono::seconds{ 5 }
        ));
    }

    EXPECT_TRUE(renderer.render(
        {},
        std::chrono::steady_clock::now() + std::chrono::seconds{ 5 }
    ));
    renderer.waitIdle();
}

TEST(RendererSmokeTest, SuboptimalAcquireSkipsStaleFrameAndRecovers)
{
    core::Window const window{ "MinecraftClone suboptimal acquire smoke", 320, 240 };
    core::vk::GlfwSurfaceProvider const surface_provider{ window };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        surface_provider,
        shader_assets,
        { .require_validation = true, .prefer_mesh_shaders = false },
    };

    {
        ScopedSuboptimalAcquire const suboptimal_acquire;
        EXPECT_FALSE(renderer.render(
            {},
            std::chrono::steady_clock::now() + std::chrono::seconds{ 5 }
        ));
        EXPECT_EQ(queue_submit_count, 0U);
        EXPECT_EQ(queue_present_count, 0U);
    }

    EXPECT_TRUE(renderer.render(
        {},
        std::chrono::steady_clock::now() + std::chrono::seconds{ 5 }
    ));
    renderer.waitIdle();
}
