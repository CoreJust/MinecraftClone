target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Attributes.hpp
            Compiler.hpp
            LanguageVersion.hpp
            NameGeneration.hpp
            OS.hpp
)
