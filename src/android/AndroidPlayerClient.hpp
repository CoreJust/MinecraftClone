#pragma once

#include "AndroidInput.hpp"
#include "AndroidShaderAssets.hpp"

#include <client/Camera.hpp>
#include <client/GameClient.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PlayerPreviewLod.hpp>
#include <client/PreviewMeshing.hpp>
#include <client/PreviewMeshWorkQueue.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <android_native_app_glue.h>

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
    void render() override;
    void presentFrame();

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
    void destroyWindowResources() noexcept;
    void pollOneEvent(int32_t timeout_millis);
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
    shared::ChunkMesher m_chunk_mesher;
    shared::ChunkMesh const* m_chunk_mesh = nullptr;
    std::unique_ptr<client::VulkanRenderer> m_renderer;
    struct PreviewMeshKeyHash final {
        [[nodiscard]] uint64_t operator()(client::HeightTileKey const key) const noexcept
        {
            return shared::heightTileCoordinateHash(key.x, key.y);
        }
    };
    client::PreviewMeshWorkQueue m_preview_mesh_work;
    client::PlayerPreviewLod m_preview_lod;
    std::optional<shared::HeightTileKey> m_preview_interest_center;
    std::optional<uint32_t> m_preview_interest_radius;
    std::unordered_map<client::HeightTileKey, shared::HeightTileSurfaceMesh, PreviewMeshKeyHash>
        m_visible_preview_mesh_bases;
    std::unordered_map<client::HeightTileKey, client::PreviewMeshSeamBridgeSet, PreviewMeshKeyHash>
        m_visible_preview_mesh_seam_bridges;
    float m_density_scale = 1.0F;
    bool m_resumed = false;
    bool m_has_focus = false;
    AndroidFlightInputLatch m_ascend_input;
    bool m_descend_pressed = false;
    client::CameraPerspective m_camera_perspective = client::CameraPerspective::FirstPerson;
};

} // namespace game_android
