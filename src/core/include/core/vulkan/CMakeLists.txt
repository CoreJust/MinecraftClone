target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Check.hpp
            DebugMessengerOptions.hpp
            ErrorCallbacks.hpp
            Extensions.hpp
            Layers.hpp
            VulkanVersion.hpp
)
