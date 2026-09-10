target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            EnumBitsFmt.hpp
            EnumFmt.hpp
            File.hpp
            JoinContainerFmt.hpp
            Log.hpp
            OptionalFmt.hpp
            StacktraceFmt.hpp
            TaggedBoolFmt.hpp
)
