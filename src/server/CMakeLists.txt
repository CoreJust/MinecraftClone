target_sources(mc_game_core
    PRIVATE
        GameServer.cpp
        WorldPreparation.cpp
        HeightTileWorkerPool.cpp
)

add_subdirectory(include/server)
