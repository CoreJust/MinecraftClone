target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            BotClient.hpp
            Camera.hpp
            CameraController.hpp
            FrameScheduler.hpp
            GameClient.hpp
            PlayerClient.hpp
            PlayerPresentation.hpp
            PreviewMeshing.hpp
            PreviewResidency.hpp
            render/DebugHud.hpp
            render/DepthFormat.hpp
            render/GuiRenderer.hpp
            render/InstalledShaderAssets.hpp
            render/ShaderAssets.hpp
            render/StoneTexture.hpp
            render/TextRenderer.hpp
            render/VulkanRenderer.hpp
)
