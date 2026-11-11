#pragma once

#include "AndroidInput.hpp"
#include "AndroidShaderAssets.hpp"

#include <client/BlockActionPresentation.hpp>
#include <client/Camera.hpp>
#include <client/GameClient.hpp>
#include <client/GenerationMapPresentation.hpp>
#include <client/PlayerClientInputState.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PlayerPreviewLod.hpp>
#include <client/PreviewMeshing.hpp>
#include <client/PreviewMeshWorkQueue.hpp>
#include <client/S2MapPresentation.hpp>
#include <client/TerrainLodRuntime.hpp>
#include <client/WorldLoading.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <generation/Generation.hpp>

#include <android_native_app_glue.h>

#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>

namespace game_android {

class AndroidPlayerClient final : public client::GameClient {
public:
    explicit AndroidPlayerClient(
        android_app& app,
        shared::WorldMode mode = shared::WorldMode::Flat
    );
    ~AndroidPlayerClient();
private:
    void servicePlatformEvents() override;
    shared::Direction input() override;
    void onInputSubmitted(shared::Direction const& direction) override;
    void render() override;
    void presentFrame();
    void reportTerrainPhase(client::RendererRuntimeInfo const& runtime);
    [[nodiscard]] bool presentationSucceeded() const override { return m_presentation_succeeded; }
    void updateLoadingPresentation(client::WorldLoadingStatus const& status);
    void resetLoadingPresentation();
    void updateMapPresentation();
    [[nodiscard]] std::optional<shared::VoxelRay> interactionEyeRay() const;
    [[nodiscard]] std::optional<shared::BlockTarget> resolveInteractionTarget() const;
    [[nodiscard]] shared::ServerBlockActionResultMessage const* currentBlockActionStatus() const noexcept;
    void clearPendingBlockActions() noexcept;
    void rememberBlockAction(uint32_t sequence, shared::BlockActionKind kind,
        std::optional<shared::WorldCoordinate> target, shared::Block material);
    [[nodiscard]] bool submitTrackedBlockAction(shared::BlockActionKind kind, shared::VoxelRay const& aim,
        std::optional<shared::WorldCoordinate> target_hint, shared::Block selected);
    [[nodiscard]] bool submitTrackedGameMode(shared::GameMode mode);
    void releaseActiveBreakAction(bool cancel_placement = true);
    void updateBlockActionInput(bool left_pressed, bool left_was_pressed, bool right_pressed,
        bool right_was_pressed, bool cycle_block_pressed, bool change_mode_pressed,
        bool cycle_block_was_pressed, bool change_mode_was_pressed,
        std::chrono::steady_clock::time_point now);
    void updateBlockInteractions(std::chrono::steady_clock::time_point now);

    void setWorldMapOpen(bool open);
    void invalidateWorldMapView() noexcept;
    bool updateWorldMapView(std::shared_ptr<shared::TerrainWorld const> const& world,
        std::shared_ptr<shared::TerrainLodArtifact const> const& root, client::S2MapLayout const& layout,
        double player_x, double player_y, uint32_t width, uint32_t height);
    void applyMapAction(client::S2WorldMapAction action);
    [[nodiscard]] int32_t handleGameInputEvent(AInputEvent const* event);
    void consumeLoadingTouch(AInputEvent const* event);

    static void handleAppCommand(android_app* app, int32_t command);
    static int32_t handleInputEvent(android_app* app, AInputEvent* event);

    void onAppCommand(int32_t command);
    void onConnectionStateReset() override;
    [[nodiscard]] bool updateVerticalInput(AInputEvent const* event) noexcept;
    void refreshPreviewTerrain(client::PlayerPresentationPosition const& local_position);
    void processPreviewTerrain();
    void enqueuePreviewMeshIfCurrent(shared::HeightTileKey key);
    [[nodiscard]]
    bool isPreviewTileInInterest(shared::HeightTileKey key) const;
    void clearPreviewTerrain(bool remove_renderer_meshes) noexcept;
    void createWindowResources();
    void destroyWindowResources();
    void drainEvents();
    void stop() noexcept;

    [[nodiscard]]
    double maximumUnobstructedCameraDistance(
        client::PlayerPresentationPosition const& local_position
    ) const noexcept;

