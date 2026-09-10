if(NOT ANDROID)
    target_sources(mc
        PRIVATE
            Key.cpp
            Keyboard.cpp
            Mouse.cpp
            Window.cpp
    )
endif()
