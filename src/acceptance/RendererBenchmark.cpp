#include <acceptance/RendererBenchmark.hpp>

#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/ProjectInfo.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#include <acceptance/FramebufferExtent.hpp>
#include <fmt/format.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
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

[[nodiscard]]
constexpr int32_t floorDivideByHeightTileSide(int32_t const value) noexcept
{
    int32_t result = value / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    if (value < 0 && value % static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH) != 0) {
        --result;
    }
    return result;
}

[[nodiscard]]
shared::HeightTileSurfaceNeighbors heightTileNeighbors(
    std::vector<shared::HeightTile> const& tiles,
    uint32_t const x,
    uint32_t const y
)
{
    uint32_t constexpr side = shared::HEIGHT_TILE_INTEREST_WIDTH;
    auto const neighbor = [&tiles](uint32_t const neighbor_x, uint32_t const neighbor_y) {
        return std::optional<shared::HeightTile::Heights>{tiles[neighbor_y * side + neighbor_x].heights};
    };
    return {
        .negative_x = x == 0U ? std::nullopt : neighbor(x - 1U, y),
        .positive_x = x + 1U == side ? std::nullopt : neighbor(x + 1U, y),
        .negative_y = y == 0U ? std::nullopt : neighbor(x, y - 1U),
        .positive_y = y + 1U == side ? std::nullopt : neighbor(x, y + 1U),
    };
}

[[nodiscard]]
std::vector<shared::HeightTile> makeS6HeightTileWindow(
    shared::HeightTileCoordinate const center,
    shared::TerrainGenerator const& generator
)
{
    static constexpr int32_t NEGATIVE_RADIUS = 45;
    static constexpr int32_t POSITIVE_RADIUS = 44;
    std::vector<shared::HeightTile> tiles;
    tiles.reserve(shared::HEIGHT_TILE_INTEREST_COUNT);
    for (int32_t y_offset = -NEGATIVE_RADIUS; y_offset <= POSITIVE_RADIUS; ++y_offset) {
        for (int32_t x_offset = -NEGATIVE_RADIUS; x_offset <= POSITIVE_RADIUS; ++x_offset) {
            tiles.push_back(generator.generateHeightTile({
                .x = center.x + x_offset,
                .y = center.y + y_offset,
            }));
        }
    }
    return tiles;
}

constexpr client::CameraPose S6_CAMERA{
    .position = {
        static_cast<double>(shared::World::FLIGHT_SPAWN.x),
        static_cast<double>(shared::World::FLIGHT_SPAWN.y),
        static_cast<double>(shared::World::FLIGHT_SPAWN.z),
    },
    .angles = { .pitch_degrees = -25.0 },
};

[[nodiscard]]
client::CameraPose cameraForHeightTileCenter(shared::HeightTileCoordinate const center) noexcept
{
    return {
        .position = {
            static_cast<double>(center.x * static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)),
            static_cast<double>(center.y * static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)),
            static_cast<double>(shared::World::FLIGHT_SPAWN.z),
        },
        .angles = S6_CAMERA.angles,
    };
}

[[nodiscard]]
uint32_t tileIndex(uint32_t const x, uint32_t const y) noexcept
{
    return y * shared::HEIGHT_TILE_INTEREST_WIDTH + x;
}

