function(mc_check_coreproject2026_provenance)
    foreach(required_variable IN ITEMS
        CoreProject2026_REVISION
        CoreProject2026_DIRTY
        CoreProject2026_REVISION_EXACT
    )
        if(NOT DEFINED ${required_variable})
            message(FATAL_ERROR "CoreProject2026 package does not expose ${required_variable}")
        endif()
    endforeach()

    set(lock_file "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock.json")
    if(NOT EXISTS "${lock_file}")
        message(FATAL_ERROR "MinecraftClone requires dependencies.lock.json")
    endif()
    file(READ "${lock_file}" lock_json)
    string(
        JSON required_revision
        ERROR_VARIABLE lock_error
        GET "${lock_json}" dependencies CoreProject2026 revision
    )
    string(LENGTH "${required_revision}" required_revision_length)
    if(
        lock_error
        OR NOT required_revision_length EQUAL 40
        OR NOT required_revision MATCHES "^[0-9a-f]+$"
    )
        message(FATAL_ERROR "dependencies.lock.json must pin a full CoreProject2026 Git revision")
    endif()

    set(MC_REQUIRED_COREPROJECT2026_REVISION "${required_revision}" PARENT_SCOPE)
    set(MC_COREPROJECT2026_REVISION "${CoreProject2026_REVISION}" PARENT_SCOPE)
    set(MC_COREPROJECT2026_DIRTY "${CoreProject2026_DIRTY}" PARENT_SCOPE)
    set(MC_COREPROJECT2026_REVISION_EXACT "${CoreProject2026_REVISION_EXACT}" PARENT_SCOPE)

    if(
        NOT CoreProject2026_REVISION_EXACT
        OR CoreProject2026_DIRTY
        OR CoreProject2026_REVISION STREQUAL "unavailable"
    )
        message(FATAL_ERROR "Exact MinecraftClone builds require a clean, known CoreProject2026 revision")
    endif()
    string(LENGTH "${CoreProject2026_REVISION}" revision_length)
    if(
        NOT revision_length EQUAL 40
        OR NOT CoreProject2026_REVISION MATCHES "^[0-9a-f]+$"
    )
        message(FATAL_ERROR "CoreProject2026_REVISION is not a full lowercase Git commit")
    endif()
    if(NOT CoreProject2026_REVISION STREQUAL required_revision)
        message(
            FATAL_ERROR
            "CoreProject2026 revision mismatch: required ${required_revision}, found ${CoreProject2026_REVISION}"
        )
    endif()
endfunction()
