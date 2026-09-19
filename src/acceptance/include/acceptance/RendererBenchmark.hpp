#pragma once

#include <client/render/VulkanRenderer.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <acceptance/EvidenceJson.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace acceptance {

struct RendererBenchmarkOptions final {
    uint32_t requested_width{ 1'920 };
    uint32_t requested_height{ 1'080 };
    uint32_t max_resize_polls{ 60 };
    uint64_t max_frames{ 1'000'000 };
    std::chrono::seconds warmup_duration{ 3 };
    std::chrono::seconds sample_duration{ 10 };
    std::chrono::seconds deadline{ 20 };
    bool require_immediate_present_mode{ false };
    bool require_validation{ false };
    bool debug_hud_enabled{ false };
};

enum class RendererBenchmarkStreamDirection : uint8_t {
    PositiveX,
    PositiveY,
    NegativeX,
    NegativeY,
};

struct RendererBenchmarkStreamUpdate final {
    shared::HeightTileCoordinate center{};
    RendererBenchmarkStreamDirection direction{};
    std::vector<shared::HeightTileCoordinate> removals;
    std::vector<shared::HeightTileCoordinate> additions;
};

[[nodiscard]] bool rendererBenchmarkMeetsFrameTarget(
    double presentation_requests_per_second,
    FrameTimingSummary const& timings
) noexcept;

[[nodiscard]]
RendererBenchmarkStreamUpdate makeRendererBenchmarkStreamUpdate(
    shared::HeightTileCoordinate center,
    uint64_t completed_updates
);

[[nodiscard]]
client::VulkanRendererOptions makeRendererBenchmarkVulkanOptions(
    RendererBenchmarkOptions const& options
) noexcept;

[[nodiscard]]
bool rendererBenchmarkPresentModeSatisfied(
    RendererBenchmarkOptions const& options,
    client::RendererPresentMode negotiated_mode
) noexcept;

[[nodiscard]]
FrameTimingSummary summarizeFrameTimings(
    std::span<std::chrono::nanoseconds const> samples
);

[[nodiscard]]
std::expected<RuntimeEvidence, std::string> runRendererBenchmark(
    RendererBenchmarkOptions const& options = { }
);

} // namespace acceptance
