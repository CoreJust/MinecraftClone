#include <client/UiPresentation.hpp>
#include <client/WorldLoading.hpp>
#include <shared/world/Chunk.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TextBounds final
{
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
};

[[nodiscard]] TextBounds textBounds(client::TextLabel const& label)
{
    float const width = static_cast<float>(label.value.size()) * 16.0F * label.placement.scale;
    float const height = 32.0F * label.placement.scale;
    return {
        .left = label.placement.position[0U],
        .top = label.placement.position[1U],
        .right = label.placement.position[0U] + width,
        .bottom = label.placement.position[1U] + height,
    };
}

[[nodiscard]] bool overlaps(TextBounds const& first, TextBounds const& second) noexcept
{
    return first.left < second.right && second.left < first.right
        && first.top < second.bottom && second.top < first.bottom;
}

} // namespace

TEST(UiPresentationTest, CreatingStaysPreparingUntilConnectionIsObservable)
{
    client::WorldLoadingStatus const status{
        .state = client::WorldLoadingState::Creating,
    };

    client::UiFrame const preparing = client::buildUiFrame(7U, status, false, false);
    EXPECT_EQ(preparing.screen, client::UiScreen::Preparing);
    EXPECT_EQ(preparing.title, "Preparing to connect");
    EXPECT_EQ(preparing.generation_progress.availability, client::UiAvailability::Unavailable);

    client::UiFrame const connecting = client::buildUiFrame(7U, status, true, false);
    EXPECT_EQ(connecting.screen, client::UiScreen::Connecting);
    EXPECT_EQ(connecting.title, "Connecting");
    EXPECT_EQ(connecting.generation_progress.availability, client::UiAvailability::Unavailable);
}

TEST(UiPresentationTest, GenerationProgressNeedsARealDenominator)
{
    client::WorldLoadingStatus status{
        .state = client::WorldLoadingState::PreparingAtlas,
        .generation_completed = 0U,
        .generation_total = 0U,
    };

    client::UiFrame const unknown = client::buildUiFrame(3U, status, false, false);
    EXPECT_EQ(unknown.screen, client::UiScreen::Generating);
    EXPECT_EQ(unknown.generation_progress.availability, client::UiAvailability::Unavailable);

    status.generation_completed = 4U;
    status.generation_total = 9U;
    client::UiFrame const measured = client::buildUiFrame(3U, status, false, false);
    EXPECT_EQ(measured.generation_progress.availability, client::UiAvailability::Available);
    EXPECT_EQ(measured.generation_progress.completed, 4U);
    EXPECT_EQ(measured.generation_progress.total, 9U);
}

TEST(UiPresentationTest, StartPromptRequiresTheExistingPromptFrameGate)
{
    client::WorldLoadingStatus const ready{
        .state = client::WorldLoadingState::ReadyForStart,
    };

    client::UiFrame const waiting_for_scene = client::buildUiFrame(2U, ready, false, false);
    EXPECT_EQ(waiting_for_scene.screen, client::UiScreen::ReadyForStart);
    EXPECT_FALSE(waiting_for_scene.show_start_prompt);
    EXPECT_EQ(waiting_for_scene.phase, "Preparing the terrain view");

    client::UiFrame const prompt = client::buildUiFrame(2U, ready, false, true);
    EXPECT_TRUE(prompt.show_start_prompt);
    EXPECT_EQ(prompt.title, "Creating your world");
    EXPECT_EQ(prompt.phase, "World ready");
}

TEST(UiPresentationTest, StaleTelemetryIsUnavailableForANewJoinEpoch)
{
    client::WorldLoadingStatus const status{
        .state = client::WorldLoadingState::Playing,
    };
    client::UiTelemetrySnapshot telemetry{
        .loaded_dense_chunks = {
            .availability = client::UiAvailability::Available,
            .value = 12U,
            .join_epoch = 7U,
            .source_identity = 23U,
        },
    };

    client::UiFrame const frame = client::buildUiFrame(8U, status, false, false, telemetry);
    EXPECT_EQ(frame.screen, client::UiScreen::Playing);
    EXPECT_EQ(frame.telemetry.loaded_dense_chunks.availability, client::UiAvailability::Unavailable);
    EXPECT_EQ(frame.telemetry.loaded_dense_chunks.value, 0U);
}

