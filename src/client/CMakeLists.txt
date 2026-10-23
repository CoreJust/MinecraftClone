target_sources(mc
    PRIVATE
        BotClient.cpp
        HeightTileInterestRegion.cpp
        PreviewMeshConnectivity.cpp
        TerrainLodRuntime.cpp
    Camera.cpp
    DepthFormat.cpp
        GenerationMapPresentation.cpp
        PlayerPreviewLod.cpp
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
            ScreenshotPng.cpp
            ScreenshotClipboard.cpp
            render/InstalledShaderAssets.cpp
    )
endif()
if(APPLE)
    find_library(MC_APPKIT_FRAMEWORK AppKit REQUIRED)
    target_sources(mc PRIVATE ScreenshotClipboard.mm)
    target_link_libraries(mc PRIVATE ${MC_APPKIT_FRAMEWORK})
endif()
if(NOT ANDROID)
    mc_target_shaders(mc
        render/shaders/grid.mesh
        render/shaders/player.mesh
        render/shaders/grid.vert
        render/shaders/player.vert
        render/shaders/player.frag
        render/shaders/stone.vert
        render/shaders/stone_indexed_block.vert
        render/shaders/stone.frag
        render/shaders/stone_solid.frag
        render/shaders/sky.vert
        render/shaders/sky.frag
        render/shaders/trivial.frag
        render/shaders/text.vert
        render/shaders/text.frag
        render/shaders/map.vert
        render/shaders/map.frag
    )

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stone_indexed_block.vert.spv" APPEND
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/stone_geometry.glsl"
    )
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stone.vert.spv" APPEND
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/stone_geometry.glsl"
    )
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stone.frag.spv" APPEND
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/terrain_texture_sampling.glsl"
    )
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stone_solid.frag.spv" APPEND
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/terrain_texture_sampling.glsl"
    )
    foreach(curved_shader IN ITEMS
        stone.vert stone_indexed_block.vert stone.frag stone_solid.frag
        player.vert player.mesh player.frag text.vert text.frag
    )
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${curved_shader}.spv" APPEND
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/world_curvature.glsl"
        )
    endforeach()
    mc_copy_target_shaders(mc_main mc)
endif()

add_subdirectory(include/client)