void shiftHeightTileWindow(
    std::vector<shared::HeightTile>& tiles,
    RendererBenchmarkStreamUpdate const& update,
    shared::TerrainGenerator const& generator
)
{
    uint32_t constexpr side = shared::HEIGHT_TILE_INTEREST_WIDTH;
    switch (update.direction) {
        case RendererBenchmarkStreamDirection::PositiveX:
            for (uint32_t y = 0U; y < side; ++y) {
                auto const row_begin = tiles.begin() + static_cast<int64_t>(tileIndex(0U, y));
                std::move(row_begin + 1, row_begin + side, row_begin);
                tiles[tileIndex(side - 1U, y)] = generator.generateHeightTile(update.additions[y]);
            }
            return;
        case RendererBenchmarkStreamDirection::NegativeX:
            for (uint32_t y = 0U; y < side; ++y) {
                auto const row_begin = tiles.begin() + static_cast<int64_t>(tileIndex(0U, y));
                std::move_backward(row_begin, row_begin + side - 1, row_begin + side);
                tiles[tileIndex(0U, y)] = generator.generateHeightTile(update.additions[y]);
            }
            return;
        case RendererBenchmarkStreamDirection::PositiveY:
            std::move(tiles.begin() + side, tiles.end(), tiles.begin());
            for (uint32_t x = 0U; x < side; ++x) {
                tiles[tileIndex(x, side - 1U)] = generator.generateHeightTile(update.additions[x]);
            }
            return;
        case RendererBenchmarkStreamDirection::NegativeY:
            std::move_backward(tiles.begin(), tiles.end() - side, tiles.end());
            for (uint32_t x = 0U; x < side; ++x) {
                tiles[tileIndex(x, 0U)] = generator.generateHeightTile(update.additions[x]);
            }
            return;
    }
}

struct PendingMeshUpdate final {
    uint32_t x;
    uint32_t y;
    std::optional<shared::HeightTileCoordinate> removal;
};

[[nodiscard]]
std::vector<PendingMeshUpdate> windowEdgeMeshUpdates(RendererBenchmarkStreamUpdate const& update)
{
    uint32_t constexpr side = shared::HEIGHT_TILE_INTEREST_WIDTH;
    std::vector<uint32_t> order(side);
    for (uint32_t index = 0U; index < side; ++index) {
        order[index] = index;
    }
    std::ranges::stable_sort(order, [](uint32_t const first, uint32_t const second) {
        int32_t const center = static_cast<int32_t>(shared::HEIGHT_TILE_INTEREST_WIDTH) - 1;
        return std::abs(static_cast<int32_t>(first * 2U) - center)
            < std::abs(static_cast<int32_t>(second * 2U) - center);
    });
    std::vector<PendingMeshUpdate> updates;
    updates.reserve(side * 3U);
    for (uint32_t const index : order) {
        switch (update.direction) {
            case RendererBenchmarkStreamDirection::PositiveX:
                updates.push_back({side - 1U, index, update.removals[index]});
                updates.push_back({side - 2U, index, std::nullopt});
                updates.push_back({0U, index, std::nullopt});
                break;
            case RendererBenchmarkStreamDirection::NegativeX:
                updates.push_back({0U, index, update.removals[index]});
                updates.push_back({1U, index, std::nullopt});
                updates.push_back({side - 1U, index, std::nullopt});
                break;
            case RendererBenchmarkStreamDirection::PositiveY:
                updates.push_back({index, side - 1U, update.removals[index]});
                updates.push_back({index, side - 2U, std::nullopt});
                updates.push_back({index, 0U, std::nullopt});
                break;
            case RendererBenchmarkStreamDirection::NegativeY:
                updates.push_back({index, 0U, update.removals[index]});
                updates.push_back({index, 1U, std::nullopt});
                updates.push_back({index, side - 1U, std::nullopt});
                break;
        }
    }
    return updates;
}

} // namespace