TEST(UiPresentationTest, ExitConfirmationFitsAndShowsConfirmAndCancelActions)
{
    client::UiExitConfirmationLayout const layout = client::layoutUiExitConfirmation(640U, 360U, 2.0F);
    ASSERT_GT(layout.scale, 0.0F);

    client::TextRenderer text;
    client::buildUiExitConfirmationPresentation(layout, text);
    auto const labels = text.labels();
    EXPECT_TRUE(std::any_of(labels.begin(), labels.end(), [](client::TextLabel const& label) {
        return label.value == "Exit the game?";
    }));
    EXPECT_TRUE(std::any_of(labels.begin(), labels.end(), [](client::TextLabel const& label) {
        return label.value == "Enter or Y: confirm";
    }));
    EXPECT_TRUE(std::any_of(labels.begin(), labels.end(), [](client::TextLabel const& label) {
        return label.value == "Esc: cancel";
    }));
    for (client::UiTextPlacement const& line : layout.lines) {
        EXPECT_GE(line.x, 0.0F);
        EXPECT_GE(line.y, 0.0F);
        EXPECT_LE(line.y + 40.0F * layout.scale, 360.0F);
    }
}

TEST(UiPresentationTest, ExitConfirmationDoesNotEmitTextForAnEmptyViewport)
{
    client::UiExitConfirmationLayout const layout = client::layoutUiExitConfirmation(0U, 0U);
    client::TextRenderer text;
    client::buildUiExitConfirmationPresentation(layout, text);
    EXPECT_TRUE(text.labels().empty());
}

TEST(UiPresentationTest, ResolvedTargetFormatsTheCombinedGameplayHudEntry)
{
    std::optional<shared::BlockTarget> const target{shared::BlockTarget{
        .cell = { .x = 12, .y = 34, .z = 56 },
        .block = shared::Block::Dirt,
    }};
    EXPECT_EQ(client::uiTargetHudEntry(target), "Target: 12,34,56 Dirt (3)");
    EXPECT_EQ(client::uiTargetHudEntry(std::nullopt), "Target: none");
}

TEST(UiPresentationTest, LoadingTextFitsTheAvailableLandscapeViewport)
{
    client::WorldLoadingStatus const status{
        .state = client::WorldLoadingState::PreparingAtlas,
        .generation_completed = 12U,
        .generation_total = 25U,
    };
    client::UiFrame const frame = client::buildUiFrame(4U, status, false, false);

    client::UiLoadingLayout const layout = client::layoutUiLoading(frame, 800U, 600U);
    EXPECT_TRUE(layout.show_progress);
    EXPECT_GT(layout.title.scale, 0.0F);
    EXPECT_GE(layout.title.x, layout.margin_pixels);
    EXPECT_LE(
        layout.title.x + static_cast<float>(frame.title.size()) * 16.0F * layout.title.scale,
        800.0F - layout.margin_pixels
    );
    EXPECT_LE(
        layout.phase.y + 32.0F * layout.phase.scale,
        600.0F - layout.margin_pixels
    );
    EXPECT_LE(
        layout.progress.x + static_cast<float>(frame.progress_text.size()) * 16.0F * layout.progress.scale,
        800.0F - layout.margin_pixels
    );
}

