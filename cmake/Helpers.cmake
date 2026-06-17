include_guard(GLOBAL)

include(Warnings)
include(Sanitizers)

function(mc_apply_target_defaults target)
    target_compile_definitions(${target} PRIVATE NOMINMAX)
    target_compile_definitions(${target} PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE)
    target_compile_features(${target} PRIVATE cxx_std_23)
    mc_enable_warnings(${target})
    mc_enable_sanitizers(${target})
    set_target_properties(${target}
        PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /Zc:preprocessor)
    endif()
endfunction()

function(mc_target_shaders TARGET)
    set(SHADER_SOURCE_FILES ${ARGN})
    list(LENGTH SHADER_SOURCE_FILES FILE_COUNT)
    if(FILE_COUNT EQUAL 0)
        message(FATAL_ERROR "Cannot create a shaders target without any source files")
    endif()

    set(SPV_FILES "")

    foreach(SHADER_SOURCE IN LISTS SHADER_SOURCE_FILES)
        cmake_path(ABSOLUTE_PATH SHADER_SOURCE NORMALIZE)
        cmake_path(GET SHADER_SOURCE FILENAME SHADER_NAME)
        cmake_path(GET SHADER_SOURCE EXTENSION SHADER_EXT)
        
        set(SPV_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.spv")
        set(COMPILE_ARGS "${SHADER_SOURCE}" -o "${SPV_FILE}")
        if(SHADER_EXT STREQUAL ".mesh" OR SHADER_EXT STREQUAL ".task")
            list(APPEND COMPILE_ARGS "--target-env=vulkan1.3")
        endif()

        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND Vulkan::glslc ${COMPILE_ARGS}
            DEPENDS ${SHADER_SOURCE}
            COMMENT "Compiling shader ${SHADER_NAME}"
        )
        
        list(APPEND SPV_FILES ${SPV_FILE})
    endforeach()

    add_custom_target(${TARGET}_shaders ALL
        DEPENDS ${SPV_FILES}
        COMMENT "Compiling all shaders for ${TARGET}"
        SOURCES ${SHADER_SOURCE_FILES}
    )
    add_dependencies(${TARGET} ${TARGET}_shaders)
endfunction()
