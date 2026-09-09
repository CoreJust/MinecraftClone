target_sources(mc
    PRIVATE
        BotClient.cpp
        GameClient.cpp
        PlayerClient.cpp
        render/ShaderAssets.cpp
        render/ShaderAssets.hpp
        render/VulkanRenderer.cpp
)
mc_target_shaders(mc
    render/shaders/grid.mesh
    render/shaders/player.mesh
    render/shaders/grid.vert
    render/shaders/player.vert
    render/shaders/trivial.frag
)

mc_copy_target_shaders(mc_main mc)
install(TARGETS mc_main RUNTIME DESTINATION .)
install(FILES $<TARGET_PROPERTY:mc,MC_SHADER_FILES> DESTINATION shaders)

add_subdirectory(include/client)
