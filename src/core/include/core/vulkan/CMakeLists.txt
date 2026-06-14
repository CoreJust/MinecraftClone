target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Capabilities.hpp
            Check.hpp
            DebugMessengerOptions.hpp
            Error.hpp
            ErrorCallbacks.hpp
            Extensions.hpp
            Handles.hpp
            Instance.hpp
            InstanceBuilder.hpp
            Layers.hpp
            Raii.hpp
            VulkanVersion.hpp
)
