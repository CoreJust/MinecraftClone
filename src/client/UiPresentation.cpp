#include <client/UiPresentation.hpp>

#include <shared/world/Chunk.hpp>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// DONT_CHECK INCLUDE_ORDER
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

namespace client
{

namespace
{

constexpr float UI_LOADING_MARGIN_PIXELS = 28.0F;
constexpr float UI_SMALL_GLYPH_SCALE = 0.5F;

constexpr TextColor UI_LIGHT{ 243.0F / 255.0F, 234.0F / 255.0F, 210.0F / 255.0F, 1.0F };
constexpr TextColor UI_OUTLINE{ 20.0F / 255.0F, 36.0F / 255.0F, 45.0F / 255.0F, 1.0F };
constexpr TextColor UI_ACCENT{ 1.0F, 224.0F / 255.0F, 128.0F / 255.0F, 1.0F };
constexpr TextColor UI_QUIET{ 194.0F / 255.0F, 208.0F / 255.0F, 207.0F / 255.0F, 1.0F };

constexpr std::array<std::string_view, 3U> EXIT_CONFIRMATION_LINES{
    "Exit the game?", "Enter or Y: confirm", "Esc: cancel",
};

constexpr std::array<std::string_view, 6U> ILLUSTRATIVE_PHASE_NAMES{
    "Formation", "Water", "Tectonics", "Ice", "Climate", "Arrival",
};

constexpr std::array<std::array<std::string_view, 2U>, 6U> ILLUSTRATIVE_PHASE_DESCRIPTIONS{{
    { "Continents drift.", "Rifts and collisions." },
    { "Seas find the coast.", "Rivers reach basins." },
    { "Continents drift.", "Collision raises ridges." },
    { "Glaciers advance.", "Cold seas freeze." },
    { "Winds and currents.", "Rain finds wet ground." },
    { "A whole world.", "One starting point." },
}};

constexpr std::array<std::string_view, 6U> ILLUSTRATIVE_PHASE_CAPTIONS{
    "Illustrative plate motion",
    "Filling coasts and basins",
    "Continental drift and uplift",
    "Advancing glaciers and sea ice",
    "Circulation and rainfall",
    "Locating the starting area",
};

constexpr std::array<std::string_view, 6U> NATIVE_ILLUSTRATIVE_PHASE_NAMES{
    "Creation", "Relief", "Water", "Ice", "Climate", "Arrival",
};

constexpr std::array<std::array<std::string_view, 2U>, 6U> NATIVE_ILLUSTRATIVE_PHASE_DESCRIPTIONS{{
    { "Molten rock cools.", "A crust begins to form." },
    { "Highlands cool first.", "A solid crust spreads." },
    { "Seas find the coast.", "Rivers reach basins." },
    { "Glaciers advance.", "Cold seas freeze." },
    { "Winds and currents.", "Rain finds wet ground." },
    { "A whole world.", "One starting point." },
}};

constexpr std::array<std::string_view, 6U> NATIVE_ILLUSTRATIVE_PHASE_CAPTIONS{
    "A cooling molten world",
    "Cooling highlands and plains",
    "Filling coasts and basins",
    "Advancing glaciers and sea ice",
    "Circulation and rainfall",
    "Locating the starting area",
};

[[nodiscard]]
uint32_t phaseIndex(GenerationMapAnimationPhase const phase, bool const native) noexcept
{
    switch (phase) {
    case GenerationMapAnimationPhase::Tectonics:
        return native ? 1U : 0U;
    case GenerationMapAnimationPhase::Creation:
        return 0U;
    case GenerationMapAnimationPhase::Water:
        return native ? 2U : 1U;
    case GenerationMapAnimationPhase::Relief:
        return native ? 1U : 2U;
    case GenerationMapAnimationPhase::Ice:
        return 3U;
    case GenerationMapAnimationPhase::Climate:
        return 4U;
    case GenerationMapAnimationPhase::Arrival:
        return 5U;
    }
    return 0U;
}

[[nodiscard]]
float sanitizedDpiScale(float const dpi_scale) noexcept
{
    return std::isfinite(dpi_scale) && dpi_scale > 0.0F
        ? std::clamp(dpi_scale, 0.25F, 8.0F)
        : 1.0F;
}

[[nodiscard]]
constexpr bool isGenerationView(UiScreen const screen) noexcept
{
    return screen == UiScreen::Generating
        || screen == UiScreen::Synchronizing
        || screen == UiScreen::ReadyForStart
        || screen == UiScreen::Starting;
}

void submitOutlined(
    TextRenderer& output,
    std::string_view const value,
    TextColor const foreground,
    UiTextPlacement const placement,
    float const dpi_scale
)
{
    float const outline = dpi_scale;
    for (auto const [dx, dy] : std::array<std::array<float, 2U>, 4U>{
        std::array<float, 2U>{ -outline, 0.0F },
        std::array<float, 2U>{ outline, 0.0F },
        std::array<float, 2U>{ 0.0F, -outline },
        std::array<float, 2U>{ 0.0F, outline },
    }) {
        static_cast<void>(output.submit(
            value,
            UI_OUTLINE,
            TextPlacement{ .position = { placement.x + dx, placement.y + dy, 0.0F }, .scale = placement.scale }
        ));
    }
    static_cast<void>(output.submit(
        value,
        foreground,
        TextPlacement{ .position = { placement.x, placement.y, 0.0F }, .scale = placement.scale }
    ));
}

void submitWrapped(
    TextRenderer& output,
    std::string_view value,
    TextColor const color,
    UiTextPlacement placement,
    float const max_width,
    float const line_advance,
    float const dpi_scale
)
{
    size_t const columns = static_cast<size_t>((std::max)(
        1.0F,
        std::floor(max_width / (16.0F * placement.scale))
    ));
    while (!value.empty()) {
        size_t count = (std::min)(columns, value.size());
        if (count < value.size()) {
            size_t const space = value.rfind(' ', count);
            if (space != std::string_view::npos && space > 0U) {
                count = space;
            }
        }
        submitOutlined(output, value.substr(0U, count), color, placement, dpi_scale);
        value.remove_prefix(count);
        while (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1U);
        }
        if (!value.empty()) {
            placement.y += line_advance;
        }
    }
}

[[nodiscard]]
std::string_view generationPhase(generation::GenerationStage const stage) noexcept
{
    switch (stage) {
    case generation::GenerationStage::Tectonics:
        return "Forming tectonic plates";
    case generation::GenerationStage::CoastalElaboration:
        return "Shaping coasts and waterways";
    case generation::GenerationStage::InitialDrainage:
        return "Mapping rivers and watersheds";
    case generation::GenerationStage::ProvisionalClimate:
        return "Establishing early climate";
    case generation::GenerationStage::TerrainErosion:
        return "Shaping terrain through erosion";
    case generation::GenerationStage::FinalClimate:
        return "Refining the climate";
    case generation::GenerationStage::FinalDrainage:
        return "Finalizing rivers and lakes";
    case generation::GenerationStage::V9Platec:
        return "Advancing native tectonic plates";
    case generation::GenerationStage::V9Terrain:
        return "Forming native terrain";
    case generation::GenerationStage::V9Climate:
        return "Solving native climate";
    case generation::GenerationStage::V9Water:
        return "Routing native water and ice";
    case generation::GenerationStage::V10Platec:
    case generation::GenerationStage::V21Platec:
        return "Advancing native tectonic plates";
    case generation::GenerationStage::V10Terrain:
    case generation::GenerationStage::V21Terrain:
        return "Forming native terrain";
    case generation::GenerationStage::V10Climate:
    case generation::GenerationStage::V21Climate:
        return "Solving native climate";
    case generation::GenerationStage::V10Water:
    case generation::GenerationStage::V21Water:
        return "Routing native water and ice";
    case generation::GenerationStage::Complete:
        return "Preparing the starting area";
    }
    return "Generating the world";
}

template <typename T>
[[nodiscard]]
UiMetric<T> currentEpochMetric(UiMetric<T> const& metric, uint64_t const join_epoch)
{
    return metric.join_epoch == join_epoch ? metric : UiMetric<T>{};
}

[[nodiscard]]
UiProgress measuredProgress(uint32_t const completed, uint32_t const total) noexcept
{
    if (total == 0U || completed > total) {
        return {};
    }
    return {
        .availability = UiAvailability::Available,
        .completed = completed,
        .total = total,
    };
}

}

