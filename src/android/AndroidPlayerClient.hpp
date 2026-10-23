#pragma once

#include "AndroidInput.hpp"
#include "AndroidShaderAssets.hpp"

#include <client/Camera.hpp>
#include <client/GameClient.hpp>
#include <client/PlayerClientInputState.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PlayerPreviewLod.hpp>
#include <client/PreviewMeshing.hpp>
#include <client/PreviewMeshWorkQueue.hpp>
#include <client/TerrainLodRuntime.hpp>
#include <client/WorldLoading.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <generation/Generation.hpp>

#include <android_native_app_glue.h>

#include <bitset>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
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
    std::shared_ptr<generation::GenerationSnapshot const> m_loading_map_snapshot;
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
