add_subdirectory(generation)

add_library(mc_world_sampling STATIC
    shared/world/Chunk.cpp
    shared/world/TerrainWorld.cpp
    shared/world/TerrainLod.cpp
)
target_include_directories(mc_world_sampling PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
target_link_libraries(mc_world_sampling PUBLIC mc_generation)
mc_apply_target_defaults(mc_world_sampling)

if(NOT ANDROID)
    add_executable(mc_world_map "${MC_WORLD_MAP_SOURCE}")
    target_link_libraries(mc_world_map PRIVATE mc_world_sampling)
    mc_apply_target_defaults(mc_world_map)
endif()

add_library(mc_game_core STATIC)
add_library(mc STATIC android/AndroidInputState.cpp android/AndroidInputState.hpp)
if(APPLE AND MC_BUILD_CLIENT)
    enable_language(OBJCXX)
endif()
target_link_libraries(mc PUBLIC mc_game_core)
target_link_libraries(mc_game_core
    PUBLIC
        mc_world_sampling
        CoreCpp::Core
        CoreCpp::Runtime
        CoreCpp::RuntimeNetwork
        CoreProject2026::CoreLang
        fmt::fmt
        spdlog::spdlog
        glm::glm
    PRIVATE CoreCpp::RuntimeExecutors
)
target_include_directories(mc_game_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/shared/include
    ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)
mc_apply_target_defaults(mc_game_core)
if(MC_BUILD_CLIENT OR (MC_BUILD_COLUMN_MAP AND NOT ANDROID))
    target_include_directories(mc_game_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/client/include)
    target_link_libraries(mc_game_core PRIVATE CoreCpp::RuntimeAudioOutput)
    target_sources(mc_game_core PRIVATE
        client/CameraController.cpp
        client/ClientAudio.cpp
        client/FrameScheduler.cpp
        client/GameClient.cpp
        client/TerrainLodSource.cpp
        client/TerrainLodSource.hpp
        client/TerrainLodSelection.cpp
        client/TerrainLodPresentation.cpp
        client/WorldLoading.cpp
        client/PreviewResidency.cpp
        client/PlayerPresentation.cpp
        client/ColumnMapPresentation.cpp
        client/ColumnMapClient.cpp
    )
    add_library(mc_column_replay STATIC acceptance/ColumnMapReplay.cpp acceptance/ReplayInput.cpp)
    target_sources(mc_column_replay PUBLIC FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/acceptance/include
        FILES acceptance/include/acceptance/ColumnMapReplay.hpp acceptance/include/acceptance/ReplayInput.hpp
    )
    target_include_directories(mc_column_replay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/acceptance/include)
    target_link_libraries(mc_column_replay PUBLIC mc_game_core)
    mc_apply_target_defaults(mc_column_replay)
    target_link_libraries(mc PUBLIC mc_column_replay)
    if(MC_BUILD_COLUMN_MAP AND NOT ANDROID)
        add_executable(mc_column_map tools/column_map_client.cpp)
        target_link_libraries(mc_column_map PRIVATE mc_column_replay)
        mc_apply_target_defaults(mc_column_map)
    endif()
endif()
if(MC_BUILD_CLIENT)
    target_link_libraries(mc PUBLIC CoreCpp::RuntimeGraphicsVulkan)
    target_link_libraries(mc PRIVATE CoreCpp::RuntimeAudioOutput)
    if(NOT ANDROID)
        target_link_libraries(mc
            PRIVATE
                CoreCpp::RuntimeGraphics
                CoreCpp::RuntimeGraphicsVulkanGlfw
                CoreCpp::RuntimeKernel
                CoreCpp::RuntimePlatformGlfw
        )
    else()
        target_link_libraries(mc
            PRIVATE
                CoreCpp::RuntimeGraphics
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
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)
if(MC_BUILD_CLIENT)
    target_include_directories(mc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/client/include)
endif()

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
