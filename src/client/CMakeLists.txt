target_sources(mc
    PRIVATE
        render/Window.cpp
        BotClient.cpp
        GameClient.cpp
        PlayerClient.cpp
)

add_subdirectory(include/client)
