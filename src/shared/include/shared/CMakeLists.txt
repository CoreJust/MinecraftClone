target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            ProjectInfo.hpp
            net/Message.hpp
            scenario/Scenario.hpp
            world/CanonicalWorld.hpp
            world/ScriptedWorld.hpp
            world/Chunk.hpp
            world/ChunkMesher.hpp
            world/WorldGeneration.hpp
            world/WorldGenerationScheduler.hpp
            world/SparseWorld.hpp
            world/World.hpp
)
