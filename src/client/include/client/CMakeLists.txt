target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            BotClient.hpp
            GameClient.hpp
            PlayerClient.hpp
            render/DebugHud.hpp
            render/InstalledShaderAssets.hpp
            render/ShaderAssets.hpp
            render/VulkanRenderer.hpp
)
