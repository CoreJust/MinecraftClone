set(DEPS
    RGFW
    Vulkan::Vulkan)
set(INCLUDES
    ./)
set(SYSTEM_INCLUDES
    ${RGFW_INCLUDE})
set(SOURCES
    main.cpp
    Config.cpp
    Resources.cpp)
set(TESTS)

add_subdirectory(Core)
add_subdirectory(Graphics)
MAIN_MODULE(clone)
