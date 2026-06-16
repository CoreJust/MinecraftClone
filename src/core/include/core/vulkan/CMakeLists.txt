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
            Features.hpp
            Instance.hpp
            InstanceBuilder.hpp
            Layers.hpp
            MemoryProperty.hpp
            PhysicalDevice.hpp
            PhysicalDeviceCapsStruct.hpp
            PhysicalDeviceProperties.hpp
            PhysicalDeviceSelector.hpp
            PhysicalDeviceType.hpp
            QueueFamily.hpp
            Resource.hpp
            Surface.hpp
            VulkanFwd.hpp
            VulkanVersion.hpp
)
