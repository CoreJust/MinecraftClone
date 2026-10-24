#pragma once

#include <client/GenerationMapPresentation.hpp>
#include <client/WorldLoading.hpp>
#include <client/render/TextRenderer.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace client
{

enum class UiAvailability : uint8_t
{
    Available,
    Pending,
    Unavailable,
    NotApplicable,
};

enum class UiScreen : uint8_t
{
    Preparing,
    Connecting,
    Generating,
    Synchronizing,
    ReadyForStart,
    Starting,
    Playing,
    Failed,
    Cancelled,
};

template <typename T>
struct UiMetric final
{
    UiAvailability availability = UiAvailability::Unavailable;
    T value{};
    std::string units;
    std::chrono::steady_clock::time_point sampled_at{};
    uint64_t join_epoch = 0U;
    uint64_t source_identity = 0U;
};

struct UiTelemetrySnapshot final
{
    UiMetric<uint64_t> const loaded_dense_chunks{};
    UiMetric<uint64_t> const measured_rss_bytes{};
};

struct UiProgress final
{
    UiAvailability availability = UiAvailability::Unavailable;
    uint32_t completed = 0U;
    uint32_t total = 0U;
};

struct UiFrame final
{
    uint64_t const join_epoch = 0U;
    UiScreen const screen = UiScreen::Preparing;
    std::string const title;
    std::string const phase;
    std::string const progress_text;
    std::string const server_address;
    std::optional<uint64_t> const world_seed;
    std::optional<GenerationMapAnimationPhase> const illustrative_phase;
    float const illustrative_phase_progress = 0.0F;
    UiProgress const generation_progress{};
    bool const show_start_prompt = false;
    UiTelemetrySnapshot const telemetry{};
};

struct UiRect final
{
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct UiTextPlacement final
{
    float x = 0.0F;
    float y = 0.0F;
    float scale = 0.0F;
};

struct UiLoadingLayout final
{
    bool compact = false;
    float dpi_scale = 1.0F;
    float margin_pixels = 0.0F;
    float left_rail_width_pixels = 0.0F;
    bool show_progress = false;
    UiRect map_viewport{};
    UiTextPlacement title{};
    UiTextPlacement server_label{};
    UiTextPlacement server_address{};
    UiTextPlacement phase{};
    UiTextPlacement progress{};
    UiTextPlacement illustration_counter{};
    UiTextPlacement illustration_title{};
    std::array<UiTextPlacement, 2U> illustration_description{};
    std::array<UiTextPlacement, 6U> illustration_rail{};
    UiTextPlacement real_generation_status{};
    UiTextPlacement status{};
    UiTextPlacement action{};
    UiTextPlacement cancel_hint{};
    std::array<UiTextPlacement, 4U> cardinals{};
};

[[nodiscard]]
UiFrame buildUiFrame(
    uint64_t join_epoch,
    WorldLoadingStatus const& loading,
    bool connection_attempt_started,
    bool prompt_frame_ready,
    UiTelemetrySnapshot telemetry = {},
    std::optional<GenerationMapAnimationFrame> animation = {},
    std::string_view server_address = {},
    std::optional<uint64_t> world_seed = {}
);

[[nodiscard]] UiLoadingLayout layoutUiLoading(
    UiFrame const& frame,
    uint32_t width,
    uint32_t height,
    float dpi_scale = 1.0F
) noexcept;

void buildUiLoadingPresentation(
    UiFrame const& frame,
    UiLoadingLayout const& layout,
    TextRenderer& output
);

[[nodiscard]]
std::optional<uint64_t> currentProcessResidentMemoryBytes() noexcept;

} // namespace client
