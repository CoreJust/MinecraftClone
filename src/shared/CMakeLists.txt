target_sources(mc
    PRIVATE
        net/Message.cpp
        scenario/CoreLangScenario.cpp
        scenario/Scenario.cpp
        world/CanonicalWorld.cpp
        world/ScriptedWorld.cpp
        world/Chunk.cpp
        world/ChunkMesher.cpp
        world/WorldGeneration.cpp
        world/WorldGenerationScheduler.cpp
        world/SparseWorld.cpp
        world/World.cpp
)

add_subdirectory(include/shared)

file(READ "${PROJECT_SOURCE_DIR}/scenarios/world/canonical_world.core" MC_CANONICAL_WORLD_SCRIPT)
file(READ "${PROJECT_SOURCE_DIR}/scenarios/world/height_tile.core" MC_S6_HEIGHT_TILE_SCRIPT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/scenarios/world/canonical_world.core"
    "${PROJECT_SOURCE_DIR}/scenarios/world/height_tile.core"
)
configure_file(
    world/CanonicalWorldScript.hpp.in
    "${PROJECT_BINARY_DIR}/generated/shared/CanonicalWorldScript.hpp"
    @ONLY
)
configure_file(
    world/HeightTileScript.hpp.in
    "${PROJECT_BINARY_DIR}/generated/shared/HeightTileScript.hpp"
    @ONLY
)
target_include_directories(mc PRIVATE "${PROJECT_BINARY_DIR}/generated")