UiLoadingMapSource uiLoadingMapSource(
    WorldLoadingState const state, generation::TerrainModelTag const model
) noexcept
{
    if (state != WorldLoadingState::PreparingAtlas && state != WorldLoadingState::PreparingSpawn
        && state != WorldLoadingState::ReadyForStart && state != WorldLoadingState::Starting) {
        return UiLoadingMapSource::Backdrop;
    }
    return model == generation::TerrainModelTag::V9 || model == generation::TerrainModelTag::V21
        || model == generation::TerrainModelTag::V10
        ? UiLoadingMapSource::Native : UiLoadingMapSource::Legacy;
}

UiFrame buildUiFrame(
    uint64_t const join_epoch,
    WorldLoadingStatus const& loading,
    bool const connection_attempt_started,
    bool const start_scene_ready,
    UiTelemetrySnapshot telemetry,
    std::optional<GenerationMapAnimationFrame> animation,
    std::string_view const server_address,
    std::optional<uint64_t> world_seed,
    UiLoadingMapSource const loading_map_source
)
{
    UiScreen screen = UiScreen::Preparing;
    std::string title = "Preparing to connect";
    std::string phase = "Preparing your world request";
    std::string progress_text;
    UiProgress progress{};
    bool show_start_prompt = false;

    switch (loading.state) {
    case WorldLoadingState::Idle:
        break;
    case WorldLoadingState::Creating:
        if (connection_attempt_started) {
            screen = UiScreen::Connecting;
            title = "Connecting";
            phase = "Connecting to the world service";
        }
        break;
    case WorldLoadingState::PreparingAtlas:
        screen = UiScreen::Generating;
        title = "Creating your world";
        phase = std::string{ generationPhase(loading.generation_stage) };
        progress = measuredProgress(loading.generation_completed, loading.generation_total);
        if (progress.availability == UiAvailability::Available) {
            progress_text = "Generation: " + std::to_string(progress.completed) + " / "
                + std::to_string(progress.total);
        }
        break;
    case WorldLoadingState::PreparingSpawn:
        screen = UiScreen::Synchronizing;
        title = "Synchronizing world";
        if (!loading.playable_sphere_ready) {
            phase = loading.initial_sphere_ready
                ? "Expanding the playable area"
                : "Preparing the starting area";
            uint32_t const completed = loading.initial_sphere_ready
                ? loading.playable_sphere_ready_chunks
                : loading.initial_sphere_ready_chunks;
            uint32_t const total = loading.initial_sphere_ready
                ? loading.playable_sphere_required_chunks
                : loading.initial_sphere_required_chunks;
            progress = measuredProgress(completed, total);
            if (progress.availability == UiAvailability::Available) {
                progress_text = (loading.initial_sphere_ready ? "Playable area: " : "Starting area: ")
                    + std::to_string(progress.completed) + " / " + std::to_string(progress.total)
                    + " chunks";
            }
        } else {
            phase = "Preparing the terrain view";
        }
        break;
    case WorldLoadingState::ReadyForStart:
        screen = UiScreen::ReadyForStart;
        title = "Creating your world";
        if (start_scene_ready) {
            phase = "World ready";
            show_start_prompt = true;
        } else {
            phase = "Preparing the terrain view";
        }
        break;
    case WorldLoadingState::Starting:
        screen = UiScreen::Starting;
        title = "Waiting for server";
        phase = "Confirming your start";
        break;
    case WorldLoadingState::Playing:
        screen = UiScreen::Playing;
        title = "Playing";
        phase.clear();
        break;
    case WorldLoadingState::Failed:
        screen = UiScreen::Failed;
        title = "World preparation failed";
        phase = loading.error.empty() ? "The world could not be prepared" : loading.error;
        break;
    case WorldLoadingState::Cancelled:
        screen = UiScreen::Cancelled;
        title = "World preparation cancelled";
        phase = "World preparation was cancelled";
        break;
    }

    UiTelemetrySnapshot const filtered_telemetry{
        .loaded_dense_chunks = currentEpochMetric(telemetry.loaded_dense_chunks, join_epoch),
        .measured_rss_bytes = currentEpochMetric(telemetry.measured_rss_bytes, join_epoch),
    };
    bool const generation_view = isGenerationView(screen);
    std::optional<GenerationMapAnimationPhase> illustrative_phase;
    float illustrative_phase_progress = 0.0F;
    if (generation_view && animation.has_value()) {
        illustrative_phase = animation->phase;
        illustrative_phase_progress = std::isfinite(animation->phase_progress)
            ? std::clamp(animation->phase_progress, 0.0F, 1.0F)
            : 0.0F;
    }
    return UiFrame{
        .join_epoch = join_epoch,
        .screen = screen,
        .title = std::move(title),
        .phase = std::move(phase),
        .progress_text = std::move(progress_text),
        .server_address = std::string{ server_address },
        .world_seed = generation_view ? world_seed : std::nullopt,
        .loading_map_source = loading_map_source,
        .illustrative_phase = illustrative_phase,
        .illustrative_phase_progress = illustrative_phase_progress,
        .generation_progress = progress,
        .show_start_prompt = show_start_prompt,
        .telemetry = filtered_telemetry,
    };
}

