add_library(mc STATIC)

target_sources(mc
    PRIVATE
        android/AndroidInputState.cpp
        android/AndroidInputState.hpp
)

target_link_libraries(mc
    PUBLIC
        CoreCpp::Core
        CoreCpp::Runtime
        CoreCpp::RuntimeNetwork
        CoreProject2026::CoreLang
        fmt::fmt
        spdlog::spdlog
    PRIVATE
        glm::glm
)
if(MC_BUILD_CLIENT)
    if(NOT ANDROID)
        target_link_libraries(mc
            PRIVATE
                CoreCpp::RuntimeGraphics
                CoreCpp::RuntimeGraphicsVulkan
                CoreCpp::RuntimeGraphicsVulkanGlfw
                CoreCpp::RuntimeKernel
                CoreCpp::RuntimePlatformGlfw
        )
    else()
        target_link_libraries(mc
            PRIVATE
                CoreCpp::RuntimeGraphics
                CoreCpp::RuntimeGraphicsVulkan
                CoreCpp::RuntimeGraphicsVulkanAndroid
                CoreCpp::RuntimeKernel
        )
    endif()
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
)
if(MC_BUILD_CLIENT)
    target_include_directories(mc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/client/include)
endif()

add_subdirectory(core)
add_subdirectory(shared)
add_subdirectory(server)
if(MC_BUILD_CLIENT)
    add_subdirectory(client)
endif()

if(ANDROID)
    add_subdirectory(android)
elseif(MC_BUILD_CLIENT)
    add_subdirectory(acceptance)
    target_link_libraries(mc_main PRIVATE mc)
endif()
mc_apply_target_defaults(mc)