RendererBenchmarkStreamUpdate makeRendererBenchmarkStreamUpdate(
    shared::HeightTileCoordinate const center,
    uint64_t const completed_updates
)
{
    int32_t constexpr negative_radius = -45;
    int32_t constexpr positive_radius = 44;
    uint64_t constexpr side = shared::HEIGHT_TILE_INTEREST_WIDTH;
    RendererBenchmarkStreamDirection const direction = static_cast<RendererBenchmarkStreamDirection>(
        (completed_updates / side) % 4U
    );
    RendererBenchmarkStreamUpdate update{ .center = center, .direction = direction };
    update.removals.reserve(shared::HEIGHT_TILE_INTEREST_WIDTH);
    update.additions.reserve(shared::HEIGHT_TILE_INTEREST_WIDTH);
    for (int32_t offset = negative_radius; offset <= positive_radius; ++offset) {
        switch (direction) {
            case RendererBenchmarkStreamDirection::PositiveX:
                update.removals.push_back({ .x = center.x + negative_radius, .y = center.y + offset });
                update.additions.push_back({ .x = center.x - negative_radius, .y = center.y + offset });
                break;
            case RendererBenchmarkStreamDirection::PositiveY:
                update.removals.push_back({ .x = center.x + offset, .y = center.y + negative_radius });
                update.additions.push_back({ .x = center.x + offset, .y = center.y - negative_radius });
                break;
            case RendererBenchmarkStreamDirection::NegativeX:
                update.removals.push_back({ .x = center.x + positive_radius, .y = center.y + offset });
                update.additions.push_back({ .x = center.x + negative_radius - 1, .y = center.y + offset });
                break;
            case RendererBenchmarkStreamDirection::NegativeY:
                update.removals.push_back({ .x = center.x + offset, .y = center.y + positive_radius });
                update.additions.push_back({ .x = center.x + offset, .y = center.y + negative_radius - 1 });
                break;
        }
    }
    switch (direction) {
        case RendererBenchmarkStreamDirection::PositiveX: update.center.x += 1; break;
        case RendererBenchmarkStreamDirection::PositiveY: update.center.y += 1; break;
        case RendererBenchmarkStreamDirection::NegativeX: update.center.x -= 1; break;
        case RendererBenchmarkStreamDirection::NegativeY: update.center.y -= 1; break;
    }
    return update;
}

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

bool rendererBenchmarkMeetsFrameTarget(
    double const presentation_requests_per_second,
    FrameTimingSummary const& timings
) noexcept
{
    // At 120 Hz the presentation call itself waits for the display deadline. Allow
    // normal scheduler jitter around that 8.33 ms deadline while still rejecting a
    // missed refresh interval at p99.
    constexpr double NOMINAL_REFRESH_RATE = 120.0;
    constexpr double HOST_CLOCK_TOLERANCE = 0.01;
    return presentation_requests_per_second >= NOMINAL_REFRESH_RATE * (1.0 - HOST_CLOCK_TOLERANCE)
        && timings.p99 <= std::chrono::nanoseconds{ 10'000'000 };
}

client::VulkanRendererOptions makeRendererBenchmarkVulkanOptions(
    RendererBenchmarkOptions const& options
) noexcept
{
    return {
        .require_validation = options.require_validation,
        .require_immediate_present_mode = options.require_immediate_present_mode,
    };
}