UiLoadingLayout layoutUiLoading(
    UiFrame const& frame,
    uint32_t const width,
    uint32_t const height,
    float const requested_dpi_scale
) noexcept
{
    float const dpi_scale = sanitizedDpiScale(requested_dpi_scale);
    float const logical_width = static_cast<float>(width) / dpi_scale;
    float const logical_height = static_cast<float>(height) / dpi_scale;
    bool const generation_view = isGenerationView(frame.screen);
    UiLoadingLayout layout{
        .compact = generation_view
            ? logical_width < 760.0F || logical_height < 320.0F
            : logical_width < 960.0F || logical_height < 520.0F,
        .dpi_scale = dpi_scale,
        .margin_pixels = 0.0F,
        .show_progress = frame.generation_progress.availability == UiAvailability::Available,
    };
    if (generation_view) {
        float const margin = logical_width < 640.0F ? 16.0F : UI_LOADING_MARGIN_PIXELS;
        bool const wide = logical_width >= 760.0F && logical_height >= 320.0F;
        float const title_scale = logical_width >= 360.0F ? 1.0F : 0.5F;
        float const body_scale = wide ? 1.0F : 0.5F;
        float const small_scale = UI_SMALL_GLYPH_SCALE;
        float const body_top = margin + title_scale * 16.0F
            + (wide ? (logical_height < 500.0F ? 48.0F : 72.0F) : 56.0F);
        layout.margin_pixels = margin * dpi_scale;
        layout.title = { margin * dpi_scale, margin * dpi_scale, title_scale * dpi_scale };
        if (frame.world_seed.has_value()) {
            layout.phase = {
                margin * dpi_scale,
                (margin + title_scale * 16.0F + 12.0F) * dpi_scale,
                small_scale * dpi_scale,
            };
        }

        layout.illustration_counter = {
            margin * dpi_scale,
            body_top * dpi_scale,
            small_scale * dpi_scale,
        };
        layout.illustration_title = {
            margin * dpi_scale,
            (body_top + 24.0F) * dpi_scale,
            title_scale * dpi_scale,
        };
        float const description_y = body_top + 28.0F + title_scale * 16.0F;
        float const description_advance = body_scale * 16.0F + 6.0F;
        for (size_t index = 0U; index < layout.illustration_description.size(); ++index) {
            layout.illustration_description[index] = {
                margin * dpi_scale,
                (description_y + static_cast<float>(index) * description_advance) * dpi_scale,
                body_scale * dpi_scale,
            };
        }

        if (logical_width >= 1100.0F && logical_height >= 560.0F) {
            float const rail_y = (std::max)(
                description_y + body_scale * 38.0F + 28.0F,
                logical_height - margin - 110.0F - 6.0F * (16.0F + 12.0F)
            );
            for (size_t index = 0U; index < layout.illustration_rail.size(); ++index) {
                layout.illustration_rail[index] = {
                    margin * dpi_scale,
                    (rail_y + static_cast<float>(index) * 28.0F) * dpi_scale,
                    small_scale * dpi_scale,
                };
            }
        }

        float const side_width = wide ? 300.0F : 260.0F;
        float const field_x = wide ? margin + side_width + 28.0F : margin;
        float const field_width = (std::max)(0.0F, logical_width - field_x - margin);
        float const field_y = wide ? margin : body_top + 116.0F;
        float const field_bottom = logical_height - margin - (wide ? 0.0F : 120.0F);
        float const square_size = std::floor((std::max)(0.0F, (std::min)(
            field_width,
            (std::max)(0.0F, field_bottom - field_y)
        )));
        layout.map_viewport = {
            .x = std::round(field_x + (field_width - square_size) * 0.5F) * dpi_scale,
            .y = std::round(field_y + ((field_bottom - field_y) - square_size) * 0.5F) * dpi_scale,
            .width = square_size * dpi_scale,
            .height = square_size * dpi_scale,
        };
        float const status_width = wide
            ? (logical_width >= 1100.0F
                ? side_width
                : layout.map_viewport.x / dpi_scale - 2.0F * margin)
            : logical_width - 2.0F * margin;
        layout.left_rail_width_pixels = (std::max)(0.0F, status_width) * dpi_scale;

        layout.real_generation_status = {
            margin * dpi_scale,
            (logical_height - margin - 112.0F) * dpi_scale,
            small_scale * dpi_scale,
        };
        if (frame.show_start_prompt) {
            layout.status = {
                margin * dpi_scale,
                (logical_height - margin - 72.0F) * dpi_scale,
                1.0F * dpi_scale,
            };
            layout.action = {
                margin * dpi_scale,
                (logical_height - margin - 32.0F) * dpi_scale,
                1.0F * dpi_scale,
            };
            layout.cancel_hint = {
                (logical_width - margin - 88.0F) * dpi_scale,
                (logical_height - margin - 16.0F) * dpi_scale,
                small_scale * dpi_scale,
            };
        } else {
            layout.status = {
                margin * dpi_scale,
                (logical_height - margin - 68.0F) * dpi_scale,
                small_scale * dpi_scale,
            };
            layout.action = {
                margin * dpi_scale,
                (logical_height - margin - 44.0F) * dpi_scale,
                small_scale * dpi_scale,
            };
        }

        float const cardinal_scale = (logical_height < 500.0F || logical_width < 1080.0F ? 0.5F : 1.0F)
            * dpi_scale;
        float const center_x = layout.map_viewport.x + layout.map_viewport.width * 0.5F;
        float const center_y = layout.map_viewport.y + layout.map_viewport.height * 0.5F;
        layout.cardinals = {
            UiTextPlacement{
                center_x - 8.0F * cardinal_scale,
                layout.map_viewport.y + 6.0F * dpi_scale,
                cardinal_scale,
            },
            UiTextPlacement{
                layout.map_viewport.x + layout.map_viewport.width - 8.0F * cardinal_scale - 7.0F * dpi_scale,
                center_y - 8.0F * cardinal_scale,
                cardinal_scale,
            },
            UiTextPlacement{
                center_x - 8.0F * cardinal_scale,
                layout.map_viewport.y + layout.map_viewport.height - 16.0F * cardinal_scale - 6.0F * dpi_scale,
                cardinal_scale,
            },
            UiTextPlacement{
                layout.map_viewport.x + 7.0F * dpi_scale,
                center_y - 8.0F * cardinal_scale,
                cardinal_scale,
            },
        };
        layout.progress = layout.real_generation_status;
        return layout;
    }

    bool const compact = logical_width < 960.0F || logical_height < 520.0F;
    float const margin = compact ? 28.0F : 64.0F;
    float const title_scale = compact ? 1.0F : 1.5F;
    float const top = compact ? 28.0F : std::round(logical_height * 0.13F);
    float const title_y = top + (compact ? 24.0F : 32.0F);
    float const server_y = title_y + title_scale * 32.0F + (compact ? 20.0F : 28.0F);
    float const address_y = server_y + 24.0F;
    float const hint_y = logical_height - (compact ? 72.0F : 84.0F);
    layout.compact = compact;
    layout.margin_pixels = margin * dpi_scale;
    layout.title = { margin * dpi_scale, title_y * dpi_scale, title_scale * dpi_scale };
    layout.server_label = {
        margin * dpi_scale,
        server_y * dpi_scale,
        UI_SMALL_GLYPH_SCALE * dpi_scale,
    };
    layout.server_address = {
        margin * dpi_scale,
        address_y * dpi_scale,
        1.0F * dpi_scale,
    };
    layout.phase = {
        margin * dpi_scale,
        (address_y + 40.0F) * dpi_scale,
        UI_SMALL_GLYPH_SCALE * dpi_scale,
    };
    layout.action = {
        margin * dpi_scale,
        hint_y * dpi_scale,
        1.0F * dpi_scale,
    };
    layout.cancel_hint = {
        (margin + 13.0F * 16.0F + (compact ? 32.0F : 48.0F)) * dpi_scale,
        hint_y * dpi_scale,
        1.0F * dpi_scale,
    };
    return layout;
}

