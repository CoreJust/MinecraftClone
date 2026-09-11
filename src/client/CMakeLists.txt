target_sources(mc
    PRIVATE
        BotClient.cpp
    Camera.cpp
    CameraController.cpp
    DepthFormat.cpp
        FrameScheduler.cpp
        GameClient.cpp
        render/DebugHud.cpp
        render/ShaderAssets.cpp
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
        render/shaders/trivial.frag
        render/shaders/debug_hud.vert
        render/shaders/debug_hud.frag
    )

    mc_copy_target_shaders(mc_main mc)
    install(TARGETS mc_main RUNTIME DESTINATION .)
    install(FILES $<TARGET_PROPERTY:mc,MC_SHADER_FILES> DESTINATION shaders)
endif()

add_subdirectory(include/client)
