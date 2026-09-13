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
            VERBATIM
        )

        list(APPEND SPV_FILES ${SPV_FILE})
    endforeach()

    add_custom_target(${TARGET}_shaders ALL
        DEPENDS ${SPV_FILES}
        COMMENT "Compiling all shaders for ${TARGET}"
        SOURCES ${SHADER_SOURCE_FILES}
    )
    add_dependencies(${TARGET} ${TARGET}_shaders)
    set_property(TARGET ${TARGET} PROPERTY MC_SHADER_FILES "${SPV_FILES}")
endfunction()

function(mc_copy_target_shaders TARGET SHADER_TARGET)
    get_target_property(SPV_FILES ${SHADER_TARGET} MC_SHADER_FILES)
    add_custom_target(${TARGET}_shader_assets
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${TARGET}>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPV_FILES} "$<TARGET_FILE_DIR:${TARGET}>/shaders"
        DEPENDS ${SHADER_TARGET}_shaders
        VERBATIM
    )
    add_dependencies(${TARGET} ${TARGET}_shader_assets)
endfunction()

function(mc_stage_windows_runtime TARGET)
    if(NOT WIN32)
        return()
    endif()

    set(options)
    set(one_value_arguments RUNTIME_ROOT)
    set(multi_value_arguments DEPENDENCIES REQUIRED_FILES)
    cmake_parse_arguments(MC_STAGE "${options}" "${one_value_arguments}" "${multi_value_arguments}" ${ARGN})

    if(NOT TARGET "${TARGET}")
        message(FATAL_ERROR "Cannot stage runtime for missing target: ${TARGET}")
    endif()
    get_target_property(target_type "${TARGET}" TYPE)
    if(NOT target_type STREQUAL "EXECUTABLE" AND NOT target_type STREQUAL "SHARED_LIBRARY")
        message(FATAL_ERROR "Windows runtime staging requires an executable or shared-library target: ${TARGET}")
    endif()
    if(NOT MC_STAGE_RUNTIME_ROOT)
        set(MC_STAGE_RUNTIME_ROOT "$<TARGET_FILE_DIR:${TARGET}>")
    endif()
    if(NOT MC_STAGE_DEPENDENCIES AND NOT MC_STAGE_REQUIRED_FILES)
        message(FATAL_ERROR "Windows runtime staging has no required files for target: ${TARGET}")
    endif()

    set(runtime_files ${MC_STAGE_REQUIRED_FILES})
    foreach(required_file IN LISTS MC_STAGE_REQUIRED_FILES)
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR "Windows runtime staging file is missing: ${required_file}")
        endif()
    endforeach()
    foreach(dependency IN LISTS MC_STAGE_DEPENDENCIES)
        if(NOT TARGET "${dependency}")
            message(FATAL_ERROR "Windows runtime staging dependency is missing: ${dependency}")
        endif()
        get_target_property(dependency_type "${dependency}" TYPE)
        if(
            dependency_type STREQUAL "SHARED_LIBRARY"
            OR dependency_type STREQUAL "MODULE_LIBRARY"
            OR dependency_type STREQUAL "UNKNOWN_LIBRARY"
        )
            list(APPEND runtime_files "$<TARGET_FILE:${dependency}>")
        endif()
    endforeach()

    foreach(runtime_file IN LISTS runtime_files)
        add_custom_command(
            TARGET ${TARGET}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${runtime_file}" "${MC_STAGE_RUNTIME_ROOT}"
            VERBATIM
        )
    endforeach()
endfunction()