void buildUiLoadingPresentation(
    UiFrame const& frame,
    UiLoadingLayout const& layout,
    TextRenderer& output
)
{
    output.clear();
    bool const generation_view = isGenerationView(frame.screen);
    submitOutlined(output, frame.title, UI_LIGHT, layout.title, layout.dpi_scale);

    if (!generation_view) {
        if (!frame.server_address.empty()) {
            submitOutlined(output, "SERVER", UI_QUIET, layout.server_label, layout.dpi_scale);
            submitOutlined(output, frame.server_address, UI_ACCENT, layout.server_address, layout.dpi_scale);
        }
        if (frame.screen == UiScreen::Cancelled) {
            submitOutlined(output, "No connection is running", UI_QUIET, layout.phase, layout.dpi_scale);
        } else if (frame.screen == UiScreen::Failed) {
            submitOutlined(output, frame.phase, UI_QUIET, layout.phase, layout.dpi_scale);
        }
        if (frame.screen == UiScreen::Preparing || frame.screen == UiScreen::Connecting
            || frame.screen == UiScreen::Synchronizing || frame.screen == UiScreen::Starting) {
            submitOutlined(output, "ESC  Cancel", UI_LIGHT, layout.action, layout.dpi_scale);
        }
        return;
    }

    if (frame.world_seed.has_value()) {
        submitOutlined(
            output,
            "Seed " + std::to_string(*frame.world_seed) + " / Illustrative history",
            UI_QUIET,
            layout.phase,
            layout.dpi_scale
        );
    }

    bool const native = frame.loading_map_source == UiLoadingMapSource::Native;
    auto const& phase_names = native ? NATIVE_ILLUSTRATIVE_PHASE_NAMES : ILLUSTRATIVE_PHASE_NAMES;
    auto const& phase_descriptions = native
        ? NATIVE_ILLUSTRATIVE_PHASE_DESCRIPTIONS : ILLUSTRATIVE_PHASE_DESCRIPTIONS;
    auto const& phase_captions = native ? NATIVE_ILLUSTRATIVE_PHASE_CAPTIONS : ILLUSTRATIVE_PHASE_CAPTIONS;
    uint32_t const phase_index = frame.illustrative_phase.has_value()
        ? (std::min)(phaseIndex(*frame.illustrative_phase, native), static_cast<uint32_t>(phase_names.size() - 1U))
        : 0U;
    if (frame.illustrative_phase.has_value()) {
        std::array<char, 8U> counter{
            static_cast<char>('0' + (phase_index + 1U) / 10U),
            static_cast<char>('0' + (phase_index + 1U) % 10U),
            ' ', '/', ' ', '0', '6', '\0',
        };
        submitOutlined(
            output,
            std::string_view{ counter.data(), counter.size() - 1U },
            UI_QUIET,
            layout.illustration_counter,
            layout.dpi_scale
        );
        submitOutlined(
            output,
            phase_names[phase_index],
            UI_ACCENT,
            layout.illustration_title,
            layout.dpi_scale
        );
        for (size_t index = 0U; index < phase_descriptions[phase_index].size(); ++index) {
            submitOutlined(
                output,
                *frame.illustrative_phase == GenerationMapAnimationPhase::Creation
                    ? std::array<std::string_view, 2U>{"Molten rock cools.", "A crust begins to form."}[index]
                    : phase_descriptions[phase_index][index],
                UI_LIGHT,
                layout.illustration_description[index],
                layout.dpi_scale
            );
        }
        if (layout.illustration_rail[0].scale > 0.0F) {
            for (size_t index = 0U; index < phase_names.size(); ++index) {
                std::string item;
                TextColor color = UI_QUIET;
                if (index < phase_index) {
                    item = "+ ";
                    color = UI_LIGHT;
                } else if (index == phase_index) {
                    item = "> ";
                    color = UI_ACCENT;
                } else {
                    item = "  ";
                }
                item.append(phase_names[index]);
                submitOutlined(output, item, color, layout.illustration_rail[index], layout.dpi_scale);
            }
        }
        if (frame.illustrative_phase.has_value() && !frame.show_start_prompt) {
            submitOutlined(
                output,
                *frame.illustrative_phase == GenerationMapAnimationPhase::Creation
                    ? "Cooling a young world" : phase_captions[phase_index],
                UI_QUIET,
                layout.status,
                layout.dpi_scale
            );
        }
    }

    static constexpr std::array<std::string_view, 4U> cardinals{ "N", "E", "S", "W" };
    if (layout.map_viewport.width > 0.0F && layout.map_viewport.height > 0.0F) {
        for (size_t index = 0U; index < cardinals.size(); ++index) {
            submitOutlined(output, cardinals[index], UI_LIGHT, layout.cardinals[index], layout.dpi_scale);
        }
    }

    std::string actual_status = "Server: " + frame.phase;
    if (!frame.progress_text.empty()) {
        actual_status.append("  ");
        actual_status.append(frame.progress_text);
    }
    submitWrapped(
        output,
        actual_status,
        UI_QUIET,
        layout.real_generation_status,
        layout.left_rail_width_pixels,
        20.0F * layout.dpi_scale,
        layout.dpi_scale
    );
    if (frame.show_start_prompt) {
        submitOutlined(output, "World ready", UI_LIGHT, layout.status, layout.dpi_scale);
        submitOutlined(output, "Press any key to start", UI_ACCENT, layout.action, layout.dpi_scale);
        submitOutlined(output, "Esc  Cancel", UI_QUIET, layout.cancel_hint, layout.dpi_scale);
    } else if (!frame.illustrative_phase.has_value()) {
        submitOutlined(output, "World data unavailable", UI_ACCENT, layout.status, layout.dpi_scale);
        submitOutlined(output, "Esc  Cancel", UI_ACCENT, layout.action, layout.dpi_scale);
    } else if (isGenerationView(frame.screen)) {
        submitOutlined(output, "Esc  Cancel", UI_ACCENT, layout.action, layout.dpi_scale);
    }
}

