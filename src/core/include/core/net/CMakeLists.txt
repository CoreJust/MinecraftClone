target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Address.hpp
            Client.hpp
            Event.hpp
            GenerateEvents.hpp
            Host.hpp
            Net.hpp
            Peer.hpp
            SendMode.hpp
            Server.hpp
)