TEST(UiPresentationTest, NativeLoadingStatusAvoidsStageDetailOnAndroidAndSmallViewports)
{
    client::WorldLoadingStatus const loading{
        .state = client::WorldLoadingState::PreparingAtlas,
        .generation_stage = generation::GenerationStage::V10Platec,
        .generation_completed = 1'234'567U,
        .generation_total = 2'400'000U,
    };
    client::GenerationMapAnimationFrame const animation{
        .phase = client::GenerationMapAnimationPhase::Creation,
    };
    client::UiFrame const frame = client::buildUiFrame(
        1U,
        loading,
        false,
        false,
        {},
        animation,
        "localhost:4242",
        42U,
        client::UiLoadingMapSource::Native
    );
    std::string const actual_status = "Server: " + frame.phase + "  " + frame.progress_text;
    struct Viewport final {
        uint32_t width;
        uint32_t height;
        uint32_t density_dpi;
    };
    std::array<Viewport, 2U> const viewports{{
        { 2'400U, 1'080U, 420U },
        { 640U, 480U, 160U },
    }};

    for (Viewport const viewport : viewports) {
        uint32_t const width = viewport.width;
        uint32_t const height = viewport.height;
        float const dpi_scale = static_cast<float>(viewport.density_dpi) / 160.0F;
        client::UiLoadingLayout const layout = client::layoutUiLoading(frame, width, height, dpi_scale);
        client::TextRenderer text;
        client::buildUiLoadingPresentation(frame, layout, text);
        TextBounds const map_bounds{
            .left = layout.map_viewport.x,
            .top = layout.map_viewport.y,
            .right = layout.map_viewport.x + layout.map_viewport.width,
            .bottom = layout.map_viewport.y + layout.map_viewport.height,
        };

        std::vector<TextBounds> status_bounds;
        std::vector<TextBounds> detail_bounds;
        bool has_server_prefix = false;
        bool has_progress_text = false;
        for (client::TextLabel const& label : text.labels()) {
            TextBounds const bounds = textBounds(label);
            EXPECT_GE(bounds.left, 0.0F) << label.value;
            EXPECT_GE(bounds.top, 0.0F) << label.value;
            EXPECT_LE(bounds.right, static_cast<float>(width)) << label.value;
            EXPECT_LE(bounds.bottom, static_cast<float>(height)) << label.value;
            if (label.value.size() > 1U && actual_status.find(label.value) != std::string::npos) {
                status_bounds.push_back(bounds);
                has_server_prefix = has_server_prefix || label.value.starts_with("Server:");
                has_progress_text = has_progress_text || label.value.find("Generation:") != std::string::npos;
            }
            if (label.value == "Cooling a young world") {
                detail_bounds.push_back(bounds);
            }
        }

        ASSERT_FALSE(status_bounds.empty());
        ASSERT_FALSE(detail_bounds.empty());
        EXPECT_TRUE(has_server_prefix);
        EXPECT_TRUE(has_progress_text);
        for (TextBounds const& status_bounds_item : status_bounds) {
            EXPECT_FALSE(layout.map_viewport.width > 0.0F && layout.map_viewport.height > 0.0F
                && overlaps(status_bounds_item, map_bounds));
            for (TextBounds const& detail_bounds_item : detail_bounds) {
                EXPECT_FALSE(overlaps(status_bounds_item, detail_bounds_item));
            }
        }
    }
}