UiExitConfirmationLayout layoutUiExitConfirmation(
    uint32_t const width,
    uint32_t const height,
    float const requested_dpi_scale
) noexcept
{
    UiExitConfirmationLayout layout;
    if (width == 0U || height == 0U) {
        return layout;
    }

    float const dpi_scale = sanitizedDpiScale(requested_dpi_scale);
    size_t const longest_line = std::max_element(
        EXIT_CONFIRMATION_LINES.begin(), EXIT_CONFIRMATION_LINES.end(),
        [](std::string_view const left, std::string_view const right) { return left.size() < right.size(); }
    )->size();
    float const width_scale = static_cast<float>(width) / (static_cast<float>(longest_line) * 16.0F);
    float const line_count = static_cast<float>(EXIT_CONFIRMATION_LINES.size());
    float const height_scale = static_cast<float>(height) / (line_count * 40.0F);
    layout.scale = std::min({ dpi_scale, width_scale, height_scale });
    float const line_advance = 40.0F * layout.scale;
    float const first_y = (static_cast<float>(height) - line_advance * line_count) * 0.5F;
    for (size_t index = 0U; index < EXIT_CONFIRMATION_LINES.size(); ++index) {
        float const line_width = static_cast<float>(EXIT_CONFIRMATION_LINES[index].size())
            * 16.0F * layout.scale;
        layout.lines[index] = {
            .x = (static_cast<float>(width) - line_width) * 0.5F,
            .y = first_y + static_cast<float>(index) * line_advance,
            .scale = layout.scale,
        };
    }
    return layout;
}

