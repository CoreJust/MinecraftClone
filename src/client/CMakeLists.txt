target_sources(mc
    PRIVATE
        BotClient.cpp
        PreviewResidency.cpp
    Camera.cpp
    CameraController.cpp
    DepthFormat.cpp
        FrameScheduler.cpp
        GameClient.cpp
        PlayerPresentation.cpp
        render/DebugHud.cpp
        render/GuiRenderer.cpp
        render/ShaderAssets.cpp
        render/TextRenderer.cpp
        render/PresentationRenderer.cpp
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
        render/shaders/stone.frag
        render/shaders/stone_solid.frag
        render/shaders/sky.vert
        render/shaders/sky.frag
        render/shaders/trivial.frag
        render/shaders/text.vert
        render/shaders/text.frag
    )

    mc_copy_target_shaders(mc_main mc)
endif()

add_subdirectory(include/client)
