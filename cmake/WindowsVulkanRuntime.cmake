file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" MC_VULKAN_SDK_ROOT)
set(MC_VULKAN_RUNTIME_DLL "${MC_VULKAN_SDK_ROOT}/Bin/vulkan-1.dll" CACHE FILEPATH
    "Absolute path to the x64 Vulkan runtime loader to stage with desktop executables"
)
get_filename_component(MC_VULKAN_RUNTIME_NAME "${MC_VULKAN_RUNTIME_DLL}" NAME)
string(TOLOWER "${MC_VULKAN_RUNTIME_NAME}" MC_VULKAN_RUNTIME_NAME)
if(NOT IS_ABSOLUTE "${MC_VULKAN_RUNTIME_DLL}"
    OR NOT EXISTS "${MC_VULKAN_RUNTIME_DLL}"
    OR IS_DIRECTORY "${MC_VULKAN_RUNTIME_DLL}"
    OR NOT MC_VULKAN_RUNTIME_NAME STREQUAL "vulkan-1.dll")
    message(FATAL_ERROR
        "Missing or invalid Vulkan runtime loader: ${MC_VULKAN_RUNTIME_DLL}. "
        "Set MC_VULKAN_RUNTIME_DLL to an existing absolute vulkan-1.dll path; "
        "the Vulkan SDK need not contain the driver-installed runtime."
    )
endif()
