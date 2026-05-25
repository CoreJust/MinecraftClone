target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            render/Window.hpp
            BotClient.hpp
            GameClient.hpp
            PlayerClient.hpp
)