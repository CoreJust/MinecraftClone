target_sources(mc
    PRIVATE
        net/Message.cpp
        scenario/CoreLangScenario.cpp
        scenario/Scenario.cpp
        world/CanonicalWorld.cpp
        world/ScriptedWorld.cpp
        world/Chunk.cpp
        world/ChunkMesher.cpp
        world/World.cpp
)

add_subdirectory(include/shared)

file(READ "${PROJECT_SOURCE_DIR}/scenarios/world/canonical_world.core" MC_CANONICAL_WORLD_SCRIPT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/scenarios/world/canonical_world.core"
)
configure_file(
    world/CanonicalWorldScript.hpp.in
    "${PROJECT_BINARY_DIR}/generated/shared/CanonicalWorldScript.hpp"
    @ONLY
)
target_include_directories(mc PRIVATE "${PROJECT_BINARY_DIR}/generated")
