target_sources(mc_game_core
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            GameServer.hpp
            WorldPreparation.hpp
            HeightTileWorkerPool.hpp
            detail/NetworkEventBatch.hpp
)
