target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            ProjectInfo.hpp
            net/Message.hpp
            world/World.hpp
)
