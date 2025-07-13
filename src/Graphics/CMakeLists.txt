set(LOCAL_SOURCES
    Vulkan/VulkanManager.cpp
    Vulkan/Version.cpp
    RenderEngine.cpp
    Window.cpp)
set(LOCAL_TESTS)

add_subdirectory(Vulkan/Internal)
SUBMODULE(Graphics)
