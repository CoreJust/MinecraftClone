#include <acceptance/RendererBenchmark.hpp>

#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/ProjectInfo.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#include <fmt/format.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <thread>
#include <vector>

namespace acceptance {

namespace {

std::string presentModeName(client::RendererPresentMode const mode)
{
    switch (mode) {
        case client::RendererPresentMode::Immediate: return "immediate";
        case client::RendererPresentMode::Mailbox: return "mailbox";
        case client::RendererPresentMode::FIFO: return "fifo";
        case client::RendererPresentMode::FIFORelaxed: return "fifo-relaxed";
        case client::RendererPresentMode::Unknown: return "unknown";
    }
    return "unknown";
}

std::string pipelinePathName(client::RendererPipelinePath const path)
{
    switch (path) {
        case client::RendererPipelinePath::Vertex: return "vertex";
        case client::RendererPipelinePath::Mesh: return "mesh";
    }
    return "unknown";
}

std::string platformName()
{
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#else
    return "unsupported";
#endif
}

std::string hardwareName()
{
    std::string cpu_name{ "unknown" };
    uint64_t ram_bytes{ 0 };
#if defined(__APPLE__)
    size_t name_size{ 0 };
    if (sysctlbyname("machdep.cpu.brand_string", nullptr, &name_size, nullptr, 0) == 0U
        && name_size > 1U
    ) {
        std::string name(name_size, '\0');
        if (sysctlbyname("machdep.cpu.brand_string", name.data(), &name_size, nullptr, 0) == 0U) {
            name.pop_back();
            cpu_name = std::move(name);
        }
    }
    size_t memory_size = sizeof(ram_bytes);
    static_cast<void>(sysctlbyname("hw.memsize", &ram_bytes, &memory_size, nullptr, 0));
#elif defined(_WIN32)
    std::array<char, 256> processor_identifier{ };
    DWORD const length = GetEnvironmentVariableA(
        "PROCESSOR_IDENTIFIER",
        processor_identifier.data(),
        static_cast<DWORD>(processor_identifier.size())
    );
    if (length > 0U && length < processor_identifier.size()) {
        cpu_name = processor_identifier.data();
    }
    MEMORYSTATUSEX memory_status{ .dwLength = sizeof(MEMORYSTATUSEX) };
    if (GlobalMemoryStatusEx(&memory_status) != 0) {
        ram_bytes = memory_status.ullTotalPhys;
    }
#endif
    return "cpu=" + cpu_name + "; logical_cpus="
        + std::to_string(std::thread::hardware_concurrency()) + "; ram_bytes="
        + std::to_string(ram_bytes);
}

[[nodiscard]]
uint64_t percentileIndex(uint64_t const sample_count, uint64_t const percentile)
{
    return (sample_count * percentile + 99U) / 100U - 1U;
}

} // namespace

FrameTimingSummary summarizeFrameTimings(std::span<std::chrono::nanoseconds const> const samples)
{
    if (samples.empty()) {
        return { };
    }
    std::vector<std::chrono::nanoseconds> sorted_samples{ samples.begin(), samples.end() };
    std::ranges::sort(sorted_samples);
    uint64_t total_nanoseconds{ 0 };
    for (std::chrono::nanoseconds const sample : sorted_samples) {
        total_nanoseconds += sample.count() > 0
            ? static_cast<uint64_t>(sample.count())
            : 0U;
    }
    uint64_t const sample_count = static_cast<uint64_t>(sorted_samples.size());
    return FrameTimingSummary{
        .sample_count = sample_count,
        .p50 = sorted_samples[percentileIndex(sample_count, 50U)],
        .p95 = sorted_samples[percentileIndex(sample_count, 95U)],
        .p99 = sorted_samples[percentileIndex(sample_count, 99U)],
        .maximum = sorted_samples.back(),
        .mean = std::chrono::nanoseconds{
            static_cast<int64_t>(total_nanoseconds / sample_count),
        },
    };
}

std::expected<RuntimeEvidence, std::string> runRendererBenchmark(
    RendererBenchmarkOptions const& options
)
{
    if (options.max_resize_polls == 0U || options.max_frames == 0U
        || options.warmup_duration < std::chrono::seconds::zero()
        || options.sample_duration <= std::chrono::seconds::zero()
        || options.deadline <= options.warmup_duration + options.sample_duration
    ) {
        return std::unexpected("renderer benchmark has invalid frame or monotonic time limits");
    }
    if (options.requested_width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
        || options.requested_height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
    ) {
        return std::unexpected("renderer benchmark extent is not supported by the window API");
    }

    core::platform::glfw::GlfwWindow const window{
        core::platform::glfw::WindowDescriptor{
            .width = options.requested_width,
            .height = options.requested_height,
            .title = "MinecraftClone renderer benchmark",
        },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(window),
        shader_assets,
        {
            .require_validation = options.require_validation,
            .require_immediate_present_mode = options.require_immediate_present_mode,
        },
    };
    std::vector<std::chrono::nanoseconds> samples;
    std::vector<client::PlayerRenderData> const players{
        client::PlayerRenderData{
            .x = 4U,
            .y = 4U,
            .color = { 1.0F, 1.0F, 1.0F, 1.0F },
        },
    };
    std::chrono::steady_clock::time_point const started_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point const deadline = started_at + options.deadline;
    uint64_t rendered_frames{ 0 };
    while (std::chrono::steady_clock::now() - started_at < options.warmup_duration) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected("renderer benchmark exceeded its monotonic deadline during warmup");
        }
        if (rendered_frames >= options.max_frames) {
            return std::unexpected("renderer benchmark reached its frame safety limit during warmup");
        }
        if (!window.nextFrame()) {
            return std::unexpected("renderer benchmark window was closed during warmup");
        }
        if (renderer.render(players, deadline)) {
            ++rendered_frames;
        }
    }
    if (!renderer.waitForSubmittedFrames(deadline)) {
        return std::unexpected("renderer benchmark exceeded its monotonic deadline while draining warmup frames");
    }
    std::chrono::steady_clock::time_point const sample_started_at = std::chrono::steady_clock::now();
    uint64_t frame_count{ 0 };
    while (std::chrono::steady_clock::now() - sample_started_at < options.sample_duration) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected("renderer benchmark exceeded its monotonic deadline");
        }
        if (rendered_frames >= options.max_frames) {
            return std::unexpected("renderer benchmark reached its frame safety limit");
        }
        if (!window.nextFrame()) {
            return std::unexpected("renderer benchmark window was closed");
        }
        if (renderer.render(players, deadline)) {
            samples.push_back(renderer.runtimeInfo().cpu_frame_duration);
            ++rendered_frames;
            ++frame_count;
        }
    }
    std::chrono::steady_clock::time_point const sample_finished_at = std::chrono::steady_clock::now();
    if (!renderer.waitForSubmittedFrames(deadline)) {
        return std::unexpected("renderer benchmark exceeded its monotonic deadline while draining sample frames");
    }
    std::chrono::steady_clock::duration const sample_elapsed = sample_finished_at - sample_started_at;
    std::chrono::steady_clock::duration const elapsed = std::chrono::steady_clock::now() - started_at;
    client::RendererRuntimeInfo const runtime = renderer.runtimeInfo();
    if (frame_count == 0U) {
        return std::unexpected("renderer benchmark did not issue a render and presentation request");
    }
    if (runtime.width != options.requested_width || runtime.height != options.requested_height) {
        return std::unexpected("renderer benchmark did not retain the requested framebuffer extent");
    }
    double const sample_elapsed_seconds = std::chrono::duration<double>(sample_elapsed).count();
    FrameTimingSummary const timings = summarizeFrameTimings(samples);

    return RuntimeEvidence{
        .mode = "benchmark-render",
        .ticks = frame_count,
        .expectations_passed = 1U,
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
        .deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline),
        .passed = true,
        .benchmark = RendererBenchmarkEvidence{
            .package_id = std::string{ shared::PROJECT_NAME } + " "
                + fmt::format("{}", shared::PROJECT_VERSION),
            .platform = platformName(),
            .hardware = hardwareName(),
            .gpu = runtime.gpu_name,
            .vulkan_api_version = fmt::format("{}", runtime.vulkan_api_version),
            .present_mode = presentModeName(runtime.present_mode),
            .pipeline_path = pipelinePathName(runtime.pipeline_path),
            .requested_width = options.requested_width,
            .requested_height = options.requested_height,
            .actual_width = runtime.width,
            .actual_height = runtime.height,
            .validation_enabled = runtime.validation_enabled,
            .warmup = std::chrono::duration_cast<std::chrono::milliseconds>(options.warmup_duration),
            .sample_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(sample_elapsed),
            .presentation_requests_per_second = sample_elapsed_seconds > 0.0
                ? static_cast<double>(frame_count) / sample_elapsed_seconds
                : 0.0,
            .presentation_request_timings = timings,
        },
    };
}

} // namespace acceptance
