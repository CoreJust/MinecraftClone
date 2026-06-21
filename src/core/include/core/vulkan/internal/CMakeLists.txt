target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            PhysicalDeviceCapsStruct.hpp
            VMA.hpp
            VulkanFwd.hpp
)
add_subdirectory(internal)