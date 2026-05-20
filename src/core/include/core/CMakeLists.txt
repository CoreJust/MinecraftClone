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
            Assert.hpp
            Assume.hpp
            CrashHandler.hpp
            Defer.hpp
            Log.hpp
            NonCopyable.hpp
            NonMovable.hpp
)
