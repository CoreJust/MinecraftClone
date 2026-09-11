cmake_minimum_required(VERSION 3.25)

project(MinecraftCloneCoreCppServerConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CoreCpp CONFIG REQUIRED COMPONENTS Core Runtime RuntimeNetwork)

foreach(required_target IN ITEMS CoreCpp::Core CoreCpp::Runtime CoreCpp::RuntimeNetwork)
    if(NOT TARGET "${required_target}")
        message(FATAL_ERROR "Missing CoreCpp dependency closure target: ${required_target}")
    endif()
endforeach()
foreach(forbidden_target IN ITEMS
    CoreCpp::RuntimeGraphics
    CoreCpp::RuntimeGraphicsVulkan
    CoreCpp::RuntimePlatformGlfw
    CoreCpp::RuntimePlatformAndroid
    CoreCpp::RuntimeAudio
)
    if(TARGET "${forbidden_target}")
        message(FATAL_ERROR "Server-only consumer loaded optional target: ${forbidden_target}")
    endif()
endforeach()

add_executable(mc_corecpp_server_consumer main.cpp)
target_link_libraries(mc_corecpp_server_consumer PRIVATE CoreCpp::RuntimeNetwork)
