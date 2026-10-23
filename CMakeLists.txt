cmake_minimum_required(VERSION 3.25)

project(MinecraftClone2026
    VERSION 0.1.1
    DESCRIPTION "A clone of Minecraft created for self-study and fun reasons."
    LANGUAGES C CXX
)

###  C++ OPTIONS  ###
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


###  BUILD OPTIONS  ###
option(BUILD_TESTING "Build tests" ON)
option(MC_BUILD_CLIENT "Build Client rendering and platform adapters" ON)
option(MC_BUILD_COLUMN_MAP "Build the CPU-only normal-game column-map client" ${MC_BUILD_CLIENT})
option(MC_ANALYSIS_HOST "Permit server-only analysis builds on otherwise unsupported host platforms" OFF)
option(MC_ENABLE_WARNINGS "Enable strict warnings" ON)
option(MC_ENABLE_SANITIZERS "Enable sanitizers where supported" OFF)
option(MC_ENABLE_HIGH_ASSERT "Enable high assertions" OFF)
option(MC_ENABLE_VULKAN_VALIDATION_LAYERS "Enable Vulkan validation layers" OFF)
option(
    MC_ALLOW_INEXACT_CORECPP
    "Deprecated compatibility flag; exact CoreCpp provenance is always required"
    OFF
)
if(NOT WIN32 AND NOT APPLE AND NOT ANDROID)
    if(NOT MC_ANALYSIS_HOST OR MC_BUILD_CLIENT)
        message(FATAL_ERROR
            "MinecraftClone product builds support only Windows, macOS, and Android; "
            "unsupported hosts may configure server-only analysis builds with "
            "MC_ANALYSIS_HOST=ON and MC_BUILD_CLIENT=OFF"
        )
    endif()
endif()
if(ANDROID)
    # Tests target desktop-only window and process contracts. Android builds are
    # produced by the Gradle application module and never include CTest.
    set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
endif()
if(ANDROID AND NOT MC_BUILD_CLIENT)
    message(FATAL_ERROR "Android packaging requires MC_BUILD_CLIENT=ON")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR MC_ENABLE_HIGH_ASSERT)
    add_compile_definitions(_MC_DEBUG)
    add_compile_definitions(_CORE_DEBUG)
endif()
if(MC_ENABLE_HIGH_ASSERT)
    add_compile_definitions(_MC_ENABLE_HIGH_ASSERT)
    add_compile_definitions(_CORE_ENABLE_HIGH_ASSERT)
endif()
if(MC_ENABLE_VULKAN_VALIDATION_LAYERS)
    add_compile_definitions(_MC_VK_VALIDATION_LAYERS)
endif()


###  DEPENDENCIES SETUP  ###
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoreCppProvenance.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoreProject2026Provenance.cmake")
# Resolve transitive packages from the selected vcpkg triplet before host-installed packages.
if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    list(PREPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()
find_package(CoreCpp CONFIG REQUIRED COMPONENTS Core Runtime RuntimeNetwork RuntimeExecutors)
if(MC_BUILD_CLIENT OR (MC_BUILD_COLUMN_MAP AND NOT ANDROID))
    find_package(CoreCpp CONFIG REQUIRED COMPONENTS RuntimeAudioOutput)
endif()
if(MC_BUILD_CLIENT)
    if(ANDROID)
        find_package(CoreCpp CONFIG REQUIRED COMPONENTS
            RuntimeGraphics
            RuntimeGraphicsVulkan
            RuntimeGraphicsVulkanAndroid
            RuntimeKernel
        )
    else()
        find_package(CoreCpp CONFIG REQUIRED COMPONENTS
            RuntimeGraphics
            RuntimeGraphicsVulkan
            RuntimeGraphicsVulkanGlfw
            RuntimeKernel
            RuntimePlatformGlfw
        )
    endif()
endif()
mc_check_corecpp_provenance()
find_package(CoreProject2026 0.1.5 CONFIG REQUIRED COMPONENTS CoreLang)
mc_check_coreproject2026_provenance()

find_package(fmt CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
if(MC_BUILD_CLIENT)
    if(ANDROID)
        find_package(Vulkan REQUIRED)
    else()
        find_package(Vulkan REQUIRED COMPONENTS glslc)
    endif()
endif()

if(WIN32 AND MC_BUILD_CLIENT AND NOT ANDROID)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/WindowsVulkanRuntime.cmake")
endif()


###  CMAKE HELPERS  ###
include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Helpers)

set(MC_WINDOWS_RUNTIME_DEPENDENCIES
    CoreCpp::Core
    CoreCpp::Runtime
    CoreCpp::RuntimeNetwork
    CoreCpp::RuntimeGraphics
    CoreCpp::RuntimeGraphicsVulkan
    CoreCpp::RuntimeGraphicsVulkanGlfw
    CoreCpp::RuntimeKernel
    CoreCpp::RuntimePlatformGlfw
    CoreProject2026::CoreLang
    fmt::fmt
    glfw
    spdlog::spdlog
    unofficial::enet::enet
)


###  BUILD  ###
if(MC_BUILD_CLIENT AND NOT ANDROID)
    add_executable(mc_main
        src/main.cpp
    )
    mc_apply_target_defaults(mc_main)
endif()
add_subdirectory(src)

if(MC_BUILD_CLIENT AND NOT ANDROID)
    mc_stage_windows_runtime(
        mc_main
        RUNTIME_ROOT "$<TARGET_FILE_DIR:mc_main>"
        DEPENDENCIES ${MC_WINDOWS_RUNTIME_DEPENDENCIES}
        REQUIRED_FILES "${MC_VULKAN_RUNTIME_DLL}"
    )
    install(TARGETS mc_main RUNTIME DESTINATION .)
    if(WIN32)
        install(FILES "${MC_VULKAN_RUNTIME_DLL}" DESTINATION .)
    endif()
    install(FILES $<TARGET_PROPERTY:mc,MC_SHADER_FILES> DESTINATION shaders)
endif()

if(NOT MC_BUILD_CLIENT)
    set(MC_FORBIDDEN_CPU_TARGETS
        CoreCpp::RuntimeGraphics
        CoreCpp::RuntimeGraphicsVulkan
        CoreCpp::RuntimeGraphicsVulkanGlfw
        CoreCpp::RuntimeGraphicsVulkanAndroid
        CoreCpp::RuntimePlatformGlfw
        Vulkan::Vulkan
    )
    if(NOT MC_BUILD_COLUMN_MAP)
        list(APPEND MC_FORBIDDEN_CPU_TARGETS CoreCpp::RuntimeAudio CoreCpp::RuntimeAudioOutput)
    endif()
    foreach(forbidden_target IN LISTS MC_FORBIDDEN_CPU_TARGETS)
        if(TARGET "${forbidden_target}")
            message(FATAL_ERROR "CPU-only configuration loaded optional target: ${forbidden_target}")
        endif()
    endforeach()
    get_target_property(server_only_links mc LINK_LIBRARIES)
    foreach(forbidden_target IN LISTS MC_FORBIDDEN_CPU_TARGETS)
        if("${server_only_links}" MATCHES "${forbidden_target}")
            message(FATAL_ERROR "CPU-only target links optional dependency: ${forbidden_target}")
        endif()
    endforeach()
endif()

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