    [[nodiscard]]
    bool canRender() const noexcept;
private:
    android_app& m_app;
    AndroidInput m_input;
    AndroidShaderAssets m_shader_assets;
    client::Camera m_camera{
        { .position = { 9.0, 9.0, 13.0 } },
    };
    client::Camera m_look_camera{
        { .position = { 9.0, 9.0, 13.0 } },
    };
    std::unique_ptr<client::VulkanRenderer> m_renderer;
    struct PreviewMeshKeyHash final {
        [[nodiscard]] uint64_t operator()(client::HeightTileKey const key) const noexcept
        {
            return shared::heightTileCoordinateHash(key.x, key.y);
        }
    };
    client::PreviewMeshWorkQueue m_preview_mesh_work;
    client::PlayerPreviewLod m_preview_lod;
    client::TerrainLodRuntime m_terrain_lod;
    std::optional<uint32_t> m_terrain_lod_reported_columns;
    std::chrono::steady_clock::time_point m_terrain_phase_last_logged{};
    uint32_t m_terrain_phase_log_records = 0U;
    uint64_t m_renderer_generation = 0U;
    std::optional<shared::HeightTileKey> m_preview_interest_center;
    std::optional<uint32_t> m_preview_interest_radius;
    std::unordered_map<client::HeightTileKey, shared::HeightTileSurfaceMesh, PreviewMeshKeyHash>
        m_visible_preview_mesh_bases;
    std::unordered_map<client::HeightTileKey, client::PreviewMeshSeamBridgeSet, PreviewMeshKeyHash>
        m_visible_preview_mesh_seam_bridges;
    client::GenerationMapAnimator m_generation_map_animator;
    std::optional<client::WorldLoadingIdentity> m_loading_map_identity;
    std::shared_ptr<generation::GlobalAtlas const> m_loading_final_atlas;
    std::shared_ptr<client::S2MapImage const> m_loading_final_map;
    std::shared_ptr<shared::TerrainLodArtifact const> m_loading_attempted_root;
    std::chrono::steady_clock::duration m_loading_visible_elapsed{};
    std::chrono::steady_clock::time_point m_loading_last_update{};
    std::optional<uint64_t> m_hud_rss_bytes;
    std::chrono::steady_clock::time_point m_hud_sampled_at{};
    std::future<float> m_hud_climate_query;
    std::shared_ptr<shared::TerrainWorld const> m_hud_climate_world;
    std::optional<float> m_hud_temperature;
    std::chrono::steady_clock::time_point m_hud_climate_sampled_at{};
    std::chrono::steady_clock::time_point m_hud_temperature_observed_at{};
    struct PendingBlockActionPresentation final {
        uint32_t sequence = 0U;
        uint64_t join_epoch = 0U;
        shared::BlockActionKind kind = shared::BlockActionKind::BreakRelease;
        std::optional<shared::WorldCoordinate> target;
        shared::Block material = shared::Block::Stone;
    };
    static constexpr size_t PENDING_BLOCK_ACTION_PRESENTATIONS = 64U;
    std::array<PendingBlockActionPresentation, PENDING_BLOCK_ACTION_PRESENTATIONS> m_pending_block_actions{};
    client::detail::BlockBreakInputState m_block_break_input;
    client::detail::BlockPlaceInputState m_block_place_input;
    shared::Block m_selected_block = shared::Block::Stone;
    uint32_t m_client_block_action_sequence = 1U;
    std::optional<std::pair<uint32_t, shared::GameMode>> m_pending_game_mode;
    bool m_initial_block_action_status_requested = false;
    bool m_break_pressed = false;
    bool m_place_pressed = false;
    bool m_break_was_pressed = false;
    bool m_place_was_pressed = false;
    std::array<int32_t, 4U> m_action_touch_pointers{{-1, -1, -1, -1}};
    std::bitset<32U> m_action_ignored_pointers;
    std::chrono::steady_clock::time_point m_next_chip_sound{};
    bool m_world_map_open = false;
    uint64_t m_map_join_epoch = 0U;
    std::optional<shared::TerrainIdentity> m_map_identity;
    client::S2WorldMapState m_world_map_state;
    std::shared_ptr<client::S2MapImage const> m_world_map_image;
    struct WorldMapViewResult final {
        std::shared_ptr<client::S2MapImage const> source;
        std::optional<client::S2MapImage> view;
    };
    std::future<WorldMapViewResult> m_map_view_job;
    std::stop_source m_map_view_cancellation;
    uint64_t m_map_view_revision = 0U;
    uint64_t m_map_job_revision = 0U;
    uint32_t m_map_view_width = 0U;
    uint32_t m_map_view_height = 0U;
    bool m_map_view_ready = false;
    std::optional<client::S2MapImage> m_minimap_image;
    std::shared_ptr<shared::TerrainLodArtifact const> m_minimap_lod;
    std::array<float, 4U> m_minimap_bounds{};
    float m_map_touch_x = 0.0F;
    float m_map_touch_y = 0.0F;
    float m_map_pinch_distance = 0.0F;
    client::S2MapOverlayLayout m_map_overlay;
    std::optional<client::S2MapOverlayLayout::ActionRegion> m_map_pressed_action;
    int32_t m_map_control_pointer = -1;
    bool m_map_control_owned = false;
    bool m_map_control_cancelled = false;
    bool m_map_close_pressed = false;
    std::optional<client::RendererFrameIdentity> m_loading_prompt_frame_target;
    std::bitset<512U> m_loading_transition_keys;
    uint32_t m_loading_touch_pointer_mask = 0U;
    float m_density_scale = 1.0F;
    bool m_resumed = false;
    bool m_has_focus = false;
    bool m_loading_overlay_active = false;
    bool m_frame_completion_tracking_enabled = false;
    bool m_loading_prompt_frame_completed = false;
    bool m_debug_hud_enabled = true;
    AndroidFlightInputLatch m_ascend_input;
    client::PlayerClientInputState m_input_actions;
    bool m_presentation_succeeded = false;
    bool m_descend_pressed = false;
    client::CameraPerspective m_camera_perspective = client::CameraPerspective::FirstPerson;
};

} // namespace game_android