std::string_view uiBlockDisplayName(shared::Block const block) noexcept
{
    switch (block) {
    case shared::Block::Air: return "Air";
    case shared::Block::Stone: return "Stone";
    case shared::Block::Grass: return "Grass";
    case shared::Block::Dirt: return "Dirt";
    case shared::Block::Bedrock: return "Bedrock";
    case shared::Block::Snow: return "Snow";
    case shared::Block::Ice: return "Ice";
    case shared::Block::Sand: return "Sand";
    }
    return "Unknown";
}

std::string uiTargetBlockLabel(shared::Block const block)
{
    std::string label{uiBlockDisplayName(block)};
    label += " (ID ";
    label += std::to_string(static_cast<uint8_t>(block));
    label += ')';
    return label;
}

std::string uiTargetHudEntry(std::optional<shared::BlockTarget> const& target)
{
    if (!target) {
        return "Target: none";
    }

    std::string entry{"Target: "};
    entry += std::to_string(target->cell.x);
    entry += ',';
    entry += std::to_string(target->cell.y);
    entry += ',';
    entry += std::to_string(target->cell.z);
    entry += ' ';
    entry += uiBlockDisplayName(target->block);
    entry += " (";
    entry += std::to_string(static_cast<uint8_t>(target->block));
    entry += ')';
    return entry;
}

