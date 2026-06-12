target_sources(mc
    PRIVATE
        render/VulkanRenderer.cpp
        BotClient.cpp
        GameClient.cpp
        PlayerClient.cpp
)
mc_target_shaders(mc
    render/shaders/grid.mesh
    render/shaders/player.mesh
    render/shaders/trivial.frag
)

add_subdirectory(include/client)
