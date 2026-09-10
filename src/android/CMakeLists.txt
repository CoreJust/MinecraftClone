target_sources(mc_android
    PRIVATE
        AndroidInput.cpp
        AndroidInput.hpp
        AndroidMain.cpp
        AndroidPlayerClient.cpp
        AndroidPlayerClient.hpp
        AndroidShaderAssets.cpp
        AndroidShaderAssets.hpp
        AndroidSurfaceProvider.cpp
        AndroidSurfaceProvider.hpp
        ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

target_compile_definitions(mc_android PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_link_options(mc_android PRIVATE "-uANativeActivity_onCreate")

target_include_directories(mc_android
    PRIVATE
        ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue
)

mc_apply_target_defaults(mc_android)