std::optional<shared::Block> uiGameplayTargetBlock(
    bool const gameplay_ui,
    std::optional<shared::BlockTarget> const& resolved_target
) noexcept
{
    if (!gameplay_ui || !resolved_target) {
        return std::nullopt;
    }
    return resolved_target->block;
}

void buildUiExitConfirmationPresentation(
    UiExitConfirmationLayout const& layout,
    TextRenderer& output
)
{
    if (layout.scale <= 0.0F) {
        return;
    }
    for (size_t index = 0U; index < EXIT_CONFIRMATION_LINES.size(); ++index) {
        TextColor const color = index == 0U ? UI_ACCENT : UI_LIGHT;
        submitOutlined(output, EXIT_CONFIRMATION_LINES[index], color, layout.lines[index], layout.scale);
    }
}

std::optional<uint64_t> currentProcessResidentMemoryBytes() noexcept
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        sizeof(counters)
    ) == 0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(counters.WorkingSetSize);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count)
        != KERN_SUCCESS) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(info.resident_size);
#elif defined(__linux__)
    std::ifstream statm{ "/proc/self/statm" };
    uint64_t total_pages = 0U;
    uint64_t resident_pages = 0U;
    long const page_size = sysconf(_SC_PAGESIZE);
    if (!(statm >> total_pages >> resident_pages) || page_size <= 0
        || resident_pages > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(page_size)) {
        return std::nullopt;
    }
    return resident_pages * static_cast<uint64_t>(page_size);
#else
    return std::nullopt;
#endif
}

} // namespace client
