#include <client/UiPresentation.hpp>
#include <client/WorldLoading.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>

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

TEST(UiPresentationTest, SynchronizingAndStartingKeepTheSameMapViewportAndRealServerStatus)
{
    client::GenerationMapAnimationFrame const animation{
        .phase = client::GenerationMapAnimationPhase::Relief,
        .phase_progress = 0.75F,
    };
    client::WorldLoadingStatus synchronizing_status{
        .state = client::WorldLoadingState::PreparingSpawn,
        .initial_sphere_ready = true,
        .playable_sphere_ready_chunks = 3U,
        .playable_sphere_required_chunks = 7U,
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
