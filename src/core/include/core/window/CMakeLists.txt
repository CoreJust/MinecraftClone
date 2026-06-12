target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Key.hpp
            Keyboard.hpp
            Mouse.hpp
            Window.hpp
)
