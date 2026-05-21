target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            macro/Attributes.hpp
            macro/Compiler.hpp
            macro/LanguageVersion.hpp
            macro/NameGeneration.hpp
            macro/OS.hpp
            net/Client.hpp
            net/Event.hpp
            net/Host.hpp
            net/Net.hpp
            net/Peer.hpp
            net/SendMode.hpp
            net/Server.hpp
            OS/Windows/Lean.hpp
            Assert.hpp
            Assume.hpp
            AtAppExit.hpp
            CrashHandler.hpp
            Defer.hpp
            Log.hpp
            NonCopyable.hpp
            NonMovable.hpp
            StaticInitializer.hpp
)
