target_sources(mc
    PRIVATE
        BotClient.cpp
        ClientAudio.cpp
        PreviewResidency.cpp
    Camera.cpp
    CameraController.cpp
    DepthFormat.cpp
        FrameScheduler.cpp
        GameClient.cpp
        PlayerPreviewLod.cpp
        PlayerPresentation.cpp
        render/DebugHud.cpp
        render/GuiRenderer.cpp
        render/HeightTileDrawIndex.cpp
        render/ShaderAssets.cpp
        render/StoneIndirectDraws.cpp
        render/TextRenderer.cpp
        render/PresentationRenderer.cpp
        render/VulkanFrustum.cpp
)
if(NOT ANDROID)
    target_sources(mc
        PRIVATE
            PlayerClient.cpp
            render/InstalledShaderAssets.cpp
    )
endif()
if(NOT ANDROID)
    mc_target_shaders(mc
        render/shaders/grid.mesh
        render/shaders/player.mesh
        render/shaders/grid.vert
        render/shaders/player.vert
        render/shaders/stone.vert
        render/shaders/stone_indexed_block.vert
        render/shaders/stone.frag
        render/shaders/stone_solid.frag
        render/shaders/sky.vert
        render/shaders/sky.frag
        render/shaders/trivial.frag
        render/shaders/text.vert
        render/shaders/text.frag
    )

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stone_indexed_block.vert.spv" APPEND
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/stone_geometry.glsl"
    )
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stone.vert.spv" APPEND
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/stone_geometry.glsl"
    )
    mc_copy_target_shaders(mc_main mc)
endif()

add_subdirectory(include/client)
