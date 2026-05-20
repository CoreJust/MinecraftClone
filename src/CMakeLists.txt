add_library(mc STATIC)
target_link_libraries(mc
    PUBLIC
        spdlog::spdlog
        fmt::fmt
    PRIVATE
        unofficial::enet::enet
)

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
