add_library(mc STATIC)

target_link_libraries(mc
    PUBLIC
        fmt::fmt
        spdlog::spdlog
    PRIVATE
        unofficial::enet::enet
        glfw
        glm::glm
        unofficial::spirv-reflect
        volk::volk_headers
        GPUOpen::VulkanMemoryAllocator
)
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

target_link_libraries(mc_main PRIVATE mc)
mc_apply_target_defaults(mc)
