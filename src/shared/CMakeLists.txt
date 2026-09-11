target_sources(mc
    PRIVATE
        net/Message.cpp
        scenario/CoreLangScenario.cpp
        scenario/Scenario.cpp
        world/World.cpp
)

add_subdirectory(include/shared)
