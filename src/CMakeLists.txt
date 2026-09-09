add_library(mc STATIC)

target_sources(mc
    PRIVATE
        android/AndroidInputState.cpp
        android/AndroidInputState.hpp
)

target_link_libraries(mc
    PUBLIC
        fmt::fmt
        spdlog::spdlog
    PRIVATE
        unofficial::enet::enet
        glm::glm
        unofficial::spirv-reflect
        volk::volk_headers
        GPUOpen::VulkanMemoryAllocator
)
if(NOT ANDROID)
    target_link_libraries(mc PRIVATE glfw)
endif()
target_compile_definitions(mc
    PRIVATE
        VMA_STATIC_VULKAN_FUNCTIONS=0
        VMA_DYNAMIC_VULKAN_FUNCTIONS=0
)
if(WIN32) # For ENet
    target_link_libraries(mc PRIVATE winmm ws2_32)
endif()
if(APPLE) # For vkCreateMetalSurfaceEXT and the Cocoa/CAMetalLayer window binding
    target_compile_definitions(mc PRIVATE VK_USE_PLATFORM_METAL_EXT)
    target_link_libraries(mc PRIVATE "-framework Cocoa" "-framework QuartzCore")
endif()
if(ANDROID)
    target_compile_definitions(mc PUBLIC VK_USE_PLATFORM_ANDROID_KHR)

    add_library(mc_android SHARED)
    target_link_libraries(mc_android PRIVATE mc android log dl)
    target_compile_definitions(mc_android PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
    mc_apply_target_defaults(mc_android)
endif()


target_include_directories(mc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core/include
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
        ${CMAKE_CURRENT_SOURCE_DIR}/client/include
)

add_subdirectory(core)
add_subdirectory(shared)
add_subdirectory(server)
add_subdirectory(client)

if(ANDROID)
    add_subdirectory(android)
else()
    target_link_libraries(mc_main PRIVATE mc)
endif()
mc_apply_target_defaults(mc)
