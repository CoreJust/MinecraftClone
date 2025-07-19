set(LOCAL_SOURCES
    Window/Window.cpp
    Window/Keyboard.cpp
    Vulkan/Pipeline/Attribute.cpp
    Vulkan/Pipeline/Vertices.cpp
    Vulkan/VulkanManager.cpp
    Vulkan/Version.cpp
    RenderEngine.cpp)
set(LOCAL_TESTS)

add_subdirectory(Vulkan/Internal)
SUBMODULE(Graphics)