TEST(UiPresentationTest, NativeLoadingStatusPreservesNormalDesktopPlacement)
{
    client::WorldLoadingStatus const loading{
        .state = client::WorldLoadingState::PreparingAtlas,
        .generation_stage = generation::GenerationStage::V10Platec,
        .generation_completed = 1'234'567U,
        .generation_total = 2'400'000U,
    };
    client::GenerationMapAnimationFrame const animation{
        .phase = client::GenerationMapAnimationPhase::Creation,
    };
    client::UiFrame const frame = client::buildUiFrame(
        1U, loading, false, false, {}, animation, "localhost:4242", 42U,
        client::UiLoadingMapSource::Native
    );
    client::UiLoadingLayout const layout = client::layoutUiLoading(frame, 1'920U, 1'080U);
    EXPECT_FLOAT_EQ(layout.left_rail_width_pixels, 300.0F);
    EXPECT_FLOAT_EQ(layout.real_generation_status.y, 940.0F);
    EXPECT_FLOAT_EQ(layout.status.y, 984.0F);

    client::TextRenderer text;
    client::buildUiLoadingPresentation(frame, layout, text);
    std::string const actual_status = "Server: " + frame.phase + "  " + frame.progress_text;
    std::vector<TextBounds> status_bounds;
    std::vector<TextBounds> detail_bounds;
    for (client::TextLabel const& label : text.labels()) {
        TextBounds const bounds = textBounds(label);
        if (actual_status.find(label.value) != std::string::npos) {
            status_bounds.push_back(bounds);
            EXPECT_GE(bounds.left, 0.0F) << label.value;
            EXPECT_GE(bounds.top, 0.0F) << label.value;
            EXPECT_LE(bounds.right, 1'920.0F) << label.value;
            EXPECT_LE(bounds.bottom, 1'080.0F) << label.value;
        }
        if (label.value == "Cooling a young world") {
            detail_bounds.push_back(bounds);
        }
    }

    ASSERT_FALSE(status_bounds.empty());
    ASSERT_FALSE(detail_bounds.empty());
    for (TextBounds const& status_bounds_item : status_bounds) {
        for (TextBounds const& detail_bounds_item : detail_bounds) {
            EXPECT_FALSE(overlaps(status_bounds_item, detail_bounds_item));
        }
    }
}

TEST(UiPresentationTest, SynchronizingAndStartingKeepTheSameMapViewportAndRealServerStatus)
{
    client::GenerationMapAnimationFrame const animation{
        .phase = client::GenerationMapAnimationPhase::Relief,
        .phase_progress = 0.75F,
    };
    client::WorldLoadingStatus synchronizing_status{
        .state = client::WorldLoadingState::PreparingSpawn,
        .playable_sphere_ready_chunks = 3U,
        .playable_sphere_required_chunks = 7U,
        .initial_sphere_ready = true,
    };
    client::UiFrame const synchronizing = client::buildUiFrame(
        9U, synchronizing_status, true, false, {}, animation, "localhost:4242", 17U);
    client::UiLoadingLayout const synchronizing_layout = client::layoutUiLoading(
        synchronizing, 1'280U, 676U);

    client::WorldLoadingStatus starting_status{
        .state = client::WorldLoadingState::Starting,
        .start_pending = true,
    };
    client::UiFrame const starting = client::buildUiFrame(
        9U, starting_status, true, false, {}, animation, "localhost:4242", 17U);
    client::UiLoadingLayout const starting_layout = client::layoutUiLoading(
        starting, 1'280U, 676U);

    EXPECT_EQ(synchronizing.screen, client::UiScreen::Synchronizing);
    EXPECT_EQ(starting.screen, client::UiScreen::Starting);
    EXPECT_EQ(synchronizing.phase, "Expanding the playable area");
    EXPECT_EQ(starting.phase, "Confirming your start");
    EXPECT_EQ(synchronizing.generation_progress.completed, 3U);
    EXPECT_EQ(synchronizing.generation_progress.total, 7U);
    EXPECT_EQ(synchronizing.illustrative_phase, client::GenerationMapAnimationPhase::Relief);
    EXPECT_EQ(starting.illustrative_phase, client::GenerationMapAnimationPhase::Relief);
    EXPECT_EQ(synchronizing_layout.map_viewport.x, starting_layout.map_viewport.x);
    EXPECT_EQ(synchronizing_layout.map_viewport.y, starting_layout.map_viewport.y);
    EXPECT_GT(synchronizing_layout.map_viewport.width, 0.0F);
    EXPECT_GT(synchronizing_layout.map_viewport.height, 0.0F);
    EXPECT_FALSE(synchronizing.show_start_prompt);
    EXPECT_FALSE(starting.show_start_prompt);

    client::TextRenderer text;
    client::buildUiLoadingPresentation(synchronizing, synchronizing_layout, text);
    EXPECT_TRUE(std::any_of(text.labels().begin(), text.labels().end(), [](auto const& label) {
        return label.value.find("Server: Expanding the playable") != std::string::npos;
    }));
    client::buildUiLoadingPresentation(starting, starting_layout, text);
    EXPECT_TRUE(std::any_of(text.labels().begin(), text.labels().end(), [](auto const& label) {
        return label.value == "Server: Confirming your start";
    }));
}

TEST(UiPresentationTest, ReadyArrivalKeepsPromptWithoutTheIllustrativeArrivalCaption)
{
    client::WorldLoadingStatus const ready{
        .state = client::WorldLoadingState::ReadyForStart,
    };
    client::GenerationMapAnimationFrame const arrival{
        .phase = client::GenerationMapAnimationPhase::Arrival,
        .phase_progress = 1.0F,
    };
    client::UiFrame const frame = client::buildUiFrame(
        4U, ready, false, true, {}, arrival, "localhost:4242", 42U);
    client::UiLoadingLayout const layout = client::layoutUiLoading(frame, 1'280U, 676U);
    client::TextRenderer text;

    client::buildUiLoadingPresentation(frame, layout, text);

    EXPECT_TRUE(frame.show_start_prompt);
    EXPECT_TRUE(std::any_of(text.labels().begin(), text.labels().end(), [](auto const& label) {
        return label.value == "World ready";
    }));
    EXPECT_TRUE(std::any_of(text.labels().begin(), text.labels().end(), [](auto const& label) {
        return label.value == "Press any key to start";
    }));
    EXPECT_FALSE(std::any_of(text.labels().begin(), text.labels().end(), [](auto const& label) {
        return label.value == "Locating the starting area";
    }));
}

TEST(UiPresentationTest, ReadySceneCanStartWhileOptionalFormationContinues)
{
    client::WorldLoadingStatus const ready{.state = client::WorldLoadingState::ReadyForStart};
    for (auto const phase : {client::GenerationMapAnimationPhase::Creation,
             client::GenerationMapAnimationPhase::Tectonics, client::GenerationMapAnimationPhase::Relief,
             client::GenerationMapAnimationPhase::Water, client::GenerationMapAnimationPhase::Ice,
             client::GenerationMapAnimationPhase::Climate, client::GenerationMapAnimationPhase::Arrival}) {
        client::GenerationMapAnimationFrame const formation{.phase = phase};
        auto const frame = client::buildUiFrame(2U, ready, false, true, {}, formation);
        EXPECT_EQ(frame.screen, client::UiScreen::ReadyForStart);
        EXPECT_EQ(frame.phase, "World ready");
        EXPECT_TRUE(frame.show_start_prompt);
        EXPECT_EQ(frame.illustrative_phase, phase);
        auto const waiting_for_scene = client::buildUiFrame(2U, ready, false, false, {}, formation);
        EXPECT_FALSE(waiting_for_scene.show_start_prompt);
        EXPECT_EQ(waiting_for_scene.phase, "Preparing the terrain view");
    }
}

TEST(UiPresentationTest, LoadingSourceKeepsIntroBackdropAndSelectsNativeBeforeFieldsArrive)
{
    for (auto const model : {generation::TerrainModelTag::R6, generation::TerrainModelTag::V9,
             generation::TerrainModelTag::V21, generation::TerrainModelTag::V10}) {
        for (auto const state : {client::WorldLoadingState::Idle, client::WorldLoadingState::Creating,
                 client::WorldLoadingState::Playing, client::WorldLoadingState::Failed,
                 client::WorldLoadingState::Cancelled}) {
            EXPECT_EQ(client::uiLoadingMapSource(state, model), client::UiLoadingMapSource::Backdrop);
        }
        for (auto const state : {client::WorldLoadingState::PreparingAtlas, client::WorldLoadingState::PreparingSpawn,
                 client::WorldLoadingState::ReadyForStart, client::WorldLoadingState::Starting}) {
            EXPECT_EQ(client::uiLoadingMapSource(state, model), model == generation::TerrainModelTag::R6
                ? client::UiLoadingMapSource::Legacy : client::UiLoadingMapSource::Native);
        }
    }
}

TEST(UiPresentationTest, NativeGenerationRailAdvancesInPhysicalOrderWithoutPhantomRelief)
{
    client::WorldLoadingStatus const loading{.state = client::WorldLoadingState::PreparingAtlas};
    auto labelsFor = [&loading](client::GenerationMapAnimationPhase const phase, bool const native) {
        client::GenerationMapAnimationFrame const animation{.phase = phase};
        auto const frame = client::buildUiFrame(1U, loading, false, false, {}, animation, {}, {},
            native ? client::UiLoadingMapSource::Native : client::UiLoadingMapSource::Legacy);
        auto const layout = client::layoutUiLoading(frame, 1'280U, 720U);
        client::TextRenderer text;
        client::buildUiLoadingPresentation(frame, layout, text);
        std::vector<std::string> labels;
        for (auto const& label : text.labels()) {
            labels.push_back(label.value);
        }
        return labels;
    };
    auto hasLabel = [](std::vector<std::string> const& labels, std::string const& expected) {
        return std::find(labels.begin(), labels.end(), expected) != labels.end();
    };
    uint32_t number = 1U;
    for (auto const phase : {client::GenerationMapAnimationPhase::Creation,
             client::GenerationMapAnimationPhase::Tectonics, client::GenerationMapAnimationPhase::Water,
             client::GenerationMapAnimationPhase::Ice, client::GenerationMapAnimationPhase::Climate,
             client::GenerationMapAnimationPhase::Arrival}) {
        auto const labels = labelsFor(phase, true);
        EXPECT_TRUE(hasLabel(labels, "0" + std::to_string(number) + " / 06"));
        ++number;
    }
    auto const relief = labelsFor(client::GenerationMapAnimationPhase::Relief, true);
    EXPECT_TRUE(hasLabel(relief, "02 / 06"));
    EXPECT_TRUE(hasLabel(relief, "Highlands cool first."));
    EXPECT_TRUE(hasLabel(relief, "Cooling highlands and plains"));
    EXPECT_TRUE(hasLabel(labelsFor(client::GenerationMapAnimationPhase::Water, false), "02 / 06"));
    EXPECT_TRUE(hasLabel(labelsFor(client::GenerationMapAnimationPhase::Relief, false), "03 / 06"));
}

TEST(UiPresentationTest, YoungWorldCaptionDoesNotReplaceMeasuredGenerationProgress)
{
    client::WorldLoadingStatus const loading{
        .state = client::WorldLoadingState::PreparingAtlas,
        .generation_stage = generation::GenerationStage::V9Climate,
        .generation_completed = 3U,
        .generation_total = 8U,
    };
    client::GenerationMapAnimationFrame const opening{.phase = client::GenerationMapAnimationPhase::Creation};
    auto const frame = client::buildUiFrame(2U, loading, true, false, {}, opening);
    auto const layout = client::layoutUiLoading(frame, 1'280U, 720U);
    client::TextRenderer text;
    client::buildUiLoadingPresentation(frame, layout, text);
    EXPECT_EQ(frame.generation_progress.completed, 3U);
    EXPECT_EQ(frame.generation_progress.total, 8U);
    EXPECT_TRUE(std::any_of(text.labels().begin(), text.labels().end(), [](auto const& label) {
        return label.value == "Molten rock cools.";
    }));
    EXPECT_TRUE(std::any_of(text.labels().begin(), text.labels().end(), [](auto const& label) {
        return label.value == "Cooling a young world";
    }));
    EXPECT_FALSE(frame.show_start_prompt);
}

TEST(UiPresentationTest, NativeV21GenerationStagesHaveExplicitPhaseLabels)
{
    client::WorldLoadingStatus loading{
        .state = client::WorldLoadingState::PreparingAtlas,
    };
    loading.generation_stage = generation::GenerationStage::V21Platec;
    EXPECT_EQ(client::buildUiFrame(1U, loading, false, false).phase, "Advancing native tectonic plates");
    loading.generation_stage = generation::GenerationStage::V21Terrain;
    EXPECT_EQ(client::buildUiFrame(1U, loading, false, false).phase, "Forming native terrain");
    loading.generation_stage = generation::GenerationStage::V21Climate;
    EXPECT_EQ(client::buildUiFrame(1U, loading, false, false).phase, "Solving native climate");
    loading.generation_stage = generation::GenerationStage::V21Water;
    EXPECT_EQ(client::buildUiFrame(1U, loading, false, false).phase, "Routing native water and ice");
}

TEST(UiPresentationTest, NativeV10GenerationReportsRealStagesAndMeasuredWork)
{
    client::WorldLoadingStatus loading{
        .state = client::WorldLoadingState::PreparingAtlas,
        .generation_completed = 2U,
        .generation_total = 4U,
    };
    for (auto const& [stage, label] : std::array{
             std::pair{generation::GenerationStage::V10Platec, "Advancing native tectonic plates"},
             std::pair{generation::GenerationStage::V10Terrain, "Forming native terrain"},
             std::pair{generation::GenerationStage::V10Climate, "Solving native climate"},
             std::pair{generation::GenerationStage::V10Water, "Routing native water and ice"}}) {
        loading.generation_stage = stage;
        auto const frame = client::buildUiFrame(1U, loading, false, false);
        EXPECT_EQ(frame.phase, label);
        EXPECT_EQ(frame.generation_progress.completed, 2U);
        EXPECT_EQ(frame.generation_progress.total, 4U);
        EXPECT_FALSE(frame.show_start_prompt);
    }
}
