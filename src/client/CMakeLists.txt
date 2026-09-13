target_sources(mc
    PRIVATE
        BotClient.cpp
    Camera.cpp
    CameraController.cpp
    DepthFormat.cpp
        FrameScheduler.cpp
        GameClient.cpp
        PlayerPresentation.cpp
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
        render/shaders/stone.vert
        render/shaders/stone.frag
        render/shaders/trivial.frag
        render/shaders/debug_hud.vert
        render/shaders/debug_hud.frag
    )

    mc_copy_target_shaders(mc_main mc)
    mc_stage_windows_runtime(
        mc_main
        RUNTIME_ROOT "$<TARGET_FILE_DIR:mc_main>"
        DEPENDENCIES ${MC_WINDOWS_RUNTIME_DEPENDENCIES}
        REQUIRED_FILES "${MC_VULKAN_RUNTIME_DLL}"
    )
    install(TARGETS mc_main RUNTIME DESTINATION .)
    if(WIN32)
        install(FILES "${MC_VULKAN_RUNTIME_DLL}" DESTINATION .)
    endif()
    install(FILES $<TARGET_PROPERTY:mc,MC_SHADER_FILES> DESTINATION shaders)
endif()

add_subdirectory(include/client)
