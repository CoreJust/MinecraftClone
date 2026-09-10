function(mc_check_corecpp_provenance)
    foreach(required_variable IN ITEMS CoreCpp_REVISION CoreCpp_DIRTY CoreCpp_REVISION_EXACT)
        if(NOT DEFINED ${required_variable})
            message(FATAL_ERROR "CoreCpp package does not expose ${required_variable}")
        endif()
    endforeach()

    set(lock_file "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock.json")
    if(NOT EXISTS "${lock_file}")
        message(FATAL_ERROR "MinecraftClone requires dependencies.lock.json")
    endif()
    file(READ "${lock_file}" lock_json)
    string(JSON lock_schema ERROR_VARIABLE lock_error GET "${lock_json}" schema)
    if(lock_error OR NOT lock_schema EQUAL 1)
        message(FATAL_ERROR "dependencies.lock.json must use schema 1")
    endif()
    string(
        JSON required_revision
        ERROR_VARIABLE lock_error
        GET "${lock_json}" dependencies CoreCpp revision
    )
    string(LENGTH "${required_revision}" required_revision_length)
    if(
        lock_error
        OR NOT required_revision_length EQUAL 40
        OR NOT required_revision MATCHES "^[0-9a-f]+$"
    )
        message(FATAL_ERROR "dependencies.lock.json must pin a full CoreCpp Git revision")
    endif()

    set(MC_REQUIRED_CORECPP_REVISION "${required_revision}" PARENT_SCOPE)
    set(MC_CORECPP_REVISION "${CoreCpp_REVISION}" PARENT_SCOPE)
    set(MC_CORECPP_DIRTY "${CoreCpp_DIRTY}" PARENT_SCOPE)
    set(MC_CORECPP_REVISION_EXACT "${CoreCpp_REVISION_EXACT}" PARENT_SCOPE)

    if(MC_ALLOW_INEXACT_CORECPP)
        message(STATUS "MinecraftClone: inexact CoreCpp provenance allowed for local development")
        return()
    endif()

    if(NOT CoreCpp_REVISION_EXACT OR CoreCpp_DIRTY OR CoreCpp_REVISION STREQUAL "unavailable")
        message(FATAL_ERROR "Exact MinecraftClone builds require a clean, known CoreCpp revision")
    endif()
    string(LENGTH "${CoreCpp_REVISION}" corecpp_revision_length)
    if(NOT corecpp_revision_length EQUAL 40 OR NOT CoreCpp_REVISION MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "CoreCpp_REVISION is not a full lowercase Git commit")
    endif()
    if(NOT CoreCpp_REVISION STREQUAL required_revision)
        message(
            FATAL_ERROR
            "CoreCpp revision mismatch: required ${required_revision}, found ${CoreCpp_REVISION}"
        )
    endif()
endfunction()
