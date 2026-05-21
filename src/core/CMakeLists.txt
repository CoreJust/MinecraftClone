target_sources(mc
    PRIVATE
        IO/Log.cpp
        IO/StacktraceFmt.cpp
        net/Address.cpp
        net/Client.cpp
        net/Event.cpp
        net/Host.cpp
        net/Net.cpp
        net/Peer.cpp
        net/Server.cpp
        CrashHandler.cpp
)

add_subdirectory(include/core)
