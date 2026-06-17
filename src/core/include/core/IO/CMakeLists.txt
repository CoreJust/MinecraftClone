target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            EnumFmt.hpp
            File.hpp
            Log.hpp
            OptionalFmt.hpp
            StacktraceFmt.hpp
)