bool rendererBenchmarkPresentModeSatisfied(
    RendererBenchmarkOptions const& options,
    client::RendererPresentMode const negotiated_mode
) noexcept
{
    return !options.require_immediate_present_mode
        || negotiated_mode == client::RendererPresentMode::Immediate;
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
    std::expected<void, std::string> const extent = ensureFramebufferExtent(
        window,
        { .width = options.requested_width, .height = options.requested_height },
        options.max_resize_polls
    );
    if (!extent.has_value()) {
        return std::unexpected(extent.error());
    }
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRendererOptions const renderer_options = makeRendererBenchmarkVulkanOptions(options);
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(window, renderer_options),
        shader_assets,
        renderer_options,
    };
    renderer.setDebugHudEnabled(options.debug_hud_enabled);
    shared::HeightTileCoordinate stream_center{
        .x = floorDivideByHeightTileSide(shared::World::FLIGHT_SPAWN.x),
        .y = floorDivideByHeightTileSide(shared::World::FLIGHT_SPAWN.y),
    };
    shared::TerrainGenerator const generator;
    std::vector<shared::HeightTile> stream_tiles = makeS6HeightTileWindow(stream_center, generator);
    if (stream_tiles.size() != shared::HEIGHT_TILE_INTEREST_COUNT) {
        return std::unexpected("renderer benchmark generated an invalid S6 height-tile scene");
    }
    renderer.setCamera(cameraForHeightTileCenter(stream_center));
    shared::HeightTileSurfaceMesher const mesher;
    for (uint32_t y = 0U; y < shared::HEIGHT_TILE_INTEREST_WIDTH; ++y) {
        for (uint32_t x = 0U; x < shared::HEIGHT_TILE_INTEREST_WIDTH; ++x) {
            renderer.upsertHeightTileMesh(
                mesher.build(stream_tiles[tileIndex(x, y)], heightTileNeighbors(stream_tiles, x, y))
            );
        }
    }
    client::RendererRuntimeInfo const initial_runtime = renderer.runtimeInfo();
    if (initial_runtime.height_tile_mesh_count != shared::HEIGHT_TILE_INTEREST_COUNT
        || initial_runtime.chunk_face_count == 0U
        || initial_runtime.chunk_mesh_upload_count != shared::HEIGHT_TILE_INTEREST_COUNT
    ) {
        return std::unexpected("renderer benchmark did not fully upload the S6 height-tile scene");
    }
    std::vector<std::chrono::nanoseconds> samples;
    std::vector<std::chrono::nanoseconds> acquire_wait_samples;
    std::vector<std::chrono::nanoseconds> command_record_samples;
    std::vector<std::chrono::nanoseconds> complete_present_wait_samples;
    std::span<client::PlayerRenderData const> const players{ };
    std::chrono::steady_clock::time_point const started_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point const deadline = started_at + options.deadline;
    uint64_t rendered_frames{ 0 };
    uint64_t completed_stream_updates{ 0 };
    std::vector<PendingMeshUpdate> pending_mesh_updates;
    uint32_t pending_mesh_cursor = 0U;
    auto const streamOneWindowEdge = [&]() -> std::expected<void, std::string> {
        RendererBenchmarkStreamUpdate const update = makeRendererBenchmarkStreamUpdate(
            stream_center, completed_stream_updates++
        );
        shiftHeightTileWindow(stream_tiles, update, generator);
        pending_mesh_updates = windowEdgeMeshUpdates(update);
        pending_mesh_cursor = 0U;
        stream_center = update.center;
        renderer.setCamera(cameraForHeightTileCenter(stream_center));
        return { };
    };
    auto const processPendingMeshes = [&]() -> std::expected<void, std::string> {
        uint32_t constexpr MAXIMUM_MESH_UPDATES_PER_FRAME = 16U;
        shared::HeightTileSurfaceMesher mesher;
        uint32_t processed = 0U;
        while (pending_mesh_cursor < pending_mesh_updates.size()
            && processed < MAXIMUM_MESH_UPDATES_PER_FRAME) {
            PendingMeshUpdate const& update = pending_mesh_updates[pending_mesh_cursor++];
            if (update.removal && !renderer.removeHeightTileMesh(*update.removal)) {
                return std::unexpected("renderer benchmark did not evict an outgoing S6 height tile");
            }
            renderer.upsertHeightTileMesh(mesher.build(
                stream_tiles[tileIndex(update.x, update.y)],
                heightTileNeighbors(stream_tiles, update.x, update.y)
            ));
            ++processed;
        }
        if (pending_mesh_cursor == pending_mesh_updates.size()) {
            pending_mesh_updates.clear();
            pending_mesh_cursor = 0U;
        }
        return { };
    };
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
        if (pending_mesh_updates.empty() && rendered_frames % 4U == 0U) {
            std::expected<void, std::string> const streamed = streamOneWindowEdge();
            if (!streamed.has_value()) {
                return std::unexpected(streamed.error());
            }
        }
        if (std::expected<void, std::string> const meshed = processPendingMeshes(); !meshed.has_value()) {
            return std::unexpected(meshed.error());
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected("renderer benchmark exceeded its monotonic deadline while streaming warmup tiles");
        }
        bool const presented = renderer.render(players, deadline);
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(
                "renderer benchmark exceeded its monotonic deadline while presenting a streamed warmup frame"
            );
        }
        if (presented) {
            ++rendered_frames;
        }
    }
    if (!renderer.waitForSubmittedFrames(deadline)) {
        return std::unexpected("renderer benchmark exceeded its monotonic deadline while draining warmup frames");
    }
    std::chrono::steady_clock::time_point const sample_started_at = std::chrono::steady_clock::now();
    uint64_t frame_count{ 0 };
    bool observed_stone_draw = false;
    while (std::chrono::steady_clock::now() - sample_started_at < options.sample_duration) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected("renderer benchmark exceeded its monotonic deadline");
        }
        if (rendered_frames >= options.max_frames) {
            return std::unexpected("renderer benchmark reached its frame safety limit");
        }
        std::chrono::steady_clock::time_point const frame_started_at = std::chrono::steady_clock::now();
        if (!window.nextFrame()) {
            return std::unexpected("renderer benchmark window was closed");
        }
        if (pending_mesh_updates.empty() && rendered_frames % 4U == 0U) {
            std::expected<void, std::string> const streamed = streamOneWindowEdge();
            if (!streamed.has_value()) {
                return std::unexpected(streamed.error());
            }
        }
        if (std::expected<void, std::string> const meshed = processPendingMeshes(); !meshed.has_value()) {
            return std::unexpected(meshed.error());
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected("renderer benchmark exceeded its monotonic deadline while streaming tiles");
        }
        bool const presented = renderer.render(players, deadline);
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected("renderer benchmark exceeded its monotonic deadline while presenting a streamed frame");
        }
        if (presented) {
            client::RendererRuntimeInfo const frame_runtime = renderer.runtimeInfo();
            observed_stone_draw = observed_stone_draw
                || (
                    frame_runtime.height_tile_mesh_count == shared::HEIGHT_TILE_INTEREST_COUNT
                    && frame_runtime.chunk_draw_count > 0U
                );
            samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - frame_started_at
            ));
            acquire_wait_samples.push_back(frame_runtime.cpu_acquire_wait_duration);
            command_record_samples.push_back(frame_runtime.cpu_command_record_duration);
            complete_present_wait_samples.push_back(frame_runtime.cpu_complete_present_wait_duration);
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
    if (!rendererBenchmarkPresentModeSatisfied(options, runtime.present_mode)) {
        return std::unexpected("renderer benchmark required immediate presentation but negotiated another mode");
    }
    if (completed_stream_updates == 0U
        || runtime.height_tile_mesh_count != shared::HEIGHT_TILE_INTEREST_COUNT
        || !observed_stone_draw
    ) {
        return std::unexpected("renderer benchmark did not stream and draw the resident S6 height-tile scene");
    }
    double const sample_elapsed_seconds = std::chrono::duration<double>(sample_elapsed).count();
    FrameTimingSummary const timings = summarizeFrameTimings(samples);
    FrameTimingSummary const acquire_wait_timings = summarizeFrameTimings(acquire_wait_samples);
    FrameTimingSummary const command_record_timings = summarizeFrameTimings(command_record_samples);
    FrameTimingSummary const complete_present_wait_timings = summarizeFrameTimings(
        complete_present_wait_samples
    );
    double const presentation_requests_per_second = sample_elapsed_seconds > 0.0
        ? static_cast<double>(frame_count) / sample_elapsed_seconds
        : 0.0;
    bool const frame_target_met = rendererBenchmarkMeetsFrameTarget(
        presentation_requests_per_second,
        timings
    );

    return RuntimeEvidence{
        .mode = "benchmark-render",
        .failure = frame_target_met ? "" : fmt::format(
            "120 FPS frame target missed: {:.2f} presentation requests/s, p99 {:.3f} ms",
            presentation_requests_per_second,
            static_cast<double>(timings.p99.count()) / 1'000'000.0
        ),
        .ticks = frame_count,
        .expectations_passed = frame_target_met ? 1U : 0U,
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
        .deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline),
        .passed = frame_target_met,
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
            .debug_hud_enabled = options.debug_hud_enabled,
            .warmup = std::chrono::duration_cast<std::chrono::milliseconds>(options.warmup_duration),
            .sample_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(sample_elapsed),
            .presentation_requests_per_second = presentation_requests_per_second,
            .presentation_request_timings = timings,
            .acquire_wait_timings = acquire_wait_timings,
            .command_record_timings = command_record_timings,
            .complete_present_wait_timings = complete_present_wait_timings,
        },
    };
}

} // namespace acceptance
