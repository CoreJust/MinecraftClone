target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Assert.hpp
            Assume.hpp
            AtAppExit.hpp
            ControlFlow.hpp
            CrashHandler.hpp
            Defer.hpp
            NonCopyable.hpp
            NonMovable.hpp
            StaticInitializer.hpp
            Version.hpp
)
