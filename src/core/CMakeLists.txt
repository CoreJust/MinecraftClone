target_sources(mc
    PRIVATE
        net/Net.cpp
        CrashHandler.cpp
        Log.cpp
)

add_subdirectory(include/core)
