target_sources(mc
    PRIVATE
        net/Client.cpp
        net/Event.cpp
        net/Host.cpp
        net/Net.cpp
        net/Peer.cpp
        net/Server.cpp
        CrashHandler.cpp
        Log.cpp
)

add_subdirectory(include/core)
