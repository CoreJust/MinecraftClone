target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Enum.hpp
            EnumImpl.hpp
            EnumMapping.hpp
            Field.hpp
            FieldRequirement.hpp
            TaggedBool.hpp
            TypeName.hpp
)
