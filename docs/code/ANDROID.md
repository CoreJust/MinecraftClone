# Android NativeActivity bridge

The Android application is a NativeActivity packaged by the Gradle module in
[`android/`](../../android). Its shared object is `mc_android`; the manifest
loads it through the `android.app.lib_name` metadata entry. The source target
is declared in [`src/CMakeLists.txt`](../../src/CMakeLists.txt) and augmented
by [`src/android/CMakeLists.txt`](../../src/android/CMakeLists.txt). It links
the NDK native app glue plus the Android system libraries, while the portable
game, Vulkan, networking, and renderer code remain in `mc`.

`android_main` creates an [`AndroidPlayerClient`](../../src/android/AndroidPlayerClient.hpp).
The client translates app commands into Vulkan surface lifetime, focus, and
render eligibility. It drains the Android looper before a frame and blocks
on the looper while paused, unfocused, or without a usable window. Teardown
destroys renderer resources before the window is released.

Network messages are serviced only while the render loop is active. Pause or
focus loss clears input before blocking; losing the native window destroys the
renderer. The client recreates rendering on resume but does not reconnect if
the server times out during a long background interval. This protocol sends
one movement step per message, so cleared input cannot leave motion active.

[`AndroidSurfaceProvider`](../../src/android/AndroidSurfaceProvider.hpp)
adapts `ANativeWindow` to the common surface-provider contract. It requires
the Vulkan surface and Android-surface instance extensions and creates the
`VkSurfaceKHR`; it must not outlive the native window.

[`AndroidInput`](../../src/android/AndroidInput.hpp) maps keyboard/gamepad
movement and a left-half touchscreen drag to the shared movement direction.
Back/Escape requests client shutdown; R requests shader reload. The Android
client explicitly disables mesh shaders and uses the vertex fallback.

[`AndroidShaderAssets`](../../src/android/AndroidShaderAssets.hpp) reads
bare `.spv` names from the APK asset manager under `shaders/`, validates their
SPIR-V word alignment and magic, and returns the portable renderer input. The
Gradle source set prepares only the fallback vertex/fragment shader sources;
see the Android section of [the build guide](../BUILD.md) for packaging and
environment requirements.
